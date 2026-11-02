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
 60: ('wallpaper_freeze', 'phase command surface frozen_command frozen_us allowed parked'),
 61: ('encode_batch', 'batch phase queued_us tasks depth thread'),
 62: ('frontend_stage', 'command commit phase batch thread surfaces bytes'),
 63: ('thread', 'thread role name0 name1 name2 name3'),
 64: ('cpu_detail','operation kind phase command object function parent thread began_us elapsed_us line queued_us'),
 67: ('cpu_definition','key part text0 text1 text2 text3'),
 68: ('dispatch','item phase queue function parent context'),
}
LATENCY_RECORD = struct.Struct('<QQII6Q')
LATENCY_FIELDS = {
 1: ('ipc_send_attempt', 'cookie message receiver task_pid destination_task destination_port'),
 2: ('ipc_receive', 'cookie message receiver task_pid destination_task destination_port'),
 3: ('user_exception', 'exception code subcode pc lr sp'),
 4: ('gpu_vote', 'previous_level requested_level reason busy_percent needed_mhz accepted_level'),
 5: ('touch_publish', 'report contacts index tracking_id x_tenth_pixel y_tenth_pixel'),
 6: ('fault_state','fault fp sp far esr shared_cache_slide'),
 7: ('fault_registers','fault first x0 x1 x2 x3'),
 8: ('fault_frames','fault first pc0 pc1 pc2 pc3'),
 9: ('fault_images','fault shared_cache_slide dyld_info version images main_image'),
 10: ('fault_stack','fault first word0 word1 word2 word3'),
 11: ('fault_codes','fault exception code subcode pc lr'),
 12: ('fault_result','fault flags error frames delay_us stack_error'),
 13: ('ipc_ports','cookie phase sender_task destination_task destination_port reply_port'),
 14: ('task_identity','task task_pid name0 name1 name2 name3'),
 16: ('gpu_counter_config','counter select_reg countable low_reg generation selector_readback'),
 17: ('gpu_counter_values','sample first v0 v1 v2 v3'),
 18: ('gpu_counter_interval','sample began_us ended_us busy_ticks flags epoch'),
 19: ('gpu_counter_validity','sample selector_mismatch unstable reset epoch_before epoch_after'),
 20: ('dispatch','item phase queue function parent context'),
 22: ('signal_exit','subject_pid status reason_namespace reason_code current_thread_owner unused'),
 23: ('signal_description','subject_pid status part text0 text1 text2'),
 24: ('exit_process_name','subject_pid unused0 unused1 text0 text1 text2'),
 25: ('fault_cache_uuid','fault cache_base uuid0 uuid1 main_image unused'),
 26: ('fault_image_uuid','fault image uuid0 uuid1 index unused'),
 27: ('fault_uuid_result','fault total captured error limit unused'),
 28: ('thread_wait','capture state wait pc lr switches'),
 29: ('thread_wait_frames','capture first pc0 pc1 pc2 pc3'),
 30: ('thread_wait_peer','capture peer_pid peer_status stack_error unused0 unused1'),
 31: ('thread_wait_coverage','subject_pid visited recorded omitted lock_busy capacity'),
 32: ('thread_wait_kernel','capture first pc0 pc1 pc2 pc3'),
}
RUNTIME_STAGES = {40:'memory_begin',41:'memory_pool',42:'memory_kib',43:'available_kib',
 44:'allocated',45:'reused',46:'memory_low',47:'memory_high',48:'cached_kib',49:'deferred_kib',
 50:'free_begin',51:'free_end',52:'release_queue_full',53:'complete_begin',54:'complete_state',
 55:'complete_handlers',56:'complete_retained',57:'complete_end'}

