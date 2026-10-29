#!/usr/bin/env python3
"""Stream a whole-partition Android sparse image for sheng's userdata, without an intermediate raw file.

Content, byte for byte as scripts/unixnu/userdata.py lays it out: SHNGROOT manifest at 0, window OS at 1 MiB, window SystemOS
after it (1 MiB aligned); then zeros up to the end of the partition (the persistent Data store and the UFS log are cleared).
Zero 4 KiB blocks become FILL chunks (written as zeros), the rest RAW chunks. Each window's SHA-256 is checked against the
earlier userdata-root.json, and the manifest's against its manifest_sha256.

  make-userdata-full.py OS.dmg SYSTEMOS.dmg ROOT.json PARTITION_BYTES OUTPUT
"""
import hashlib
import json
import plistlib
import re
import struct
import subprocess
import sys

BLOCK = 4096
MIB = 1 << 20
CHUNK = 8 * MIB
RAW, FILL = 0xCAC1, 0xCAC2
MAX_RAW = 4096
ZERO = bytes(BLOCK)


def image_size(dmg):
    return plistlib.loads(subprocess.run(['hdiutil', 'imageinfo', '-plist', dmg], capture_output=True, check=True).stdout)['Size Information']['Total Bytes']


def attach(dmg):
    out = subprocess.run(['hdiutil', 'attach', '-nomount', '-readonly', '-noverify', '-noautofsck', dmg], capture_output=True, text=True, check=True).stdout
    for line in out.splitlines():
        m = re.match(r'(/dev/disk\d+)\s', line)
        if m:
            return m.group(1)
    sys.exit('no device for ' + dmg)


class Writer:
    def __init__(self, path, total_blocks):
        self.out = open(path, 'wb')
        self.out.write(bytes(28))
        self.total, self.chunks, self.blocks = total_blocks, 0, 0
        self.raw, self.raw_blocks, self.zero_run = bytearray(), 0, 0

    def flush_raw(self):
        if self.raw_blocks:
            self.out.write(struct.pack('<HHII', RAW, 0, self.raw_blocks, 12 + len(self.raw)) + self.raw)
            self.chunks += 1
            self.raw, self.raw_blocks = bytearray(), 0

    def flush_zero(self):
        if self.zero_run:
            self.out.write(struct.pack('<HHII', FILL, 0, self.zero_run, 16) + bytes(4))
            self.chunks += 1
            self.zero_run = 0

    def data(self, buffer):
        assert len(buffer) % BLOCK == 0
        for at in range(0, len(buffer), BLOCK):
            block = buffer[at:at + BLOCK]
            if block == ZERO:
                self.flush_raw()
                self.zero_run += 1
            else:
                self.flush_zero()
                self.raw += block
                self.raw_blocks += 1
                if self.raw_blocks == MAX_RAW:
                    self.flush_raw()
        self.blocks += len(buffer) // BLOCK

    def zeros(self, count):
        self.flush_raw()
        self.zero_run += count
        self.blocks += count

    def close(self):
        self.flush_raw()
        self.flush_zero()
        assert self.blocks == self.total, (self.blocks, self.total)
        self.out.seek(0)
        self.out.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BLOCK, self.total, self.chunks, 0))
        self.out.close()


def main():
    os_dmg, sys_dmg, root_json, partition, output = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
    pins = json.load(open(root_json))
    layout, cursor = [], MIB
    for name, dmg in (('OS', os_dmg), ('SystemOS', sys_dmg)):
        size = image_size(dmg)
        layout.append((name, dmg, cursor, size))
        cursor += (size + MIB - 1) // MIB * MIB
    total = cursor
    for (name, _, offset, size), pin in zip(layout, pins['windows']):
        if (name, offset, size) != (pin['name'], pin['offset'], pin['size']):
            sys.exit(f'layout differs from {root_json}: {name} {offset} {size}')
    if total != pins['image_bytes'] or partition % BLOCK or partition < total:
        sys.exit('bad sizes')
    writer = Writer(output, partition // BLOCK)
    # Block 0: a RAW chunk right after the file header, overwritten in place with the manifest at the end; then the 1 MiB padding.
    writer.out.write(struct.pack('<HHII', RAW, 0, 1, 12 + BLOCK) + ZERO)
    writer.chunks += 1
    writer.blocks += 1
    writer.zeros(MIB // BLOCK - 1)
    entries = []
    position = MIB
    for name, dmg, offset, size in layout:
        assert position == offset
        device = attach(dmg)
        try:
            digest, done = hashlib.sha256(), 0
            with open(device.replace('/dev/disk', '/dev/rdisk'), 'rb', buffering=0) as handle:
                while done < size:
                    want = min(CHUNK, size - done)
                    data = handle.read(want)
                    if len(data) != want:
                        sys.exit(f'short read {name} at {done}')
                    digest.update(data)
                    if len(data) % BLOCK:
                        data += bytes(BLOCK - len(data) % BLOCK)
                    writer.data(data)
                    done += want
        finally:
            subprocess.run(['hdiutil', 'detach', device], capture_output=True)
        written = (size + BLOCK - 1) // BLOCK * BLOCK
        aligned = (size + MIB - 1) // MIB * MIB
        writer.zeros((aligned - written) // BLOCK)
        position += aligned
        pin = next(w for w in pins['windows'] if w['name'] == name)
        if digest.hexdigest() != pin['sha256']:
            sys.exit(f'{name} sha256 {digest.hexdigest()} differs from the pin {pin["sha256"]}')
        entries.append((name, offset, size, digest.digest()))
        print(f'{name}: {size} bytes at {offset:#x} sha256 {digest.hexdigest()} (matches)', flush=True)
    writer.zeros((partition - total) // BLOCK)
    header = bytearray(4096)
    header[0:8] = b'SHNGROOT'
    struct.pack_into('<IIQ', header, 8, 1, len(entries), total)
    for index, (name, offset, size, digest) in enumerate(entries):
        struct.pack_into('<32sQQII32s40s', header, 24 + 128 * index, name.encode(), offset, size, 512, 1, digest, bytes(40))
    header[4064:4096] = hashlib.sha256(bytes(header[:4064])).digest()
    if hashlib.sha256(bytes(header)).hexdigest() != pins['manifest_sha256']:
        sys.exit('manifest differs from the pin')
    writer.close()
    with open(output, 'r+b') as f:
        f.seek(28 + 12)
        f.write(bytes(header))
    print(f'{output}: partition {partition} bytes, root image {total} bytes, manifest {pins["manifest_sha256"]} (matches)')


if __name__ == '__main__':
    main()
