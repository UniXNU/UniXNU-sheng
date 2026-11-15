#!/usr/bin/env python3
"""Pack a firmware watchdog probe that waits in XNU before EARLY_BOOT."""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import tomllib

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path, help='known sheng header-v0 XNU boot image')
    parser.add_argument('output', type=Path)
    parser.add_argument('--uefi', required=True, type=Path)
    parser.add_argument('--fd', required=True, type=Path)
    parser.add_argument('--dtb', required=True, type=Path)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('choose a new output image')
    board = tomllib.loads((ROOT / 'board.toml').read_text())
    core = (ROOT / board['package']['unixnu']).resolve()
    sys.path.insert(0, str(core / 'scripts'))
    from unixnu import bootimg
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sheng-watchdog-', dir=args.output.parent) as directory:
        work = Path(directory)
        payload = work / 'payload.img'
        with args.input.open('rb') as source:
            header = source.read(4096)
            if header[:8] != b'ANDROID!':
                parser.error('require a sheng Android boot image')
            kernel_size, _, ramdisk_size, _, _, _, _, page, version, _ = struct.unpack_from('<10I', header, 8)
            if version != 0 or page < 2048 or page & (page - 1):
                parser.error('require a sheng header-v0 image')
            source.seek(page + (kernel_size + page - 1) // page * page)
            with payload.open('wb') as target:
                left = ramdisk_size
                while left:
                    data = source.read(min(left, 8 * 1024 * 1024))
                    if not data:
                        parser.error('truncated payload')
                    target.write(data)
                    left -= len(data)
        kernel = work / 'kernel.macho'
        path = '::/EFI/UniXNU/Payloads/kernel.macho'
        subprocess.run(['mcopy', '-i', str(payload), path, str(kernel)], check=True)
        subprocess.run([sys.executable, str(core / 'scripts/patch-xnu-entry-probe.py'),
                        str(kernel), '--watchdog-wait', '--reset-delay', '120'], check=True)
        subprocess.run(['mcopy', '-o', '-i', str(payload), str(kernel), path], check=True)
        context = SimpleNamespace(board=board, work=work)
        built = bootimg.build(context, args.uefi.resolve(), args.fd.resolve(), args.dtb.resolve(),
                              ramdisk=payload, output=work / 'probe.img')
        shutil.move(built, args.output.resolve())
        shutil.move(work / 'boot.json', args.output.resolve().with_suffix('.json'))
    print(f'Watchdog probe: {args.output}; kernel reset fallback=120s')


if __name__ == '__main__':
    main()
