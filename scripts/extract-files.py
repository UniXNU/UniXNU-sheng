#!/usr/bin/env python3
"""Put the proprietary files of proprietary-files.txt into .cache/firmware/, checked against their SHA-256.

usage: extract-files.py [SOURCE] [--section NAME] [--check] [--device DIR]

SOURCE is, in the manner of LineageOS extract-files:
  DIR            a dump, a mounted partition or a checkout holding the listed paths
  HOST:DIR       a running system, read with "ssh HOST cat"; nothing on it is changed
  (none)         the public mirror named by "@fetch" in proprietary-files.txt
--check only verifies what is in .cache/firmware and exits 1 when a required file is missing or wrong.
Files with matching hashes are reused. Failed replacements leave the previous cache untouched.
Run network extraction from UniXNU, or set UNIXNU_ROOT to that checkout.
"""
import argparse
import hashlib
import subprocess
import os
import tempfile
import sys
from pathlib import Path


def device_root(argument):
    if argument:
        return Path(argument).resolve()
    here = Path(__file__).resolve().parents[1]
    # Run from the build tree, the device repository is the one that "unixnu tree" linked .cache/firmware to.
    link = here / '.cache/firmware'
    return link.resolve().parents[1] if link.is_symlink() else here


def entries(path):
    base, section, rows = None, '', []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        text = line.strip()
        if text.startswith('@fetch '):
            base = text.split(None, 1)[1].rstrip('/') + '/'
        elif text.startswith('#'):
            section = text.lstrip('# ').strip() or section
        elif text:
            name, _, digest = text.partition('|')
            if len(digest) != 64:
                raise SystemExit(f'{path.name}:{number}: expected path|sha256')
            rows.append((name.lstrip('-'), digest, name.startswith('-'), section))
    return base, rows


def read_source(source, base, name):
    if source is None:
        if not base:
            raise SystemExit('no SOURCE given and proprietary-files.txt has no @fetch')
        candidates = [Path(__file__).resolve().parent, Path.cwd() / 'scripts']
        if os.environ.get('UNIXNU_ROOT'):
            candidates.insert(0, Path(os.environ['UNIXNU_ROOT']) / 'scripts')
        for directory in candidates:
            if (directory / 'github_download.py').is_file():
                sys.path.insert(0, str(directory))
                break
        else:
            raise ValueError('Run from the UniXNU checkout or set UNIXNU_ROOT to it')
        from github_download import fetch_bytes
        print(f'Fetching {name}', flush=True)
        return fetch_bytes(base + name, timeout=60, max_bytes=16 * 1024 * 1024)
    if ':' in source and not Path(source).exists():
        host, _, directory = source.partition(':')
        return subprocess.run(['ssh', '-o', 'BatchMode=yes', host, 'cat', f'{directory.rstrip("/")}/{name}'], check=True, capture_output=True).stdout
    return (Path(source) / name).read_bytes()


def install(target, data):
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, prefix=target.name + '.', delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
        temporary.replace(target)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', nargs='?')
    parser.add_argument('--section', help='only the entries under this heading of proprietary-files.txt')
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--device', help='device repository (default: this one)')
    args = parser.parse_args()
    root = device_root(args.device)
    base, rows = entries(root / 'proprietary-files.txt')
    out = root / '.cache/firmware'
    failed = 0
    for name, digest, optional, section in rows:
        if args.section and args.section.lower() not in section.lower():
            continue
        target = out / name
        if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == digest:
            print(f'ok       {name}')
            continue
        if args.check:
            print(f'{"optional " if optional else "MISSING  "}{name}')
            failed += not optional
            continue
        try:
            local = root / 'firmware' / name
            if (args.source is None and local.is_file() and
                    hashlib.sha256(local.read_bytes()).hexdigest() == digest):
                data = local.read_bytes()
            else:
                data = read_source(args.source, base, name)
        except (OSError, subprocess.SubprocessError, ValueError, RuntimeError) as error:
            print(f'{"optional " if optional else "FAILED   "}{name}: {error}')
            failed += not optional
            continue
        if hashlib.sha256(data).hexdigest() != digest:
            print(f'FAILED   {name}: sha256 differs, not kept')
            failed += 1
            continue
        install(target, data)
        print(f'extracted {name} ({len(data)} bytes)')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
