#!/usr/bin/env python3
"""Prepare a fresh macOS Data seed and signed root windows for sheng packaging."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2] / 'UniXNU'
sys.path.insert(0, str(ROOT / 'scripts'))
from unixnu.cli import parser, prepare
from unixnu.config import Context
from unixnu.ipsw import Ipsw
from unixnu.userdata import disk_blocks, image_size


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reuse-policies', action='store_true', help='use the selected profile\'s signed root headers and public key')
    args = parser.parse_args()
    subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] /
                                      'scripts/extract-files.py')], check=True)
    subprocess.run([sys.executable, str(Path(__file__).resolve().parents[1] /
                                      'drivers/Ath12kWifi/tools/fetch-sources.py')], check=True)
    ctx = Context(parser().parse_args(['build', 'kernel', '-D', str(Path(__file__).resolve().parents[1]),
                                      '--os', 'macos-27']))
    prepare(ctx, ['kernel'])
    source = Ipsw(ctx)
    runtime = ctx.work / 'runtime'
    runtime.mkdir(parents=True, exist_ok=True)
    seed = runtime / 'data.dbr'
    if not seed.exists():
        password = os.environ['UNIXNU_LAB_PASSWORD']
        subprocess.run(['sudo', sys.executable, str(ROOT / 'scripts/make-macos-data-seed.py'),
                        str(source.disk_image('OS')), str(seed), '--gib',
                        str(ctx.board['payload']['runtime']['data_gib']), '--skip-setup',
                        '--user-stdin', '--auto-login'],
                       input=json.dumps({'name': 'unixnu', 'password': password}).encode(), check=True)
        subprocess.run(['sudo', 'chown', str(os.getuid()), str(seed)], check=True)
    policies = ctx.device / 'scripts/policies' / ctx.work.parent.name
    if args.reuse_policies:
        if any(not (policies / window['name'] / name).is_file()
               for window in ctx.board['userdata']['windows']
               for name in ('manifest.header.bin', 'trusted-key.bin', 'verification.json')):
            raise SystemExit('Import the selected boot image policies with scripts/import-root-policies.py before reusing them')
        print('Reusing the selected profile signed root policy inputs.')
        return
    for window in ctx.board['userdata']['windows']:
        name = window['name']
        image = source.disk_image(window['component'])
        raw = runtime / f'{name}.raw'
        size = image_size(image)
        try:
            with raw.open('wb') as output:
                for block in disk_blocks(image, size):
                    output.write(block)
            volume = ctx.board['root'][name.lower() + '_volume_uuid']
            subprocess.run([sys.executable, str(ROOT / 'scripts/make-root-policies.py'),
                            '--tree', str(ctx.tree), '--output', str(policies),
                            '--window', f'{name}={raw}:{volume}'], check=True)
        finally:
            raw.unlink(missing_ok=True)


if __name__ == '__main__':
    main()
