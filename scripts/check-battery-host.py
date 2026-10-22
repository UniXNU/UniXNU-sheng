#!/usr/bin/env python3
"""Host tests of the battery manager codec, refresh cycle and power source mapping (package/tests/battmgr_host.c). Model tests only."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'battmgr-host'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                    str(PACKAGE / 'tests/battmgr_host.c'), str(PACKAGE / 'ShengBattmgr.c'), '-o', str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip()
print(out)
sys.exit(0 if out.startswith('PASS') else 1)
