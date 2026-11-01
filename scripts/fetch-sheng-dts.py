#!/usr/bin/env python3
"""Fetch only the pinned sheng DTS include closure, then compile it."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import subprocess
import shutil
import urllib.request

COMMIT = '42f3b40c702abbc37c3a04c95af008cd283aeaf3'
REPOSITORY = 'https://github.com/ianchb/sm8550-mainline'
BASE = f'https://raw.githubusercontent.com/ianchb/sm8550-mainline/{COMMIT}/'
ROOT = Path('.cache/linux-dts')
BOARD = 'arch/arm64/boot/dts/qcom/sm8550-xiaomi-sheng.dts'
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"\n]+)[>"]', re.M)


def fetch(name):
    destination = ROOT / name
    if not destination.exists() and not destination.is_symlink():
        data = urllib.request.urlopen(BASE + name, timeout=30).read()
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
    data = (os.readlink(destination).encode() if destination.is_symlink()
            else destination.read_bytes())
    dependencies = set()
    text = data.decode()
    if re.fullmatch(r'(\.\./)+[\w/.,+-]+\s*', text):
        target = posixpath.normpath(str(PurePosixPath(name).parent / text.strip()))
        if target.startswith('../'):
            raise ValueError('Source symlink escapes the source tree')
        if not destination.is_symlink():
            destination.unlink()
            destination.symlink_to(text.strip())
        return name, hashlib.sha256(data).hexdigest(), {target}
    for opening, include in INCLUDE.findall(text):
        if opening == '<':
            dependencies.add('include/' + include)
        else:
            dependencies.add(str(PurePosixPath(name).parent / include))
    return name, hashlib.sha256(data).hexdigest(), dependencies


if __name__ == '__main__':
    pending = {BOARD}
    hashes = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        while pending:
            results = list(executor.map(fetch, sorted(pending)))
            pending = set()
            for name, digest, dependencies in results:
                hashes[name] = digest
                pending |= dependencies
            pending -= hashes.keys()
    (ROOT / 'sources.json').write_text(json.dumps({
        'repository': REPOSITORY, 'commit': COMMIT, 'sha256': hashes,
    }, indent=2) + '\n')
    output = Path('build/sheng')
    output.mkdir(parents=True, exist_ok=True)
    preprocessed = output / 'sheng.preprocessed.dts'
    with preprocessed.open('w') as stream:
        subprocess.run(['clang', '-E', '-P', '-nostdinc', '-undef', '-D__DTS__',
                        '-x', 'assembler-with-cpp', '-I', str(ROOT / 'include'),
                        str(ROOT / BOARD)], stdout=stream, check=True)
    with (output / 'dtc.log').open('w') as log:
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o',
                        str(output / 'sheng.dtb'), str(preprocessed)],
                       stderr=log, check=True)
    adreno = Path('build/adreno-firmware')
    adreno.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(output / 'sheng.dtb', adreno / 'sheng.dtb')
    print(f'Compiled sheng.dtb from {len(hashes)} source files at {COMMIT}')
