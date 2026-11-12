#!/usr/bin/env python3
"""Run the actual keyboard worker scheduling policy against a scripted clock and I/O."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'
with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'keyboard-service-host'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-g', '-O1', '-fsanitize=address,undefined',
                    str(PACKAGE / 'tests/keyboard_service_host.cpp'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
