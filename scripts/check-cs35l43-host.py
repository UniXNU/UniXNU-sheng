#!/usr/bin/env python3
"""Host tests of the CS35L43 amplifier programs (package/tests/cs35l43_host.c). Model tests only."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'cs35l43-host'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                    str(PACKAGE / 'tests/cs35l43_host.c'), str(PACKAGE / 'ShengCs35l43.c'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith('PASS') else 1)
