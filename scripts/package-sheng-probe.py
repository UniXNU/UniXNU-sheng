#!/usr/bin/env python3
"""Wrap the diagnostic BootShimProbe in the same v0 container as sheng-uefi.img.

probe-reset: paint the frame buffer top white, wait, PSCI SYSTEM_RESET.
probe-fb:    paint the frame buffer top white, then enter the real UEFI FD.
Frame-buffer geometry comes from the sheng MemoryMapLib "Display Reserved"
entry and the sheng.dsc SimpleFb PCDs. Diagnostic output only; ignored.
"""
import gzip
import hashlib
import json
import os
import re
import struct
import subprocess
import tomllib
from pathlib import Path
from unixnu.util import tool

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/sheng/probe'
PKG = ROOT / 'uefi-sheng/Platforms/Xiaomi/shengPkg'


def pcd(name):
    return int(re.search(rf'{name}\|(\d+)', (PKG / 'sheng.dsc').read_text()).group(1))


def abl_dtb():
    # Appended DTB must carry the stock splash_region or ABL powers the panel off.
    subprocess.run(['python3', ROOT / 'scripts/sheng-abl-dtb.py'], check=True)
    return (ROOT / 'build/sheng/sheng-abl.dtb').read_bytes()


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    config = tomllib.loads((ROOT / 'uefi-sheng/Resources/Configs/sheng.toml').read_text())
    base, size = config['uefi_fd']['base'], config['uefi_fd']['size']
    image = config['boot_image']
    region = re.search(r'\{"Display Reserved",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+)',
                       (PKG / 'Library/MemoryMapLib/MemoryMapLib.c').read_text())
    fb_base, fb_size = int(region.group(1), 16), int(region.group(2), 16)
    width, height, depth = pcd('PcdFrameBufferWidth'), pcd('PcdFrameBufferHeight'), pcd('PcdFrameBufferColorDepth')
    fill = width * (depth // 8) * (height // 4)
    fill -= fill % 16
    if not 0 < fill <= fb_size:
        raise SystemExit('Marker does not fit inside Display Reserved')
    clang = tool('clang')
    objcopy = tool('llvm-objcopy')
    fd = (ROOT / 'uefi-sheng/Build/shengPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd').read_bytes()
    dtb = abl_dtb()
    if len(fd) != size:
        raise SystemExit('FD size differs from sheng configuration')
    report = {'fb_base': hex(fb_base), 'fb_fill': hex(fill), 'geometry': [width, height, depth],
              'fd_sha256': hashlib.sha256(fd).hexdigest(), 'qemu_tested': False,
              'hardware_tested': False, 'images': {}}
    for name, reset in (('probe-reset', 1), ('probe-fb', 0)):
        obj, binary = OUT / f'{name}.o', OUT / f'{name}.bin'
        subprocess.run([clang, '--target=aarch64-none-elf', '-c', f'-DFD_BASE={base}', f'-DFD_SIZE={size}',
                        '-DREQUIRES_KERNEL_HEADER=1', f'-DFB_BASE={fb_base}', f'-DFB_FILL={fill}',
                        f'-DPROBE_RESET={reset}', '-DPROBE_DELAY_S=3',
                        ROOT / 'uefi-sheng/BootShim/BootShimProbe.S', '-o', obj], check=True)
        subprocess.run([objcopy, '-O', 'binary', obj, binary], check=True)
        shim = binary.read_bytes()
        if shim[56:60] != b'ARM\x64' or struct.unpack_from('<QQ', shim, 8) != (base, len(shim) + size):
            raise SystemExit(f'{name}: ARM64 header does not describe shim + FD')
        kernel = OUT / f'{name}.Image.gz-dtb'
        kernel.write_bytes(gzip.compress(shim + fd, mtime=0) + dtb)
        (OUT / 'ramdisk').write_bytes(b'')
        out = OUT / f'{name}.img'
        command = ['python3', ROOT / 'uefi-sheng/Resources/Scripts/mkbootimg.py',
                   '--kernel', kernel, '--ramdisk', OUT / 'ramdisk', '-o', out]
        for key in ('base', 'kernel_offset', 'tags_offset', 'pagesize', 'header_version'):
            command.extend([f'--{key}', str(image[key])])
        subprocess.run(command, check=True)
        data = out.read_bytes()
        report['images'][name] = {'path': str(out.relative_to(ROOT)), 'sha256': hashlib.sha256(data).hexdigest(),
                                  'bytes': len(data), 'shim_bytes': len(shim)}
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
