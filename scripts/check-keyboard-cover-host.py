#!/usr/bin/env python3
"""Exercise KeyboardCover.cpp buffer reuse, serialization, reentrancy and cleanup with synchronous host IOHID stubs."""
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'
with tempfile.TemporaryDirectory() as tmp:
    codec = Path(tmp) / 'Wn8030.o'
    sanitize = ['-g', '-O1', '-fsanitize=address,undefined']
    subprocess.run(['cc', '-std=c11', *sanitize, '-c', str(PACKAGE / 'Wn8030.c'), '-o', str(codec)], check=True)
    for macos in (0, 1):
        exe = Path(tmp) / f'keyboard-cover-{macos}'
        subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', *sanitize, f'-DXNU_TARGET_OS_OSX={macos}',
                        '-I', str(PACKAGE / 'tests/keyboard-hid'), str(PACKAGE / 'tests/keyboard_cover_host.cpp'),
                        str(codec), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
