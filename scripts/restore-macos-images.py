#!/usr/bin/env python3
"""Restore the pinned macOS root images from an Android sparse userdata backup."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tomllib

DEVICE = Path(__file__).resolve().parents[1]
CORE = (DEVICE / tomllib.loads((DEVICE / 'board.toml').read_text())['package']['unixnu']).resolve()
sys.path.insert(0, str(CORE / 'scripts'))
from unixnu.cli import parser as cli_parser
from unixnu.config import Context
from unixnu.util import sha256_file


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--config', help='UniXNU local build input file')
    args = parser.parse_args()
    options = ['status', '-D', str(DEVICE), '--os', 'macos-27']
    if args.config:
        options += ['--config', args.config]
    ctx = Context(cli_parser().parse_args(options))
    cache = ctx.out_root / 'ipsw' / ctx.board['ipsw']['build']
    cache.mkdir(parents=True, exist_ok=True)
    tool = CORE / 'scripts/extract-sparse-window.py'

    def extract(output, offset, size):
        subprocess.run([sys.executable, str(tool), str(args.source), str(output),
                        '--offset', str(offset), '--bytes', str(size)], check=True)

    with tempfile.TemporaryDirectory(prefix='root-header-', dir=cache) as directory:
        header = Path(directory) / 'header.bin'
        extract(header, 0, 4096)
        data = header.read_bytes()
    if data[:8] != b'SHNGROOT' or hashlib.sha256(data[:4064]).digest() != data[4064:]:
        parser.error('require a userdata backup with a valid SHNGROOT manifest')
    version, count = struct.unpack_from('<II', data, 8)
    if version not in (1, 2) or not 1 <= count <= (4064 - 24) // 128:
        parser.error('unsupported SHNGROOT manifest')
    windows = {}
    for index in range(count):
        name, offset, size = struct.unpack_from('<32sQQ', data, 24 + index * 128)
        windows[name.rstrip(b'\0').decode()] = (offset, size)
    for window in ctx.board['userdata']['windows']:
        name = window['name']
        target = cache / (name + '.raw.dmg')
        expected = ctx.board['root'][name.lower() + '_image_sha256']
        if not target.exists():
            extract(target, *windows[name])
        if sha256_file(target) != expected:
            raise SystemExit(f'{target}: content differs from the selected OS profile')
        print(f'{name}: restored the selected macOS root image', flush=True)


if __name__ == '__main__':
    main()
