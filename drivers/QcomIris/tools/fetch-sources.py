#!/usr/bin/env python3
"""Fetch the pinned Linux files of tools/source-lock.json into deps/linux and verify their SHA-256 digests.

Files already present with the pinned digest are kept. Usage: fetch-sources.py [--offline]
"""
import hashlib
import json
import sys
import urllib.request
from pathlib import Path

root = Path(__file__).resolve().parents[1]
lock = json.loads((root / 'tools/source-lock.json').read_text())['linux']
deps = root / 'deps/linux'
offline = '--offline' in sys.argv[1:]
for path, digest in lock['files'].items():
    target = deps / path
    if target.exists() and hashlib.sha256(target.read_bytes()).hexdigest() == digest:
        continue
    if offline:
        raise SystemExit(f'missing or changed: {target}')
    url = f"{lock['url']}/plain/{path}?h={lock['ref']}"
    request = urllib.request.Request(url, headers={'User-Agent': 'unixnu-fetch/1'})
    with urllib.request.urlopen(request, timeout=60) as response:
        data = response.read()
    actual = hashlib.sha256(data).hexdigest()
    if actual != digest:
        raise SystemExit(f'{path}: sha256 {actual}, pinned {digest}')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    print(f'fetched {path}')
print(f"Linux {lock['ref']} ({lock['commit'][:12]}): {len(lock['files'])} files verified in {deps}")
