#!/usr/bin/env python3
"""Stage the sheng VPU firmware for the Data addon, under the path package/VideoProbe.cpp reads at the first session. The file is
the "Video" entry of proprietary-files.txt, extracted beforehand by scripts/extract-files.py into .cache/firmware/ (it never
enters a repository); this script only checks it against its SHA-256 and copies it. OUTPUT gets <prefix>/ and report.json."""
import argparse
import hashlib
import json
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
p.add_argument('--source', type=Path, default=root / '.cache/firmware', help='the extracted firmware (default .cache/firmware)')
a = p.parse_args()
path = re.search(r'^const char kFirmware\[\] = "/private/var/ngpu/([^"]+)";$', (root / 'package/VideoProbe.cpp').read_text(), re.M).group(1)
prefix, name = path.rsplit('/', 1)
section, entries = '', {}
for line in (root / 'proprietary-files.txt').read_text().splitlines():
    text = line.strip()
    if text.startswith('#'):
        section = text.lstrip('# ').strip() or section
    elif text and not text.startswith('@') and section == 'Video':
        key, _, digest = text.partition('|')
        entries[key.lstrip('-')] = digest
matches = [key for key in entries if key.rsplit('/', 1)[-1] == name]
if len(matches) != 1:
    raise SystemExit(f'proprietary-files.txt has no single Video entry for {name}')
key = matches[0]
source = a.source / key
if not source.is_file():
    raise SystemExit(f'{source} is missing: extract it first with scripts/extract-files.py [SOURCE] --section Video')
data = source.read_bytes()
if hashlib.sha256(data).hexdigest() != entries[key]:
    raise SystemExit(f'{source}: SHA-256 differs from proprietary-files.txt')
out = a.output.resolve()
(out / 'report.json').unlink(missing_ok=True)
destination = out / prefix
destination.mkdir(parents=True, exist_ok=True)
(destination / name).write_bytes(data)
(out / 'report.json').write_text(json.dumps(dict(staged=True, firmware_prefix=prefix, directory=prefix,
    file=key, bytes=len(data), sha256=entries[key], hardware_verified=False), indent=2) + '\n')
print('Staged VPU firmware: ' + str(destination / name))