CPU_DETAIL_KINDS=dict(enumerate(('encoding','closure_release','completed_callback','buffer','texture','owner_deallocator','surface_release','function','library','compute_pipeline','render_pipeline','sampler','depth_stencil','event','fence','encoder','command_buffer','queue','heap','binary_archive','memory_owner'),1))
FRAME_RECORD=struct.Struct('<II4Q8I')
def frontend_details(blob):
    if len(blob)%FRAME_RECORD.size:raise ValueError('truncated frontend stream')
    rows=[]
    for at in range(0,len(blob),FRAME_RECORD.size):
        kind,seq,t0,t1,t2,t3,a,b,c,d,tid,cpu0,cpu1,tid2=FRAME_RECORD.unpack_from(blob,at)
        if kind==17:
            rows.append(dict(name='cpu_detail',sequence=seq,us=t0,operation=t1,command=t2,object=t3,
                             kind=a&65535,detail_name=CPU_DETAIL_KINDS.get(a&65535,'unknown'),phase=a>>16,thread=b,function=c|(d<<32),elapsed_us=cpu0,line=cpu1))
        elif kind==18:rows.append(dict(name='cpu_detail_link',us=t0,operation=t1,parent=t2,queued_us=t3,began_us=a|(b<<32)))
        elif kind==22:rows.append(dict(name='runtime_stage_wide',us=t0,runtime=t1,stage=a,argument=b))
        elif kind==19:
            raw=struct.pack('<3Q2I',t1,t2,t3,b,c)
            rows.append(dict(name='cpu_definition',key=t0,part=a,text=raw.split(b'\0')[0].decode('utf-8',errors='replace')))
    return rows

GPU_COUNTER_NAMES=('cp_clocks','uche_arbiter_stall','uche_latency_cycles','uche_latency_samples',
 'uche_tp_read_beats','uche_sp_read_beats','uche_tp_requests','tp_busy','tp_l1_requests','tp_l1_misses',
 'sp_busy','sp_alu_working','sp_tp_stall','sp_fs_full_alu','sp_icache_requests','sp_icache_misses',
 'sp_pixels','cmp_read_data','cmp_write_data')

def latency_analysis(rows):
    queues,running,dispatch,ipc_pending,ipc,reply_ports={}, {}, [], {}, [], {}
    counter_parts,previous,counters={},None,[]
    faults={};waits={};coverage=[]
    for row in rows:
        kind,pid,us,f=row['kind'],row['pid'],row['us'],row['fields']
        if kind==1:
            ipc_pending[f['cookie']]=dict(pid=pid,us=us,**f)
            request=reply_ports.pop(f['destination_port'],None)
            if request:ipc.append(dict(reply_cookie=f['cookie'],request_cookie=request,reply_port=f['destination_port']))
        elif kind==2:
            sent=ipc_pending.pop(f['cookie'],None)
            ipc.append(dict(cookie=f['cookie'],message=f['message'],receive_us=us,receiver_pid=pid,
                            send_us=sent['us'] if sent and sent['message']==f['message'] else None))
        elif kind==13 and f['phase']==1:
            request=reply_ports.pop(f['destination_port'],None)
            if request:ipc.append(dict(reply_cookie=f['cookie'],request_cookie=request,reply_port=f['destination_port']))
            if f['reply_port']:reply_ports[f['reply_port']]=f['cookie']
        elif kind==20:
            phase=f['phase']&255;key=pid,f['item']
            if phase==1:
                queues[key]=dict(pid=pid,item=f['item'],queued_us=us,parent=f['parent'],function=f['function'],queue=f['queue'])
            elif phase==2:
                item=queues.pop(key,dict(pid=pid,item=f['item'],queued_us=None,parent=f['parent'],function=f['function'],queue=f['queue']))
                item.update(started_us=us,thread=row['thread']);running[key]=item
            elif phase==3:
                item=running.pop(key,None)
                if item:
                    item['ended_us']=us;item['run_us']=us-item['started_us']
                    item['queue_us']=item['started_us']-item['queued_us'] if item['queued_us'] is not None else None
                    dispatch.append(item)
        elif kind==17:
            part=counter_parts.setdefault(f['sample'],dict(values={}))
            part['values'].update((f['first']+i,f['v'+str(i)]) for i in range(4) if f['first']+i<len(GPU_COUNTER_NAMES))
        elif kind==18:counter_parts.setdefault(f['sample'],dict(values={}))['interval']=f
        elif kind==19:
            part=counter_parts.pop(f['sample'],dict(values={}));interval=part.get('interval')
            complete=interval and len(part['values'])==len(GPU_COUNTER_NAMES)
            result=dict(sample=f['sample'],scope='global_br',valid=False,validity=f,interval=interval)
            if complete and previous and previous['valid_end']:
                valid=(not interval['flags'] and not (f['selector_mismatch']|f['unstable']|f['reset']) and
                       f['epoch_before']==previous['epoch'] and f['epoch_after']==f['epoch_before'])
                elapsed=interval['ended_us']-previous['ended_us']
                deltas=[part['values'][i]-previous['values'][i] for i in range(len(GPU_COUNTER_NAMES))]
                valid=valid and elapsed>0 and min(deltas)>=0
                if valid:
                    d=dict(zip(GPU_COUNTER_NAMES,deltas));result.update(valid=True,elapsed_us=elapsed,delta=d,
                        cp_clocks_per_us=d['cp_clocks']/elapsed,tp_sp_read_bytes=32*(d['uche_tp_read_beats']+d['uche_sp_read_beats']))
            if complete:previous=dict(values=part['values'],epoch=f['epoch_after'],ended_us=interval['ended_us'],
                                      valid_end=not(interval['flags']&~1) and not(f['selector_mismatch']|f['unstable']))
            else:previous=None
            counters.append(result)
        elif kind in (6,7,8,9,10,11,12,25,26,27):
            fault=faults.setdefault((pid,f['fault']),dict(pid=pid,fault=f['fault'],registers={},frames={},stack={}))
            if kind in (7,8,10):
                target= fault['registers' if kind==7 else 'frames' if kind==8 else 'stack']
                prefix='x' if kind==7 else 'pc' if kind==8 else 'word'
                target.update((f['first']+i,f[prefix+str(i)]) for i in range(4))
            elif kind==26:fault.setdefault('images',[]).append(dict(**f,uuid=row.get('uuid')))
            elif kind==25:fault[row['name']]=dict(**f,uuid=row.get('uuid'))
            else:fault[row['name']]=f
        elif kind in (28,29,30,32):
            wait=waits.setdefault((pid,f['capture']),dict(pid=pid,thread=row['thread'],capture=f['capture'],frames={},kernel={}))
            if kind in (29,32):wait['frames' if kind==29 else 'kernel'].update((f['first']+i,f['pc'+str(i)]) for i in range(4))
            else:wait[row['name']]=f
        elif kind==31:coverage.append(dict(us=us,**f))
    return dict(ipc=ipc,dispatch=dispatch,gpu_counters=counters,faults=list(faults.values()),
                pending_dispatch=len(queues),running_dispatch=len(running),waits=list(waits.values()),wait_coverage=coverage)

