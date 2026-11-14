#!/usr/bin/env bash
# Run one timed image, retrieve its Linux log and confirm the final fastboot state.
# Usage: SERIAL=<serial> READER=<reader.img> [OUT=<directory>] run-sheng-unattended.sh IMAGE [wait_seconds=600]
set -euo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 - "$ROOT" "$@" <<'PY'
import os
from pathlib import Path
import subprocess
import sys
import time

root = Path(sys.argv[1])
if len(sys.argv) not in (3, 4):
    sys.exit('usage: run-sheng-unattended.sh IMAGE [wait_seconds=600]')
image = Path(sys.argv[2]).resolve()
limit = int(sys.argv[3]) if len(sys.argv) == 4 else 600
serial = os.environ.get('SERIAL')
reader = Path(os.environ.get('READER', root / 'out/sheng/boot/sheng-linux-log.img')).resolve()
output = Path(os.environ.get('OUT', root / 'build/sheng-logs')).resolve()
if not image.is_file() or not reader.is_file() or limit <= 0:
    sys.exit('require existing test and reader images and a positive timeout')

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
            return
        time.sleep(2)
    raise SystemExit(f'{serial}: fastboot did not appear within {seconds}s; evidence retained in {output}')

def boot(path):
    subprocess.run(['fastboot', '-s', serial, 'boot', str(path)], check=True, timeout=120)

output.mkdir(parents=True, exist_ok=True)
wait_fastboot(30)
boot(image)
time.sleep(2)
wait_fastboot(limit)
boot(reader)
command = [sys.executable, str(root / 'scripts/read-sheng-log.py'), '--wait', '120', '--out', str(output)]
if os.environ.get('PORT'):
    command += ['--port', os.environ['PORT']]
try:
    result = subprocess.run(command, timeout=270).returncode
except subprocess.TimeoutExpired:
    result = 124
    print('log retrieval exceeded 270s; waiting for the reader return timer', flush=True)
wait_fastboot(330)
print(f'{serial}: returned to fastboot; log reader exit={result}', flush=True)
sys.exit(result)
PY
