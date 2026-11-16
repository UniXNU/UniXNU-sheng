#!/usr/bin/env python3
"""Exercise charger challenge responses with macOS CommonCrypto as the peer."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'
with tempfile.TemporaryDirectory() as directory:
    executable = Path(directory) / 'charge-auth'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', str(PACKAGE / 'tests/charge_auth_host.c'),
                    str(PACKAGE / 'ShengChargeAuth.c'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