def latency_records(blob):
    if len(blob)<16:raise ValueError('short latency header')
    magic,total,kept,dropped=struct.unpack_from('<4I',blob)
    if magic!=0x3154414c or kept>524288 or len(blob)!=16+kept*LATENCY_RECORD.size:
        raise ValueError('invalid latency stream')
    rows=[]
    for at in range(16,len(blob),LATENCY_RECORD.size):
        us,tid,kind,pid,*values=LATENCY_RECORD.unpack_from(blob,at)
        name,names=LATENCY_FIELDS.get(kind,('unknown',''))
        row=dict(us=us,thread=tid,kind=kind,pid=pid,name=name,fields=dict(zip(names.split(),values)))
        if kind not in LATENCY_FIELDS:row['raw']=values
        if kind in (23,24):row['text']=struct.pack('<3Q',*values[3:]).split(b'\0')[0].decode('utf-8',errors='replace')
        if kind in (25,26):row['uuid']=struct.pack('<2Q',*values[2:4]).hex()
        if kind==14:row['text']=struct.pack('<4Q',*values[2:]).split(b'\0')[0].decode('utf-8',errors='replace')
        if kind==20:
            row['phase']=values[1]&255
            row['flags']=values[1]>>8
        rows.append(row)
    rows.sort(key=lambda r:r['us'])
    return dict(total=total,kept=kept,dropped=dropped,overwritten=max(0,total-524288)),rows

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
        if event == 39:
            row['stage_name'] = RUNTIME_STAGES.get(values[1], str(values[1]))
        if event==67:
            row['text']=struct.pack('<4Q',*values[2:6]).split(b'\0')[0].decode('utf-8',errors='replace')
        if event == 63:
            row['text'] = struct.pack('<4Q', *values[2:6]).split(b'\0')[0].decode('utf-8', errors='replace')
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
