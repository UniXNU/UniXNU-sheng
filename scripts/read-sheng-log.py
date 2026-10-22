#!/usr/bin/env python3
"""Fetch the UniXNU diagnostic log (package/BootLog.c) from the sheng log reader (package/linux-log) over its USB CDC ACM shell and
decode it; then, unless --no-reboot, run /bin/rebootbl so the tablet returns to fastboot.

usage: read-sheng-log.py [--port /dev/cu.usbmodemXXXX] [--out build/sheng-logs] [--no-reboot] [--wait 120]
       read-sheng-log.py --decode log.bin [--out DIR]       (decode a saved window only)
Output: DIR/<tag>-<boot id>/ log.bin, header.txt, msgbuf.txt, slots.txt (every slot of the run, oldest first) and latest.txt; with a
header that names an export, also export.bin and export/ (the render server's pipeline-cache directory, files plus manifest.json). A version
3 export lies in the region below the log window and is fetched from the reader on its own; --decode reports it but cannot unpack it."""
import argparse
import base64
import glob
import gzip
import hashlib
import json
import os
import re
import select
import struct
import sys
import termios
import time
import tty
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BLOCK = 4096
HEADER = struct.Struct('<8sIIQQ8I6I32s')
FIELDS = ('magic', 'version', 'header_bytes', 'boot_id', 'first_lba', 'window_blocks', 'msg_first', 'msg_blocks', 'msg_bytes',
          'slot_first', 'slot_blocks', 'slot_count', 'slots_written', 'uptime_s', 'rounds', 'write_errors', 'gpt_open', 'gpt_last',
          'state', 'tag')
EXPORT = struct.Struct('<4I')   # version 2: export_first, export_blocks, export_bytes, export_generation
EXPORT_FIELDS = ('export_first', 'export_blocks', 'export_bytes', 'export_generation')
EXPORT3 = struct.Struct('<I')   # version 3: export_below (the export region ends that many blocks below the window)
DIAG4 = struct.Struct('<II')    # version 4: diag_first, diag_bytes (package/BootDiagExport.c, in the same region)
LOG_BLOCKS = 8192               # the window the reader dumps (package/linux-log/init, NativeUfs kLogReserveBlocks)


def unpack_export(blob, run, name='export'):
    """The export area's blob (package/BootLog.c, drivers/NativeMetal/CacheExport.h) into run/<name>/: the files and a manifest. The
    diagnostic export (name diag) has the same form with root /."""
    (run / f'{name}.bin').write_bytes(blob)
    raw_bytes, zbytes = struct.unpack_from('<II', blob, 8)
    if blob[:8] == b'SHEXP001':
        raw = zlib.decompress(blob[16:16 + zbytes])
    elif blob[:8] == b'SHEXR001':   # version 3: the stream as the plugin read it
        raw = blob[16:16 + raw_bytes]
    else:
        raise SystemExit('export holds no SHEXP001/SHEXR001 blob')
    if len(raw) != raw_bytes or raw[:8] != b'M2VCACH1':
        raise SystemExit('export blob is corrupt')
    at = 8
    (n,) = struct.unpack_from('<I', raw, at); at += 4
    root = raw[at:at + n].decode(); at += n
    (count,) = struct.unpack_from('<I', raw, at); at += 4
    out = run / name
    files = []
    for _ in range(count):
        plen, mode, uid, gid, size = struct.unpack_from('<IIIIQ', raw, at); at += 24
        rel = raw[at:at + plen].decode(); at += plen
        data = raw[at:at + size]; at += size
        if rel.startswith('/') or '..' in Path(rel).parts:
            raise SystemExit('export path escapes its directory: ' + rel)
        dest = out / 'files' / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        files.append(dict(path=rel, mode=mode, uid=uid, gid=gid, size=size, sha256=hashlib.sha256(data).hexdigest()))
    if at != len(raw):
        raise SystemExit('export blob has trailing bytes')
    (out / 'manifest.json').write_text(json.dumps(dict(directory=root, files=files, blob_sha256=hashlib.sha256(blob).hexdigest()), indent=1) + '\n')
    print(f'{out}: {count} files, {sum(f["size"] for f in files)} bytes from {root}')


