#!/usr/bin/env python3
"""Host tests of the Qualcomm QTEE SMCInvoke, UFS RPMB listener, and Xiaomi devauth wire codec (package/tests/qcom_tee_wire_host.c)."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'qcom-tee-wire-host'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                    '-I', str(PACKAGE), str(PACKAGE / 'tests/qcom_tee_wire_host.c'), str(PACKAGE / 'Wn8030.c'), '-o', str(exe)], check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True)
sys.stderr.write(result.stderr)
line = result.stdout.rstrip().splitlines()[-1] if result.stdout.strip() else 'no output'
print(line)
sys.exit(0 if result.returncode == 0 and line.startswith('PASS') else 1)
