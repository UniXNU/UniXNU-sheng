#!/usr/bin/env bash
# Run one timed image, retrieve its Linux log and confirm the final fastboot state.
# Usage: SERIAL=<serial> READER=<reader.img> [USERDATA=<full.sparse>] [OUT=<directory>] run-sheng-unattended.sh IMAGE [wait_seconds=600]
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 - "$ROOT" "$@" <<'PY'
import os
import json
from pathlib import Path
import re
import secrets
import struct
import subprocess
import sys
import tempfile
import time
import tomllib

root = Path(sys.argv[1])
if len(sys.argv) not in (3, 4):
    sys.exit('usage: run-sheng-unattended.sh IMAGE [wait_seconds=600]')
image = Path(sys.argv[2]).resolve()
limit = int(sys.argv[3]) if len(sys.argv) == 4 else 600
serial = os.environ.get('SERIAL')
reader = Path(os.environ.get('READER', root / 'out/sheng/boot/sheng-linux-log.img')).resolve()
output = Path(os.environ.get('OUT', root / 'build/sheng-logs')).resolve()
userdata = Path(os.environ['USERDATA']).resolve() if os.environ.get('USERDATA') else None
if not image.is_file() or not reader.is_file() or limit <= 0:
    sys.exit('require existing test and reader images and a positive timeout')
if userdata is not None and not userdata.is_file():
    sys.exit('USERDATA must name the full-partition sparse image')
maximum = tomllib.loads((root / 'board.toml').read_text())['bootimg']['image']['ramdisk_max_bytes']
for candidate in (image, reader):
    with candidate.open('rb') as source:
        header = source.read(48)
    if len(header) < 48 or header[:8] != b'ANDROID!':
        sys.exit(f'{candidate}: require an Android boot image')
    if struct.unpack_from('<I', header, 16)[0] > maximum:
        sys.exit(f'{candidate}: encoded ramdisk exceeds the {maximum}-byte sheng limit')
if sys.platform == 'darwin':
    subprocess.Popen(['/usr/bin/caffeinate', '-is', '-w', str(os.getpid())])

def devices():
    result = subprocess.run(['fastboot', 'devices'], capture_output=True, text=True, check=True, timeout=10)
    return [line.split()[0] for line in result.stdout.splitlines() if line.split()]

if not serial:
    found = devices()
    if len(found) != 1:
        sys.exit('set SERIAL when exactly one fastboot device is not present')
    serial = found[0]

def wait_fastboot(seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if serial in devices():
            try:
                result = subprocess.run(['fastboot', '-s', serial, 'getvar', 'product'],
                                        capture_output=True, text=True, timeout=5)
                if result.returncode == 0 and re.search(r'^product:\s*sheng\s*$', result.stdout + result.stderr, re.M):
                    return
            except subprocess.TimeoutExpired:
                pass
        time.sleep(2)
    raise SystemExit(f'{serial}: fastboot did not appear within {seconds}s; evidence retained in {output}')

def boot(path):
    try:
        subprocess.run(['fastboot', '-s', serial, 'boot', str(path)], check=True, timeout=120)
    except subprocess.TimeoutExpired:
        print(f'{serial}: boot acknowledgement timed out; waiting for the image return timer', flush=True)
        return False
    return True

output.mkdir(parents=True, exist_ok=True)
previous_tags = {line[4:] for path in output.glob('*/header.txt')
                 for line in path.read_text(errors='replace').splitlines() if line.startswith('tag=')}
tag = 's' + secrets.token_hex(3)[:5]
while tag in previous_tags:
    tag = 's' + secrets.token_hex(3)[:5]
tagged_directory = tempfile.TemporaryDirectory(prefix='sheng-timed-boot-')
tagged = Path(tagged_directory.name) / image.name
subprocess.run([sys.executable, str(root / 'scripts/repack-sheng-boot.py'), str(image), str(tagged),
                '--kernel-arg', 'build-tag=' + tag], check=True)
wait_fastboot(30)
if userdata is not None:
    def variable(name):
        result = subprocess.run(['fastboot', '-s', serial, 'getvar', name],
                                capture_output=True, text=True, check=True, timeout=15)
        match = re.search(r'^' + re.escape(name) + r':\s*(\S+)', result.stdout + result.stderr, re.M)
        if not match:
            raise SystemExit('fastboot did not report ' + name)
        return match[1]
    if variable('product') != 'sheng':
        sys.exit('USERDATA requires the sheng product')
    partition = int(variable('partition-size:userdata'), 0)
    with userdata.open('rb') as source:
        header = source.read(28)
    if len(header) != 28:
        sys.exit('USERDATA has no Android sparse header')
    magic, major, _, _, _, block, blocks, _, _ = struct.unpack('<I4H4I', header)
    if magic != 0xed26ff3a or major != 1 or block * blocks != partition:
        sys.exit('USERDATA must cover the complete reported userdata partition')
    print(f'{serial}: writing the complete {partition}-byte userdata partition', flush=True)
    subprocess.run(['fastboot', '-s', serial, 'flash', 'userdata', str(userdata)], check=True, timeout=3600)
started = time.monotonic()
acknowledged = boot(tagged)
time.sleep(2)
wait_fastboot(limit)
test_seconds = time.monotonic() - started
print(f'{serial}: test image returned after {test_seconds:.3f}s; collecting its log', flush=True)
reader_started = time.monotonic()
reader_acknowledged = boot(reader)
command = [sys.executable, str(root / 'scripts/read-sheng-log.py'), '--wait', '120', '--out', str(output), '--expected-tag', tag]
if os.environ.get('PORT'):
    command += ['--port', os.environ['PORT']]
try:
    result = subprocess.run(command, timeout=270).returncode
except subprocess.TimeoutExpired:
    result = 124
    print('log retrieval exceeded 270s; waiting for the reader return timer', flush=True)
wait_fastboot(330)
header_fresh = any(('tag=' + tag) in path.read_text(errors='replace').splitlines() for path in output.glob('*/header.txt'))
checkpoint_fresh = any(('tag=' + tag) in path.read_text(errors='replace').splitlines() for path in output.glob('*/startup-checkpoint.txt'))
fresh = header_fresh or checkpoint_fresh
if result == 0 and not fresh:
    result = 3
    print(f'{serial}: the capture does not carry test tag {tag}; this boot is unqualified', flush=True)
(output / 'cycle.json').write_text(json.dumps(dict(
    image=image.name, reader=reader.name, boot_acknowledged=acknowledged,
    expected_tag=tag, fresh_log=fresh, header_fresh=header_fresh, checkpoint_fresh=checkpoint_fresh,
    test_seconds=round(test_seconds, 3), reader_acknowledged=reader_acknowledged,
    reader_seconds=round(time.monotonic() - reader_started, 3),
    reader_exit=result, final_fastboot=True), indent=2) + '\n')
print(f'{serial}: returned to fastboot; log reader exit={result}', flush=True)
tagged_directory.cleanup()
sys.exit(result)
PY