def header(blob):
    if len(blob) < BLOCK:
        raise SystemExit(f'log window too short ({len(blob)} bytes)')
    h = dict(zip(FIELDS, HEADER.unpack_from(blob)))
    if h['version'] >= 2:
        h.update(zip(EXPORT_FIELDS, EXPORT.unpack_from(blob, HEADER.size)))
    if h['version'] >= 3:
        (h['export_below'],) = EXPORT3.unpack_from(blob, HEADER.size + EXPORT.size)
    if h['version'] >= 4:
        h['diag_first'], h['diag_bytes'] = DIAG4.unpack_from(blob, HEADER.size + EXPORT.size + EXPORT3.size)
    return h


def decode(blob, out):
    h = header(blob)
    if h['magic'] != b'SHLOG001':
        raise SystemExit(f'no log header (magic {h["magic"]!r}): nothing was written, or the window is not the log')
    tag = h['tag'].split(b'\0')[0].decode(errors='replace') or 'untagged'
    run = out / f'{tag}-{h["boot_id"]:x}'
    run.mkdir(parents=True, exist_ok=True)
    (run / 'log.bin').write_bytes(blob)
    lines = [f'{k}={v.split(chr(0))[0] if isinstance(v, str) else (v.split(b"\0")[0].decode(errors="replace") if isinstance(v, bytes) else v)}'
             for k, v in h.items()]
    (run / 'header.txt').write_text('\n'.join(lines) + '\n')
    msg = blob[h['msg_first'] * BLOCK:(h['msg_first'] + h['msg_blocks']) * BLOCK][:h['msg_bytes']]
    (run / 'msgbuf.txt').write_bytes(msg)
    count, written = h['slot_count'], h['slots_written']
    first = max(0, written - count)
    slots = []
    for n in range(first, written):
        at = (h['slot_first'] + (n % count) * h['slot_blocks']) * BLOCK
        slots.append(blob[at:at + h['slot_blocks'] * BLOCK].split(b'\0')[0].decode(errors='replace'))
    (run / 'slots.txt').write_text('\n'.join(slots))
    (run / 'latest.txt').write_text(slots[-1] if slots else '')
    if h.get('export_bytes') and not h.get('export_below'):
        start = h['export_first'] * BLOCK
        unpack_export(blob[start:start + h['export_bytes']], run)
    elif h.get('export_bytes'):
        print(f'export generation {h["export_generation"]}: {h["export_bytes"]} bytes below the window (fetched separately)')
    print(f'{run}: tag={tag} uptime={h["uptime_s"]}s slots={written} (kept {len(slots)}) rounds={h["rounds"]} '
          f'errors={h["write_errors"]} gpt={h["gpt_open"]:x}/{h["gpt_last"]:x} msgbuf={len(msg)} bytes')
    return run


class Port:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        attrs[2] |= termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def send(self, text):
        os.write(self.fd, text.encode())

    def read_until(self, marker, timeout):
        data, end = bytearray(), time.time() + timeout
        while marker not in data:
            if time.time() > end:
                raise SystemExit(f'timed out waiting for {marker!r}; last output: {bytes(data[-300:])!r}')
            ready, _, _ = select.select([self.fd], [], [], 0.5)
            if ready:
                chunk = os.read(self.fd, 1 << 16)
                if not chunk:
                    raise SystemExit('serial port closed')
                data += chunk
        return bytes(data)


def find_port(wait):
    end = time.time() + wait
    while time.time() < end:
        ports = sorted(glob.glob('/dev/cu.usbmodem*'))
        if ports:
            return ports[0]
        time.sleep(1)
    raise SystemExit('no /dev/cu.usbmodem* port: is the log reader booted?')


