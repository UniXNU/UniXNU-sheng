#!/usr/bin/env python3
"""Change XNU arguments and fit a sheng boot payload to its boot partition.

repack-sheng-boot.py INPUT OUTPUT --kernel-arg autoreset=480
repack-sheng-boot.py INPUT OUTPUT --remove-arg data-reset --partition-bytes 201326592
repack-sheng-boot.py INPUT OUTPUT --payload-file ADDON=EFI/UniXNU/Payloads/kernel.macho.gpuaddon
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import runpy
import struct
import subprocess
import sys
import tempfile
import tomllib

MIB = 1024 * 1024


def run(*command):
    subprocess.run([str(value) for value in command], check=True)


def change_arguments(text, added, removed):
    names = {value.split('=', 1)[0] for value in added} | set(removed)
    blocks = re.split(r'(?m)(?=^\[\[entry\]\]\s*$)', text)
    changed = 0
    for index, block in enumerate(blocks):
        if not block.startswith('[[entry]]'):
            continue
        entry = tomllib.loads(block)['entry'][0]
        if entry.get('type') != 'xnu':
            continue
        values = [value for value in entry.get('args', '').split() if value.split('=', 1)[0] not in names]
        values += added
        line = ' '.join(values)
        if len(line.encode()) >= 1000:
            raise ValueError(f'XNU arguments use {len(line.encode())} bytes; the maximum is 999')
        block, count = re.subn(r'(?m)^args[ \t]*=[ \t]*"(?:\\.|[^"\\])*"[ \t]*$',
                               lambda _: 'args = ' + json.dumps(line), block)
        if count != 1:
            raise ValueError('require one quoted args value per XNU entry')
        blocks[index] = block
        changed += 1
    if not changed:
        raise ValueError('payload has no XNU entry')
    return ''.join(blocks)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('input', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--kernel-arg', action='append', default=[])
    parser.add_argument('--remove-arg', action='append', default=[])
    parser.add_argument('--desktop-session', action='store_true',
                        help='prepare a desktop with automatic login, quiet runtime jobs and no lab or return timer')
    parser.add_argument('--partition-bytes', type=int)
    parser.add_argument('--payload-mib', type=int, help='FAT payload size for a diagnostic RAM boot')
    parser.add_argument('--payload-file', action='append', default=[], metavar='SOURCE=FAT_PATH')
    parser.add_argument('--kernel-tree', type=Path, help='Apply the payload entry/panic probes and POST seal from this resolved build tree to the replacement kernel')
    parser.add_argument('--uefi', type=Path, help='UEFI checkout providing the panic font')
    parser.add_argument('--core', type=Path, help='resolved UniXNU tree providing the payload codec')
    args = parser.parse_args()
    if args.desktop_session:
        os.umask(0o077)
    board = tomllib.loads((Path(__file__).resolve().parents[1] / 'board.toml').read_text())
    core = (args.core or Path(__file__).resolve().parents[1] / board['package']['unixnu']).resolve()
    sys.path.insert(0, str(core / 'scripts'))
    from unixnu import ramdisk as codec
    if args.output.exists() or args.input.resolve() == args.output.resolve():
        parser.error('choose a new output file')
    blob = args.input.read_bytes()
    if len(blob) < 4096 or blob[:8] != b'ANDROID!':
        parser.error('require an Android boot image')
    kernel_size, _, ramdisk_size, _, second_size, _, _, page, version, _ = struct.unpack_from('<10I', blob, 8)
    if version != 0 or page < 2048 or page & (page - 1):
        parser.error('require a sheng header-v0 image with power-of-two page size')
    align = lambda size: (size + page - 1) // page * page
    ramdisk_at = page + align(kernel_size)
    second_at = ramdisk_at + align(ramdisk_size)
    end = second_at + align(second_size)
    if end > len(blob) or any(blob[end:]):
        parser.error('boot image has truncated sections or unknown trailing data')
    kernel = blob[page:page + kernel_size]
    second = blob[second_at:second_at + second_size]
    payload_bytes = args.payload_mib * MIB if args.payload_mib is not None else None
    maximum = board['bootimg']['image']['ramdisk_max_bytes']
    if payload_bytes is not None and payload_bytes < MIB:
        parser.error('payload size must be at least 1 MiB')
    if args.partition_bytes:
        room = args.partition_bytes - page - align(kernel_size) - align(second_size)
        if room < MIB:
            parser.error('boot partition leaves no room for the payload')
        maximum = min(maximum, room)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sheng-repack-', dir=args.output.parent) as directory:
        work = Path(directory)
        original = work / 'original.img'
        original.write_bytes(blob[ramdisk_at:ramdisk_at + ramdisk_size])
        compressed = codec.packed(original)
        if compressed:
            decoded = work / 'decoded.img'
            codec.decode(original, decoded, core)
            original = decoded
        original_size = original.stat().st_size
        payload_bytes = payload_bytes or original_size
        if args.partition_bytes and not compressed:
            payload_bytes = min(payload_bytes, maximum // MIB * MIB)
        config = work / 'unixnu.toml'
        run('mcopy', '-i', original, '::/EFI/UniXNU/unixnu.toml', config)
        if args.desktop_session:
            values = [value for entry in tomllib.loads(config.read_text())['entry']
                      if entry.get('type') == 'xnu' for value in entry.get('args', '').split()]
            args.remove_arg += [value.split('=', 1)[0] for value in values
                                if value.split('=', 1)[0].startswith(('log-', 'diag-', 'trace-'))]
            args.remove_arg += ['log', 'diag', 'gpu-trace', 'gpu-perf', 'icon-cb', 'gpu-prime',
                                'build-tag', 'exec-log', 'vc-text', 'storage-probe', 'data-reset',
                                'autoreset', 'autoreset-clean', 'uefi-watchdog']
            disabled = {'com.apple.cameracaptured', 'org.unixnu.test.daemon',
                        'org.unixnu.test.agent', 'org.unixnu.graphics-log'}
            for value in values:
                if value.startswith('disable-jobs='):
                    disabled.update(value.split('=', 1)[1].split(','))
            args.kernel_arg += ['log=0', 'autoreset=0', 'uefi-watchdog=1',
                                'disable-jobs=' + ','.join(sorted(disabled))]
        config.write_text(change_arguments(config.read_text(), args.kernel_arg, args.remove_arg))
        replacements = []
        if args.desktop_session:
            addon_path = 'EFI/UniXNU/Payloads/kernel.macho.gpuaddon'
            addon = work / 'desktop-addon.bin'
            run('mcopy', '-i', original, '::/' + addon_path, addon)
            addon_codec = runpy.run_path(str(core / 'scripts/addon-add-dir.py'))
            entries = []
            for name, mode, body in addon_codec['read'](addon):
                if name.startswith(('test/', 'debug/')) or name.endswith('org.unixnu.graphics-log.plist'):
                    continue
                if '/org.unixnu.test.' in name:
                    continue
                if name.endswith('AutoLogin/capture-graphics.sh'):
                    continue
                if name.endswith('AutoLogin/install.sh'):
                    script = body.decode().replace('exec >>/Users/Shared/unixnu-test/daemon.log 2>&1',
                                                 'exec >/dev/null 2>&1')
                    script = script.split('/bin/sleep 60\n', 1)[0]
                    script = '\n'.join(line.split(' || /usr/bin/logger', 1)[0] + ' || :'
                                       if ' || /usr/bin/logger' in line else line
                                       for line in script.splitlines()
                                       if not line.startswith('/usr/bin/logger ')) + '\n'
                    body = script.encode()
                if name.endswith('.plist') and name.startswith(('Library/LaunchDaemons/', 'Library/LaunchAgents/')):
                    job = plistlib.loads(body)
                    job['StandardOutPath'] = '/dev/null'
                    job['StandardErrorPath'] = '/dev/null'
                    body = plistlib.dumps(job)
                entries.append((name, mode, body))
            for domain, label in [('LaunchDaemons', 'org.unixnu.test.daemon'),
                                  ('LaunchAgents', 'org.unixnu.test.agent'),
                                  ('LaunchDaemons', 'org.unixnu.graphics-log')]:
                name = f'Library/{domain}/{label}.plist'
                entries.append((name, 0o644, plistlib.dumps(dict(
                    Label=label, Disabled=True, RunAtLoad=False,
                    ProgramArguments=['/usr/bin/true']))))
            addon_codec['write'](addon, entries)
            replacements.append((addon, Path(addon_path)))
        for specification in args.payload_file:
            source, separator, destination = specification.partition('=')
            relative = Path(destination)
            if not separator or not Path(source).is_file() or relative.is_absolute() or '..' in relative.parts:
                parser.error('--payload-file needs an existing source and relative FAT path')
            replacements.append((Path(source), relative))
        if args.kernel_tree:
            tree = args.kernel_tree.resolve()
            destination = Path('EFI/UniXNU/Payloads/kernel.macho')
            selected = [source for source, relative in replacements if relative == destination]
            if len(selected) != 1 or not any(relative == destination.with_suffix('.macho.vroot') for _, relative in replacements):
                parser.error('--kernel-tree requires one kernel.macho and its matching kernel.macho.vroot replacement')
            from unixnu.runtime import seed_uuid
            kernel_bytes = selected[0].read_bytes()
            initrds = {entry.get('initrd') for entry in tomllib.loads(config.read_text())['entry']
                       if entry.get('type') == 'xnu' and entry.get('initrd')}
            for index, initrd in enumerate(sorted(initrds)):
                seed = work / f'data-{index}.dbr'
                path = Path(initrd.replace('\\', '/'))
                path = Path(str(path).lstrip('/')) if path.is_absolute() else Path('EFI/UniXNU') / path
                run('mcopy', '-i', original, '::/' + path.as_posix(), seed)
                identity = seed_uuid(tree, seed)
                if identity.encode() + b'\0' not in kernel_bytes:
                    parser.error(f'replacement kernel does not contain payload Data UUID {identity}; rebuild with --data-ramdisk SEED')
            candidate = work / 'kernel.macho'
            candidate.write_bytes(kernel_bytes)
            env = dict(os.environ)
            if args.uefi:
                env['UNIXNU_UEFI'] = str(args.uefi.resolve())
            if board.get('payload', {}).get('panic_screen'):
                subprocess.run([sys.executable, str(tree / 'scripts/patch-xnu-entry-probe.py'),
                                str(candidate), '--panic-screen', '--no-reset'], cwd=tree, env=env, check=True)
                if b'_source_corecrypto14_bootstrap\0' in candidate.read_bytes():
                    subtype = struct.unpack_from('<I', candidate.read_bytes(), 8)[0] & 0xff
                    env['UNIXNU_XNU_ARCH'] = {0: 'arm64', 2: 'arm64e'}[subtype]
                    line = ' '.join(entry.get('args', '') for entry in tomllib.loads(config.read_text())['entry']
                                    if entry.get('type') == 'xnu')
                    subprocess.run([sys.executable, str(tree / 'scripts/seal-source-crypto-kernel.py'),
                                    str(candidate), '--kernel-args', line], cwd=tree, env=env, check=True)
            replacements = [(candidate if relative == destination else source, relative) for source, relative in replacements]
        payload = original
        # Rebuild file replacements on a fresh FAT with zeroed free clusters.
        if payload_bytes != original_size or replacements:
            files = work / 'files'
            files.mkdir()
            run('mcopy', '-s', '-i', original, '::*', files)
            (files / 'EFI/UniXNU/unixnu.toml').write_bytes(config.read_bytes())
            for source, relative in replacements:
                destination = files / relative
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_bytes(source.read_bytes())
            payload = work / 'resized.img'
            with payload.open('wb') as output:
                output.truncate(payload_bytes)
            run('mformat', '-i', payload, '-T', payload_bytes // 512, '-h', 64, '-s', 32, '-v', 'SHENGESP', '::')
            for path in sorted(files.iterdir()):
                run('mcopy', '-s', '-i', payload, path, '::/')
        else:
            for source, relative in replacements:
                run('mcopy', '-o', '-i', payload, source, '::/' + relative.as_posix())
            run('mcopy', '-o', '-i', payload, config, '::/EFI/UniXNU/unixnu.toml')
        if compressed:
            packed = work / 'packed.img'
            codec.encode(payload, packed, core)
            ramdisk = packed.read_bytes()
        else:
            ramdisk = payload.read_bytes()
        if len(ramdisk) > maximum:
            parser.error(f'encoded ramdisk exceeds the {maximum}-byte sheng limit; use matching compressed-payload firmware')
    header = bytearray(blob[:page])
    struct.pack_into('<I', header, 16, len(ramdisk))
    digest = hashlib.sha1()
    for section in (kernel, ramdisk, second):
        digest.update(section)
        digest.update(struct.pack('<I', len(section)))
    header[576:608] = digest.digest() + bytes(12)
    with args.output.open('wb') as output:
        output.write(header)
        for section in (kernel, ramdisk, second):
            output.write(section)
            output.write(bytes(align(len(section)) - len(section)))
    print(f'{args.output}: {args.output.stat().st_size} bytes, FAT {payload_bytes // MIB} MiB, encoded ramdisk {len(ramdisk)} bytes')


if __name__ == '__main__':
    main()
