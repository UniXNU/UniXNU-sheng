#!/usr/bin/env python3
"""Host tests of hotspot IE sanitization and ULA-only RA scrubbing (WifiHotspotIe.h, WifiRaFilter.h)."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'wifi-hotspot-ie-host'
    build = subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                            str(PACKAGE / 'tests/wifi_hotspot_ie_host.c'), '-o', str(exe)], capture_output=True, text=True)
    if build.returncode != 0:
        sys.stderr.write(build.stderr)
        raise SystemExit('FAIL: wifi hotspot IE / RA filter host tests (build)')
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
if result.returncode != 0:
    raise SystemExit('FAIL: wifi hotspot IE / RA filter host tests')
print(result.stdout.strip())
print('PASS: wifi hotspot IE and RA filter host tests')
