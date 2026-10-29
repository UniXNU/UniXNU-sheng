#!/usr/bin/env python3
"""Android sparse image that writes only the given byte ranges of a raw image; the rest is DONT_CARE.

make-sparse-delta.py IMAGE OUTPUT START:END [START:END ...]

Ranges are byte offsets (0x... allowed), 4096-byte aligned (END may be the image size). fastboot writes the
RAW chunks and leaves every DONT_CARE block of the partition as it is.
"""
import struct
import sys
from pathlib import Path

BLOCK = 4096
RAW, DONT_CARE = 0xCAC1, 0xCAC3
MAX_RAW_BLOCKS = 16384  # 64 MiB per RAW chunk


def main():
    image, output = Path(sys.argv[1]), Path(sys.argv[2])
    size = image.stat().st_size
    if size % BLOCK:
        raise SystemExit('image size is not a multiple of 4096')
    total = size // BLOCK
    ranges = []
    for text in sys.argv[3:]:
        start, end = (int(part, 0) for part in text.split(':'))
        if start % BLOCK or (end % BLOCK and end != size) or not 0 <= start < end <= size:
            raise SystemExit(f'bad range {text}')
        ranges.append((start // BLOCK, (end + BLOCK - 1) // BLOCK))
    ranges.sort()
    for (_, end), (start, _) in zip(ranges, ranges[1:]):
        if start < end:
            raise SystemExit('ranges overlap')
    chunks, cursor = [], 0
    for start, end in ranges:
        if start > cursor:
            chunks.append((DONT_CARE, cursor, start - cursor))
        while start < end:
            count = min(end - start, MAX_RAW_BLOCKS)
            chunks.append((RAW, start, count))
            start += count
        cursor = end
    if cursor < total:
        chunks.append((DONT_CARE, cursor, total - cursor))
    written = 0
    with image.open('rb') as source, output.open('wb') as target:
        target.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BLOCK, total, len(chunks), 0))
        for kind, start, count in chunks:
            if kind == DONT_CARE:
                target.write(struct.pack('<HHII', kind, 0, count, 12))
                continue
            source.seek(start * BLOCK)
            data = source.read(count * BLOCK)
            data += bytes(count * BLOCK - len(data))  # pad a final partial block of the image
            target.write(struct.pack('<HHII', kind, 0, count, 12 + len(data)))
            target.write(data)
            written += count
    print(f'{output}: {total} blocks, {written} written ({written * BLOCK} bytes), {len(chunks)} chunks')


if __name__ == '__main__':
    main()
