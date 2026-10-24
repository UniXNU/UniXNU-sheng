#!/usr/bin/env python3
"""Host test of the firmware partition reader (package/FatVolume.c and the split image view in package/QcomMdt.c,
package/tests/modem_fw_host.c). The volumes are made by mtools, not by this script: a FAT16 like the device's firmware partition
(4096-byte sectors, 4 per cluster) and a FAT32 with 512-byte sectors, each holding a synthetic split firmware image and a fragmented file.
The joined image the reader must produce is made here on its own. With SHENG_ADSP_FW=DIR (a local copy of the device's files, never part
of the repository) the device's adsp and adsp_dtb pieces are checked the same way. Needs mformat, mmd, mcopy and mdel."""
import os
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = ROOT / 'kits/sheng' if (ROOT / 'kits/sheng').is_dir() else ROOT / 'package'
SOURCES = ['tests/modem_fw_host.c', 'FatVolume.c', 'QcomMdt.c']


def synthetic_split(rng):
    """An ELF32 firmware image as program headers plus pieces: headers, hash, two loads (one with a zero tail), a header without bytes."""
    loads = [(0x1000, 0x80000, 3000, 3000, 0x08000005), (0x2000, 0x81000, 9000, 12288, 0x08000007),
             (0x6000, 0x84000, 0, 4096, 0x08000006), (0x7000, 0x85000, 70000, 70000, 0x08000004)]
    phnum = 2 + len(loads)
    header_size = 52 + 32 * phnum
    hash_size = 40 + 32 * phnum
    headers = [(0, 0, 0, header_size, 0, 0x07000000), (0, header_size + 0x400, 0x7f000, hash_size, hash_size, 0x02200000)]
    for offset, address, filesz, memsz, flags in loads:
        headers.append((1, offset if filesz else 0, address, filesz, memsz, flags))
    elf = bytearray(b'\x7fELF\x01\x01\x01' + bytes(9))
    elf += struct.pack('<HHIIIIIHHHHHH', 2, 164, 1, 0, 52, 0, 0, 52, 32, phnum, 0, 0, 0)
    for kind, offset, address, filesz, memsz, flags in headers:
        elf += struct.pack('<8I', kind, offset, address, address, filesz, memsz, flags, 0x1000)
    assert len(elf) == header_size
    pieces = {0: bytes(elf)}
    for i, (kind, offset, address, filesz, memsz, flags) in enumerate(headers[1:], 1):
        if filesz:
            pieces[i] = rng.randbytes(filesz)
    return pieces


def headers_of(b00):
    phoff, = struct.unpack_from('<I', b00, 28)
    phnum, = struct.unpack_from('<H', b00, 44)
    return [struct.unpack_from('<8I', b00, phoff + 32 * i) for i in range(phnum)]


def join(pieces):
    """The joined image: piece i at the file offset of program header i, zeros between."""
    phdrs = headers_of(pieces[0])
    size = max(p[1] + p[4] for p in phdrs if p[4])
    image = bytearray(size)
    for i, p in enumerate(phdrs):
        if p[4]:
            image[p[1]:p[1] + p[4]] = pieces[i][:p[4]]
    return bytes(image)


def run(cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def volume(tmp, name, mformat_args, size, images, rng):
    img = tmp / name
    with open(img, 'wb') as f:
        f.truncate(size)
    run(['mformat', '-i', str(img), *mformat_args, '::'])
    run(['mmd', '-i', str(img), '::image'])
    # a fragmented file: fill, free every other file, then write one larger than any hole
    holes = []
    for i in range(6):
        hole = tmp / f'hole{i}'
        hole.write_bytes(rng.randbytes(20000))
        run(['mcopy', '-i', str(img), str(hole), f'::image/hole{i}.bin'])
        holes.append(f'::image/hole{i}.bin')
    for h in holes[::2]:
        run(['mdel', '-i', str(img), h])
    frag = tmp / 'frag.bin'
    frag.write_bytes(rng.randbytes(150000))
    run(['mcopy', '-i', str(img), str(frag), '::image/Fragmented-Firmware.data'])  # a long name: two long-name entries
    for base, pieces in images.items():
        for i, data in pieces.items():
            piece = tmp / f'{base}.b{i:02d}'
            piece.write_bytes(data)
            run(['mcopy', '-i', str(img), str(piece), f'::image/{base}.b{i:02d}'])
    return img, frag


def main():
    rng = random.Random(8550)
    images = {'adsp': synthetic_split(rng)}
    device = os.environ.get('SHENG_ADSP_FW')
    if device:
        for base in ('adsp', 'adsp_dtb'):
            files = sorted(Path(device).glob(base + '.b[0-9][0-9]'))
            images['dev' if base == 'adsp' else 'devdtb'] = {int(f.suffix[2:]): f.read_bytes() for f in files}
    with tempfile.TemporaryDirectory() as t:
        tmp = Path(t)
        exe = tmp / 'modem-fw-host'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                        *[str(PACKAGE / s) for s in SOURCES], '-o', str(exe)], check=True)
        layouts = [('fat16.img', ['-M', '4096', '-c', '4', '-T', '32768', '-h', '1', '-s', '32768'], 128 << 20),
                   ('fat32.img', ['-F', '-c', '1'], 300 << 20)]
        for name, args, size in layouts:
            img, frag = volume(tmp, name, args, size, images, rng)
            for base, pieces in images.items():
                joined = tmp / f'{base}.mbn'
                joined.write_bytes(join(pieces))
                out = subprocess.run([str(exe), str(img), f'image/{base}', str(joined), 'IMAGE/fragmented-firmware.DATA', str(frag)],
                                     check=True, capture_output=True, text=True).stdout.strip()
                print(f'{base}: {out}')
                if not out.startswith('PASS'):
                    sys.exit(1)


main()
