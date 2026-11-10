#!/usr/bin/env python3
"""Stage pinned sheng Bluetooth firmware under the kernel's Data addon prefix."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import os
import tempfile
import sys

sys.path.append(str(Path.cwd() / 'scripts'))
from github_download import fetch_bytes

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--source', type=Path, default=root/'.cache/firmware/qca')
p.add_argument('--fetch', action='store_true', help='fetch missing inputs from the pinned firmware repository')
a = p.parse_args()
manifest = json.loads((root/'firmware/bluetooth/manifest.json').read_text())
prefix = re.search(r'^#define SHENG_BT_FIRMWARE_PREFIX "([^"]+)"$',
    (root/'package/BluetoothFirmware.h').read_text(), re.M).group(1)
staged = {}
for name, expected in manifest['files'].items():
    if not re.fullmatch(r'hmtbtfw[0-9a-f]{2}\.tlv|hmtnv[0-9a-f]{2}\.(bin|b[0-9a-f]{2,4})', name):
        raise SystemExit('Unsupported Bluetooth firmware name: '+name)
    source = a.source/name
    fetched = False
    if not source.exists() and a.fetch:
        url = manifest['repository'].replace('github.com', 'raw.githubusercontent.com')+'/'+manifest['commit']+'/qca/'+name
        data = fetch_bytes(url, max_bytes=expected['bytes'])
        fetched = True
    else:
        data = source.read_bytes()
    if len(data) != expected['bytes'] or hashlib.sha256(data).hexdigest() != expected['sha256']:
        raise SystemExit('Bluetooth firmware identity mismatch: '+name)
    if fetched:
        source.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(dir=source.parent, prefix=name+'.', delete=False) as cache:
            temporary = Path(cache.name)
            cache.write(data)
        try:
            os.replace(temporary, source)
        finally:
            temporary.unlink(missing_ok=True)
    staged[name] = data
destination = a.output.resolve()/prefix
destination.mkdir(parents=True, exist_ok=True)
for name, data in staged.items():
    (destination/name).write_bytes(data)
report = dict(staged=True, firmware_prefix=prefix, files=manifest['files'],
              repository=manifest['repository'], commit=manifest['commit'], hardware_verified=False)
(a.output/'bluetooth-firmware.json').write_text(json.dumps(report, indent=2)+'\n')
print('Staged Bluetooth firmware: '+str(destination))
