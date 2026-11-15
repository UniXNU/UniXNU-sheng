#!/usr/bin/env python3
"""Wait for a kernel build, combine it with a prepared GPU lab, then collect the timed device run."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import tomllib

DEVICE = Path(__file__).resolve().parents[1]
CORE = (DEVICE / tomllib.loads((DEVICE / 'board.toml').read_text())['package']['unixnu']).resolve()


def run(*command, **kwargs):
    return subprocess.run([str(value) for value in command], check=True, **kwargs)


def extract_kernel(image, output, work):
    sys.path.insert(0, str(CORE / 'scripts'))
    from unixnu import ramdisk
    with image.open('rb') as source:
        header = source.read(48)
        if len(header) != 48 or header[:8] != b'ANDROID!':
            raise ValueError('require an Android boot image')
        kernel, _, size, _, _, _, _, page, version, _ = struct.unpack_from('<10I', header, 8)
        if version or page < 2048 or page & (page - 1):
            raise ValueError('require a header-v0 sheng image')
        source.seek(page + (kernel + page - 1) // page * page)
        payload = work / 'payload.img'
        with payload.open('wb') as target:
            left = size
            while left:
                data = source.read(min(left, 8 * 1024 * 1024))
                if not data:
                    raise ValueError('truncated payload')
                target.write(data)
                left -= len(data)
    if ramdisk.packed(payload):
        decoded = work / 'decoded.img'
        ramdisk.decode(payload, decoded, CORE)
        payload = decoded
    run('mcopy', '-i', payload, '::/EFI/UniXNU/Payloads/kernel.macho', output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=int)
    parser.add_argument('--repository', default='rponeawa/UniXNU-sheng')
    parser.add_argument('--base', type=Path, required=True, help='prepared GPU image with the selected plugin, tools and preserved Data')
    parser.add_argument('--reader', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--build-timeout', type=int, default=21600)
    parser.add_argument('--device-timeout', type=int, default=7500)
    args = parser.parse_args()
    if not args.base.is_file() or not args.reader.is_file() or args.build_timeout <= 0 or args.device_timeout <= 0:
        parser.error('require existing base and reader images and positive timeouts')
    args.output.mkdir(parents=True, exist_ok=True, mode=0o700)
    if args.output.stat().st_mode & 0o077:
        parser.error('output directory must be private, mode 0700')
    end, previous = time.monotonic() + args.build_timeout, None
    while True:
        status = json.loads(subprocess.check_output(['gh', 'run', 'view', str(args.run), '--repo', args.repository,
                                                    '--json', 'status,conclusion,headSha']))
        if status != previous:
            print(json.dumps(status), flush=True)
            previous = status
        if status['status'] == 'completed':
            if status['conclusion'] != 'success':
                raise SystemExit('kernel build ended: ' + status['conclusion'])
            break
        if time.monotonic() >= end:
            raise SystemExit('kernel build wait timed out')
        time.sleep(30)
    inputs = args.output / 'ci'
    command = [sys.executable, CORE / 'scripts/fetch-actions-artifact.py', args.repository, args.run,
               'sheng-macos-27', inputs]
    for name in ('boot.img', 'package.manifest.json'):
        if not (inputs / name).exists():
            command += ['--file', name]
    if '--file' in command:
        run(*command)
    image = args.output / 'boot.img'
    if not image.exists():
        with tempfile.TemporaryDirectory(prefix='kernel-', dir=args.output) as directory:
            work = Path(directory)
            kernel = work / 'kernel.macho'
            extract_kernel(inputs / 'boot.img', kernel, work)
            run(sys.executable, DEVICE / 'scripts/repack-sheng-boot.py', args.base, image,
                '--payload-file', str(kernel) + '=EFI/UniXNU/Payloads/kernel.macho', '--remove-arg', 'data-reset')
    (args.output / 'sources.json').write_text(json.dumps(dict(repository=args.repository, run=args.run,
        device_commit=status['headSha'], gpu_base=str(args.base.resolve()), preserve_data=True), indent=2) + '\n')
    environment = dict(os.environ, READER=str(args.reader.resolve()), OUT=str((args.output / 'logs').resolve()))
    environment.pop('USERDATA', None)
    run(DEVICE / 'scripts/run-sheng-unattended.sh', image, args.device_timeout, env=environment)


if __name__ == '__main__':
    main()
