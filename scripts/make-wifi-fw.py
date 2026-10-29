#!/usr/bin/env python3
"""make-wifi-fw.py OUT [DIR]: pack the WCN7850 firmware files of DIR (default ./firmware/wcn7850) into one raw image for a spare partition.
Layout: 4096-byte header (magic 'WFW1', u32 count, then per file: 24-byte name, u32 offset, u32 size), files at 4096-byte aligned offsets.
Never commit the output or the inputs."""
import struct, sys, os
src = sys.argv[2] if len(sys.argv) > 2 else os.path.join('firmware', 'wcn7850')
names = ['amss.bin', 'm3.bin', 'board-2.bin'] + (['amss2.bin'] if os.path.exists(os.path.join(src, 'amss2.bin')) else [])
files = [(n, open(os.path.join(src, n), 'rb').read()) for n in names]
header = bytearray(4096)
header[0:4] = b'WFW1'
struct.pack_into('<I', header, 4, len(files))
body = bytearray()
at = 4096
for i, (n, d) in enumerate(files):
    struct.pack_into('<24sII', header, 8 + 32 * i, n.encode(), at, len(d))
    pad = (-len(d)) % 4096
    body += d + bytes(pad)
    at += len(d) + pad
open(sys.argv[1], 'wb').write(bytes(header) + bytes(body))
print(sys.argv[1], 4096 + len(body), 'bytes')
