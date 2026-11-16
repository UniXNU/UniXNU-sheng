#!/usr/bin/env python3
"""Generate the macOS root-policy signatures from the IPSW root images (CI first install).

Run after `unixnu tree` and `unixnu ipsw fetch` of the root windows. It reads each window's raw APFS
container exactly as userdata carries it, checks it against the OS profile [root] digest, then signs
all windows with one fresh owner key via make-root-policies.py. The signed headers land in the device
repository's scripts/policies/<profile> (so every later tree assembly copies a consistent set); the
block hash tables stay in <tree>/build/verified-root-<NAME>/hashes.bin for the userdata --object inputs.
"""
import hashlib
import os
import subprocess
import sys

# Import the framework's unixnu package, not the copy assembled into the build tree: the copy's
# ROOT resolves to the tree (no profiles/os). UNIXNU_ROOT is the framework checkout.
sys.path.insert(0, os.path.join(os.environ['UNIXNU_ROOT'], 'scripts'))

from unixnu.cli import parser
from unixnu.config import Context
from unixnu.ipsw import Ipsw
from unixnu import userdata


def main():
    args = parser().parse_args(['build', 'userdata', *sys.argv[1:]])
    ctx = Context(args)
    source = Ipsw(ctx)
    root = ctx.board['root']
    keymap = {'OS': 'os', 'SystemOS': 'systemos', 'App': 'app'}
    rawdir = ctx.work / 'root-raw'
    rawdir.mkdir(parents=True, exist_ok=True)
    specs = []
    for window in ctx.board['userdata']['windows']:
        name = window['name']
        key = keymap[name]
        try:
            source.member(window['component'])
        except Exception:
            if window.get('optional'):
                print(f'{name}: not in this IPSW, skipped')
                continue
            raise
        dmg = source.disk_image(window['component'])
        size = userdata.image_size(dmg)
        raw = rawdir / f'{name}.img'
        digest = hashlib.sha256()
        with open(raw, 'wb') as handle:
            for block in userdata.disk_blocks(dmg, size):
                handle.write(block)
                digest.update(block)
        wanted = root[key + '_image_sha256']
        if digest.hexdigest() != wanted:
            raise SystemExit(f'{name}: raw image sha256 {digest.hexdigest()} != profile {wanted}')
        specs += ['--window', f'{name}={raw}:{root[key + "_volume_uuid"]}']
        print(f'{name}: {size} bytes raw, sha256 matches the profile')
    if not specs:
        raise SystemExit('no root window found in the IPSW')
    output = ctx.device / 'scripts/policies' / ctx.board['os']['name']
    subprocess.run([sys.executable, str(ctx.tree / 'scripts/make-root-policies.py'),
                    '--tree', str(ctx.tree), '--output', str(output), *specs], check=True)
    print('root policies written to', output)


if __name__ == '__main__':
    main()