def fetch(port_path, out, reboot, retries=6):
    port = Port(port_path)
    time.sleep(0.5)
    port.send('\n')
    time.sleep(0.5)
    port.send('stty -echo 2>/dev/null; echo R$((40+2))R\n')
    port.read_until(b'R42R', 30)

    # The markers are computed by the shell, so the echoed command (if echo is still on) never contains them. '@' is not a base64 character.
    def run(command, timeout=120):
        port.send('echo @@B$((40+2))@@; ' + command + '; echo @@E$((40+2))@@\n')
        data = port.read_until(b'@@E42@@', timeout)
        return data[data.rindex(b'@@B42@@') + 7:data.rindex(b'@@E42@@')].strip()

    # One gzip of a file on the reader, then 64 KiB pieces, each with its own MD5 and retried alone: a single 1.7 MB base64 stream lost or
    # gained characters on the serial line often enough that int-146's four whole-stream attempts all failed.
    def pull(path, timeout=120):
        summary = run(f'gzip -c {path} > /run/l.gz; wc -c < /run/l.gz; md5sum /run/l.gz', timeout).split()
        total, want = int(summary[0]), summary[1].decode()
        piece, data = 65536, bytearray()
        for n in range((total + piece - 1) // piece):
            for attempt in range(retries):
                body = run(f'dd if=/run/l.gz bs={piece} skip={n} count=1 2>/dev/null | md5sum; dd if=/run/l.gz bs={piece} skip={n} count=1 2>/dev/null | base64', 60)
                first, _, rest = body.partition(b'\n')
                try:
                    chunk = base64.b64decode(re.sub(rb'[^A-Za-z0-9+/=]', b'', rest))
                except Exception:
                    chunk = b''
                if first.split() and hashlib.md5(chunk).hexdigest() == first.split()[0].decode():
                    break
                print(f'piece {n} corrupt; retrying', file=sys.stderr)
            else:
                raise SystemExit(f'piece {n} failed {retries} times')
            data += chunk
        if len(data) != total or hashlib.md5(data).hexdigest() != want:
            raise SystemExit('transfer does not match the reader\'s MD5')
        return bytes(data)

    data = pull('/run/log.bin')
    (out / 'last-raw.gz').parent.mkdir(parents=True, exist_ok=True)
    (out / 'last-raw.gz').write_bytes(data)
    window = gzip.decompress(data)
    run_dir = decode(window, out)
    h = header(window)
    # The export region below the window, read from userdata on the reader (the same partition lookup as its init).
    def region(first, size, name):
        blocks = (size + BLOCK - 1) // BLOCK
        made = run('P=; for u in /sys/class/block/*/uevent; do grep -q "^PARTNAME=userdata$" "$u" && P=$(basename $(dirname "$u")); done; '
                   'B=$(( $(cat /sys/class/block/$P/size) / 8 )); '
                   f'dd if=/dev/$P of=/run/{name}.bin bs=4096 skip=$((B - {LOG_BLOCKS} - {h["export_below"]} + {first})) '
                   f'count={blocks} 2>/dev/null; wc -c < /run/{name}.bin', 300)
        if int(made.split()[-1]) != blocks * BLOCK:
            raise SystemExit(f'{name} region read short: {made!r}')
        unpack_export(gzip.decompress(pull(f'/run/{name}.bin', 1800))[:size], run_dir, name)
        run(f'rm -f /run/{name}.bin /run/l.gz')

    if h.get('export_bytes') and h.get('export_below'):
        region(h['export_first'], h['export_bytes'], 'export')
    if h.get('diag_bytes') and h.get('export_below'):
        region(h['diag_first'], h['diag_bytes'], 'diag')
    if reboot:
        port.send('/bin/rebootbl\n')
        print('sent rebootbl: the tablet should come back in fastboot')
    return run_dir


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port')
    ap.add_argument('--out', type=Path, default=ROOT / 'build/sheng-logs')
    ap.add_argument('--no-reboot', action='store_true')
    ap.add_argument('--wait', type=int, default=120)
    ap.add_argument('--decode', type=Path)
    a = ap.parse_args()
    if a.decode:
        decode(a.decode.read_bytes(), a.out)
        return
    fetch(a.port or find_port(a.wait), a.out, not a.no_reboot)


if __name__ == '__main__':
    main()
