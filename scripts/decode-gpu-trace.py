#!/usr/bin/env python3
"""Decode gpu.trace.bin and gpu.pixels.bin from read-sheng-log.py's diag/files.

The JSONL contains kernel uptime microseconds, process-local handles and explicit
links. It does not infer a GPU dependency from adjacent timestamps. Pixel previews
show sampled texels; .raw and .json retain the original bytes and format.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zlib

HEADER = struct.Struct('<IIIIQQ')
RECORD = struct.Struct('<QQII12Q')
PIXEL = struct.Struct('<IIII5Q4IQ6IQ')
FIELDS = {
 1: ('config', 'kind table_mode preemption input_bits output_bits priority_mode'),
 3: ('kernel_build', 'source_id'),
 10: ('submit', 'fence queue ring priority role loaded switching context submitted_us buffers'),
 11: ('retire', 'fence queue ring submitted_us gpu_start_ticks gpu_end_ticks stamped replays'),
 12: ('switch', 'phase ring target_or_fence first_fence_or_slice owner priority ttbr'),
 13: ('recovery', 'phase reason_or_ring ring_or_fence issued'),
 14: ('fence_query', 'reason issued prefix ring0 ring1 rings'),
 15: ('import', 'live address bytes handle physical'),
 20: ('display', 'request phase pointer surface_object physical slot shown pending queued'),
 21: ('display_surface', 'request physical width height stride format allocation mapped_bytes'),
 22: ('display_fence', 'request reason target submitted retired elapsed_ns ring0 ring1 query_reason'),
 23: ('display_latch', 'request flush elapsed_ns shown pending'),
 24: ('display_pixel', 'request result physical format bytes fence gpu_complete latched'),
 30: ('command', 'command runtime phase status passes commit handle'),
 31: ('surface', 'command texture surface address bytes width height stride format phase usage offset'),
 32: ('event', 'command event_handle value signal'),
 33: ('attachment', 'command pass attachment texture actual_texture actual_resolve original_samples samples load store actual_load actual_store'),
 34: ('attachment_format', 'command pass attachment resolve format resolve_format swizzle copies single_sample level slice depth_plane'),
 35: ('texture', 'texture parent format swizzle samples type usage surface_format'),
 36: ('pass', 'vk_command pass mode width height colors samples flags format swap draws recorded_ns'),
 37: ('gmem', 'vk_command site view image format linear mutable swap view_swap view_mutable'),
 38: ('vk_submit', 'vk_submit fence first_buffer buffers busy space_wait_us waits signals'),
 39: ('runtime', 'runtime stage argument'),
 40: ('pixel', 'command stage result surface format bytes flags'),
 41: ('dropped', 'count'),
 42: ('vk_command', 'vk_submit vk_command index count'),
 43: ('vk_sync', 'vk_submit signal sync state fence value'),
 45: ('runtime_vk', 'runtime vk_command batch'),
 46: ('process', ''), 47: ('metal_build', ''),
 48: ('metal_config', 'single_sample pixels'),
 49: ('texture_create', 'texture original_samples samples original_format format width height swizzle plane'),
 50: ('vk_begin', 'vk_command'),
 51: ('metal_fence', 'command fence stages update'),
 52: ('cpu_event', 'event_handle value operation timeout_ms result'),
 53: ('unsupported_shared_event', 'event_handle'),
 55: ('priority_request', 'queue requested background foreground effective_background accepted'),
 56: ('colour_description', 'surface capture kind characters'),
 57: ('colour_hash', 'surface capture kind bytes'),
 58: ('turnip_build', ''), 59: ('turnip_debug', ''),
}
PIXEL_FIELDS = 'magic version pid stage capture us command surface physical width height format colour_space source_stride grid_width grid_height row_bytes bytes_per_pixel flags bytes fence'.split()


def records(blob):
    if len(blob) < HEADER.size:
        raise ValueError('truncated GPU trace header')
    magic, version, size, count, issued, dropped = HEADER.unpack_from(blob)
    if (magic, version, size) != (0x31545047, 1, RECORD.size):
        raise ValueError('unsupported GPU trace ABI')
    if count > 118784 or len(blob) != HEADER.size + count * size:
        raise ValueError('invalid GPU trace length')
    seen = set()
    rows = []
    for offset in range(HEADER.size, len(blob), size):
        seq, us, event, pid, *values = RECORD.unpack_from(blob, offset)
        key = (seq, us, event, pid, *values)
        if key in seen:
            continue
        seen.add(key)
        name, names = FIELDS.get(event, ('unknown', ''))
        row = dict(sequence=seq, us=us, event=event, name=name, pid=pid,
                   fields=dict(zip(names.split(), values)), raw=values)
        if event in (46, 47, 58, 59):
            row['text'] = struct.pack('<12Q', *values).split(b'\0')[0].decode('utf-8', errors='replace')
        if event == 56:
            row['text'] = struct.pack('<8Q', *values[4:]).split(b'\0')[0].decode('utf-8', errors='replace')
        if event == 57:
            row['sha256'] = struct.pack('<4Q', *values[4:8]).hex()
        rows.append(row)
    rows.sort(key=lambda row: (row['us'], row['sequence']))
    return dict(issued=issued, dropped=dropped, records=len(rows)), rows


def links(rows):
    """Use explicit identities; pointer reuse is bounded by begin and submit records."""
    runtime, commands, vk, pending, imports = {}, {}, {}, {}, {}
    result = []
    passes, surfaces = {}, {}
    for row in rows:
        f, pid, event = row['fields'], row['pid'], row['event']
        if event == 30:
            runtime[pid, f['runtime']] = f['command']
            commands[pid, f['command']] = f
        elif event == 36:
            passes.setdefault((pid, f['vk_command']), []).append(f['pass'])
        elif event == 50:
            passes[pid, f['vk_command']] = []
            vk.pop((pid, f['vk_command']), None)
        elif event == 45:
            vk[pid, f['vk_command']] = f['runtime']
        elif event == 42:
            if f['index'] == 0:
                pending[pid, f['vk_submit']] = []
            pending.setdefault((pid, f['vk_submit']), []).append(
                dict(vk_command=f['vk_command'], runtime=vk.get((pid, f['vk_command'])),
                     passes=list(passes.get((pid, f['vk_command']), []))))
        elif event == 38:
            for item in pending.get((pid, f['vk_submit']), []):
                result.append(dict(kind='command_fence', pid=pid, us=row['us'], fence=f['fence'],
                                   command=runtime.get((pid, item['runtime'])), **item))
            # A split submission has more chunks, so keep the list until its submit pointer is reused.
        elif event == 15:
            key = pid, f['handle']
            if f['live']:
                imports[key] = f
            else:
                old = imports.pop(key, None)
                if old:
                    surfaces.pop((pid, old['address']), None)
        elif event == 31 and f['surface']:
            surfaces[pid, f['address']] = f['surface']
            physical = [i['physical'] + f['address'] - i['address'] for (owner, _), i in imports.items()
                        if owner == pid and i['address'] <= f['address'] < i['address'] + i['bytes']]
            result.append(dict(kind='surface_use', pid=pid, us=row['us'], **f,
                               physical=physical[0] if len(set(physical)) == 1 else None))
        elif event == 21:
            ids = {surfaces.get((owner, imp['address'])) for (owner, _), imp in imports.items()
                   if imp['physical'] == f['physical']}
            ids.discard(None)
            result.append(dict(kind='display_surface', us=row['us'], **f,
                               surface=next(iter(ids)) if len(ids) == 1 else None))
    return result


def png(path, width, height, pixels):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    rows = b''.join(b'\0' + pixels[y*width*4:(y+1)*width*4] for y in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def pixels(blob, out):
    if len(blob) < 16:
        raise ValueError('truncated pixel collection')
    magic, version, count, skipped = struct.unpack_from('<4I', blob)
    if (magic, version) != (0x31535047, 1) or count > 24:
        raise ValueError('unsupported pixel collection ABI')
    at = 16
    manifest = []
    for _ in range(count):
        if at + PIXEL.size > len(blob):
            raise ValueError('truncated pixel header')
        h = dict(zip(PIXEL_FIELDS, PIXEL.unpack_from(blob, at))); at += PIXEL.size
        n, w, height, bpp = h['bytes'], h['grid_width'], h['grid_height'], h['bytes_per_pixel']
        if (h['magic'], h['version']) != (0x31585047, 1) or not h['flags'] & 1 or not w or not height or \
                bpp not in (4, 8) or n > 65536 or h['row_bytes'] != w*bpp or n != w*height*bpp or \
                w > h['width'] or height > h['height'] or at+n > len(blob):
            raise ValueError('invalid pixel dimensions or completion flag')
        raw = blob[at:at+n]; at += n
        stem = f"pixel-{h['pid']}-{h['stage']}-{h['capture']}"
        h['sha256'] = hashlib.sha256(raw).hexdigest()
        h['preview'] = None
        (out / (stem + '.raw')).write_bytes(raw)
        is_metal = bool(h['flags'] & 8)
        bgra = h['format'] in ((80, 81) if is_metal else (0x42475241,))
        rgba = h['format'] in ((70, 71) if is_metal else (0x52474241,))
        if bpp == 4 and (bgra or rgba):
            preview = bytearray(raw)
            if bgra:
                for i in range(0, n, 4):
                    preview[i], preview[i+2] = preview[i+2], preview[i]
            if h['stage'] == 3:
                preview[3::4] = bytes([255])*(w*height)
                h['preview_alpha'] = 'opaque display; raw alpha preserved'
            png(out / (stem + '.png'), w, height, preview)
            h['preview'] = stem + '.png'
        elif bpp == 8 and is_metal and h['format'] == 115:
            import math
            preview = bytearray()
            for i, (value,) in enumerate(struct.iter_unpack('<e', raw)):
                value = max(0., min(1., value)) if math.isfinite(value) else 0.
                if i % 4 != 3:
                    value = value*12.92 if value <= .0031308 else 1.055*value**(1/2.4)-.055
                preview.append(round(value*255))
            png(out / (stem + '.png'), w, height, preview)
            h['preview'] = stem + '.png'
            h['preview_transform'] = 'linear half float clipped to 0..1 and encoded as sRGB; raw preserved'
        (out / (stem + '.json')).write_text(json.dumps(h, indent=2)+'\n')
        manifest.append(h)
    if at != len(blob):
        raise ValueError('trailing pixel collection bytes')
    return dict(captures=manifest, skipped=skipped)


def decode(directory, out):
    out.mkdir(parents=True, exist_ok=True)
    summary = {}
    rows = []
    trace = directory / 'gpu.trace.bin'
    if trace.exists():
        summary['trace'], rows = records(trace.read_bytes())
        (out / 'events.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in rows))
        (out / 'links.jsonl').write_text(''.join(json.dumps(row)+'\n' for row in links(rows)))
    sample = directory / 'gpu.pixels.bin'
    if sample.exists():
        summary['pixels'] = pixels(sample.read_bytes(), out)
        for capture in summary['pixels']['captures']:
            colour = [r for r in rows if r['event'] in (56, 57) and r['pid'] == capture['pid'] and
                      r['fields']['capture'] == capture['capture'] and r['fields']['surface'] == capture['surface']]
            capture['surface_colour'] = colour
            stem = f"pixel-{capture['pid']}-{capture['stage']}-{capture['capture']}"
            (out / (stem + '.json')).write_text(json.dumps(capture, indent=2)+'\n')
    (out / 'summary.json').write_text(json.dumps(summary, indent=2)+'\n')
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    result = decode(args.directory, args.out or args.directory.parent / 'gpu')
    print(json.dumps(result.get('trace', {})))
