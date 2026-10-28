#!/usr/bin/env python3
"""Build the sheng audio server plug-in (hal/ShengAudioPlugIn.c) as an iOS driver bundle, signed ad hoc.

  build-sheng-audio-hal.py OUTDIR [TARGET]

TARGET is the clang target of the audio server (default arm64-apple-ios17.0; iPadOS 27's audio server is arm64e-apple-ios27.0).

Writes OUTDIR/ShengAudio.driver and OUTDIR/trust.json (the executable's CDHash, for the owner trust cache). The iOS SDK has no
audio server plug-in headers; the macOS SDK's CoreAudio headers declare the same interface and are used for the build only."""
import json
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'
if len(sys.argv) not in (2, 3):
    sys.exit(__doc__)
out = Path(sys.argv[1]).resolve()
target = sys.argv[2] if len(sys.argv) == 3 else 'arm64-apple-ios17.0'
ios = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
mac = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
bundle = out / 'ShengAudio.driver'
shutil.rmtree(bundle, ignore_errors=True)
bundle.mkdir(parents=True)
with tempfile.TemporaryDirectory() as tmp:
    headers = Path(tmp) / 'CoreAudio'
    headers.mkdir()
    for name in ('AudioServerPlugIn.h', 'AudioHardwareBase.h', 'AudioHardware.h'):
        (headers / name).symlink_to(Path(mac) / 'System/Library/Frameworks/CoreAudio.framework/Headers' / name)
    executable = bundle / 'ShengAudio'
    subprocess.run(['xcrun', 'clang', '-target', target, '-isysroot', ios, '-I', tmp, '-I', str(PACKAGE), '-O2', '-g0',
                    '-Wall', '-Wextra', '-Werror', '-bundle', '-fvisibility=hidden', '-framework', 'CoreFoundation', '-framework', 'IOKit',
                    str(ROOT / 'hal/ShengAudioPlugIn.c'), '-o', str(executable)], check=True)
shutil.copy(ROOT / 'hal/Info.plist', bundle / 'Info.plist')
subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.unixnu.sheng.audio', str(bundle)], check=True)
subprocess.run(['codesign', '--verify', '--strict', str(bundle)], check=True)
detail = subprocess.run(['codesign', '--display', '--verbose=4', str(bundle)], capture_output=True, text=True, check=True).stderr
cdhash = re.search(r'^CDHash=([0-9a-f]{40})$', detail, re.M).group(1)
(out / 'trust.json').write_text(json.dumps({'cdhash': cdhash}, indent=2) + '\n')
print(f'{bundle} CDHash {cdhash}')
