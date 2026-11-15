#!/usr/bin/env python3
"""Build the fastboot-boot-only sheng log reader: the prebuilt sheng kernel (ianchb/sm8550-mainline release asset boot_sheng_singleboot.img,
kernel and appended DTB reused byte for byte, as linux-ref does) plus an initramfs with busybox, the init next to this file, rebootbl and
nvmem-reboot-mode.ko from the same release's linux-xiaomi-sheng.deb. Nothing on UFS is mounted or written. Inputs and output stay outside Git.

usage: build-linux-log-boot.py --boot boot_sheng_singleboot.img --busybox busybox --rebootbl rebootbl --module nvmem-reboot-mode.ko --out log.img"""
import argparse
import gzip
import hashlib
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
CMDLINE = b'rdinit=/init console=tty0 loglevel=4 consoleblank=0'


def cpio(entries):
    """newc archive; entries are (path, mode, data) with directories first."""
    out = bytearray()
    for ino, (name, mode, data) in enumerate(entries + [('TRAILER!!!', 0, b'')], 1):
        path = name.encode() + b'\0'
        out += b'070701' + b''.join(b'%08x' % v for v in (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(path), 0)) + path
        out += b'\0' * (-len(out) % 4) + data
        out += b'\0' * (-len(out) % 4)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    for name in ('boot', 'busybox', 'rebootbl', 'out'):
        ap.add_argument('--' + name, required=True, type=Path)
    ap.add_argument('--module', action='append', default=[], type=Path)
    ap.add_argument('--file', action='append', default=[], metavar='TARGET=SOURCE',
                    help='add a relative initramfs path, preserving the source executable bit')
    ap.add_argument('--append', action='append', default=[], help='additional kernel command-line argument')
    a = ap.parse_args()
    boot = a.boot.read_bytes()
    if boot[:8] != b'ANDROID!':
        raise SystemExit('not an Android boot image')
    kernel_size, kernel_addr, ramdisk_size, _, second_size, _, tags, page, version, os_version = struct.unpack_from('<10I', boot, 8)
    if version != 0 or ramdisk_size or second_size:
        raise SystemExit('expected the header v0 release image without ramdisk')
    kernel = boot[page:page + kernel_size]
    dirs = ['bin', 'dev', 'proc', 'sys', 'run', 'tmp', 'lib', 'lib/modules']
    entries = [(d, 0o40755, b'') for d in dirs]
    entries += [('dev/console', 0o20600, b''), ('init', 0o100755, (HERE / 'init').read_bytes()),
                ('bin/busybox', 0o100755, a.busybox.read_bytes()), ('bin/rebootbl', 0o100755, a.rebootbl.read_bytes())]
    entries += [('lib/modules/' + m.name, 0o100644, m.read_bytes()) for m in a.module]
    present = {name for name, _, _ in entries}
    additions = []
    for spec in a.file:
        name, separator, source = spec.partition('=')
        target = Path(name)
        if not separator or not name or target.is_absolute() or '..' in target.parts or name in present:
            ap.error(f'--file {spec}: require a new relative TARGET=SOURCE path')
        path = Path(source)
        if not path.is_file():
            ap.error(f'--file {spec}: source is not a file')
        for parent in reversed(target.parents):
            folder = parent.as_posix()
            if folder != '.' and folder not in present:
                entries.append((folder, 0o40755, b''))
                present.add(folder)
        mode = 0o100755 if path.stat().st_mode & 0o111 else 0o100644
        additions.append((name, mode, path.read_bytes()))
        present.add(name)
    entries += additions
    ramdisk = gzip.compress(cpio(entries), 9, mtime=0)
    pad = lambda b: b + b'\0' * (-len(b) % page)
    header = bytearray(page)
    header[:8] = b'ANDROID!'
    struct.pack_into('<10I', header, 8, len(kernel), kernel_addr, len(ramdisk), 0x01000000, 0, 0x00f00000, tags, page, 0, os_version)
    cmdline = b' '.join([CMDLINE, *(value.encode() for value in a.append)])
    if len(cmdline) >= 512:
        ap.error('kernel command line exceeds the boot header field')
    header[64:64 + len(cmdline)] = cmdline
    digest = hashlib.sha1()
    for blob in (kernel, ramdisk, b''):
        digest.update(blob)
        digest.update(struct.pack('<I', len(blob)))
    header[576:596] = digest.digest()
    image = bytes(header) + pad(kernel) + pad(ramdisk)
    a.out.write_bytes(image)
    print(f'BUILT {a.out} ({len(image)} bytes) sha256 {hashlib.sha256(image).hexdigest()} ramdisk {len(ramdisk)}')


if __name__ == '__main__':
    main()
