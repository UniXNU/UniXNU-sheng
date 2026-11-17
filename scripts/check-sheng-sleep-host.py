#!/usr/bin/env python3
"""Host tests of sheng's platform sleep wake sources (package/tests/sheng_sleep_host.c, package/ShengSleep.h)."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'sheng-sleep-host'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                    str(PACKAGE / 'tests/sheng_sleep_host.c'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
if result.returncode != 0:
    raise SystemExit('FAIL: sheng sleep host tests')
print(result.stdout.strip())
print('PASS: sheng sleep host tests')
