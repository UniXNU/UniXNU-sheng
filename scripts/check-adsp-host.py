#!/usr/bin/env python3
"""Host tests of the audio DSP bring-up cores (package/tests/qcom_adsp_host.c): firmware image parsing, SMEM, SMP2P, GLINK, the
AOSS mailbox, cmd-db and RSC request encoding, SCM ids. Model tests only; nothing here touches hardware."""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'
SOURCES = ['tests/qcom_adsp_host.c', 'QcomMdt.c', 'QcomSmem.c', 'QcomSmp2p.c', 'QcomGlink.c', 'QcomQmp.c', 'QcomRpmh.c']

with tempfile.TemporaryDirectory() as tmp:
    exe = Path(tmp) / 'qcom-adsp-host'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                    *[str(PACKAGE / s) for s in SOURCES], '-o', str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.strip()
print(out)
sys.exit(0 if out.startswith('PASS') else 1)
