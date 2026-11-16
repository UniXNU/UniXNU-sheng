#!/usr/bin/env python3
"""Create the first-install macOS Data seed (data.dbr) as root, before `unixnu build --init-data`.

unixnu refuses to build the payload until <work>/runtime/data.dbr exists (runtime.prepare). This
reproduces the exact command it prints: the board's payload.runtime spec names the seed tool and the
Data size (data_gib), and the IPSW OS image is the template source. The seed tool mounts the OS image
and copies the Data template preserving file owners, which only root can write, so it runs under sudo.
Run after ci-macos-policies.py (the OS image is already fetched and decrypted by then).
"""
import os
import subprocess
import sys

# Import the framework's unixnu package, not the tree copy (whose ROOT has no profiles/os).
sys.path.insert(0, os.path.join(os.environ['UNIXNU_ROOT'], 'scripts'))

from unixnu.cli import parser
from unixnu.config import Context
from unixnu.ipsw import Ipsw


def main():
    args = parser().parse_args(['build', 'userdata', *sys.argv[1:]])
    ctx = Context(args)
    spec = ctx.board['payload']['runtime']
    out = ctx.work / 'runtime'
    out.mkdir(parents=True, exist_ok=True)
    seed = out / 'data.dbr'
    if seed.is_file():
        print('Data seed already present:', seed)
        return
    os_image = Ipsw(ctx).disk_image('OS')
    cmd = ['sudo', sys.executable, str(ctx.tree / spec['data_seed_tool']), str(os_image), str(seed),
           '--gib', str(spec['data_gib'])]
    if spec.get('skip_setup'):
        cmd.append('--skip-setup')
    print('creating Data seed:', ' '.join(cmd), flush=True)
    subprocess.run(cmd, check=True)
    print('Data seed written to', seed)


if __name__ == '__main__':
    main()