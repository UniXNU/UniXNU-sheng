#!/usr/bin/env python3
"""Change XNU arguments and fit a sheng boot payload to its boot partition.

repack-sheng-boot.py INPUT OUTPUT --kernel-arg autoreset=480
repack-sheng-boot.py INPUT OUTPUT --remove-arg data-reset --partition-bytes 201326592
repack-sheng-boot.py INPUT OUTPUT --payload-file ADDON=EFI/UniXNU/Payloads/kernel.macho.gpuaddon
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
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
            raise ValueError('XNU arguments reach the 1000-byte limit')
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
    parser.add_argument('--partition-bytes', type=int)
    parser.add_argument('--payload-file', action='append', default=[], metavar='SOURCE=FAT_PATH')
    args = parser.parse_args()
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
    payload_bytes = ramdisk_size
    if args.partition_bytes:
        room = args.partition_bytes - page - align(kernel_size) - align(second_size)
        payload_bytes = min(payload_bytes, room // MIB * MIB)
        if payload_bytes < MIB:
            parser.error('boot partition leaves no room for the payload')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='sheng-repack-', dir=args.output.parent) as directory:
        work = Path(directory)
        original = work / 'original.img'
        original.write_bytes(blob[ramdisk_at:ramdisk_at + ramdisk_size])
        config = work / 'unixnu.toml'
        run('mcopy', '-i', original, '::/EFI/UniXNU/unixnu.toml', config)
        config.write_text(change_arguments(config.read_text(), args.kernel_arg, args.remove_arg))
        for specification in args.payload_file:
            source, separator, destination = specification.partition('=')
            relative = Path(destination)
            if not separator or not Path(source).is_file() or relative.is_absolute() or '..' in relative.parts:
                parser.error('--payload-file needs an existing source and relative FAT path')
            run('mcopy', '-o', '-i', original, source, '::/' + relative.as_posix())
        payload = original
        if payload_bytes != ramdisk_size:
            files = work / 'files'
            files.mkdir()
            run('mcopy', '-s', '-i', original, '::*', files)
            (files / 'EFI/UniXNU/unixnu.toml').write_bytes(config.read_bytes())
            payload = work / 'resized.img'
            with payload.open('wb') as output:
                output.truncate(payload_bytes)
            run('mformat', '-i', payload, '-T', payload_bytes // 512, '-h', 64, '-s', 32, '-v', 'SHENGESP', '::')
            for path in sorted(files.iterdir()):
                run('mcopy', '-s', '-i', payload, path, '::/')
        else:
            run('mcopy', '-o', '-i', payload, config, '::/EFI/UniXNU/unixnu.toml')
        ramdisk = payload.read_bytes()
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
    print(f'{args.output}: {args.output.stat().st_size} bytes, payload {len(ramdisk) // MIB} MiB')


if __name__ == '__main__':
    main()
