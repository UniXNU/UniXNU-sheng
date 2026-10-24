#!/usr/bin/env python3
"""Host tests of the GPR packet codec and the audio framework readiness query (package/tests/gpr_host.c). Model tests only."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'gpr-host'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                    str(PACKAGE / 'tests/gpr_host.c'), str(PACKAGE / 'ShengGpr.c'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
print(result.stdout.strip())
sys.exit(0 if result.returncode == 0 and result.stdout.startswith('PASS') else 1)
