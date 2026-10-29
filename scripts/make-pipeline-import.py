#!/usr/bin/env python3
"""Android sparse image of userdata that writes the import region alone: the pipeline caches compiled ahead on the host.

The region is the read-only part of the reserve at the end of userdata (drivers/ShengUfs native_ufs_import_read): from the reserve's
first block up to a 1 MiB guard below the log's export region. The render server's plugin reads its entries on demand through the GPU
driver (AdrenoMetal CacheImport.h): nothing is copied into the Data volume, which lives in RAM (int 590aaee2: unpacking the whole set
into it took 850 MB and starved the GPU of memory).

Layout, little endian, 4096-byte blocks:
  block 0      "SHNGIMP2", u32 version 2, u32 entry count, u64 index bytes, u64 data bytes, the SHA-256 of the index
  block 1..    the index: per entry u64 data offset, u32 stored bytes, u32 bytes, u16 path bytes, the path (relative to the cache
               directory), sorted by path
  then         the data, from the next block: each entry zlib-compressed on its own
Every other block of the partition is DONT_CARE, so fastboot leaves the GPT, the root image, the Data store and the log as they are.

usage: make-pipeline-import.py CACHE_DIR OUTPUT.img [--partition-bytes N] [--reserve-mib M]
CACHE_DIR: UniXNU build/pipeline-cache/cache (scripts/make-pipeline-cache.py). Flash: fastboot flash userdata OUTPUT.img"""
import argparse
import hashlib
from pathlib import Path
import struct
import sys
import zlib

BLOCK = 4096
LOG_BLOCKS, EXPORT_BLOCKS, GUARD_BLOCKS = 8192, 65536, 256   # drivers/ShengUfs: kLogReserveBlocks, kExportBlocks, kImportGuardBlocks
RAW, DONT_CARE = 0xCAC1, 0xCAC3
MAX_RAW_BLOCKS = 16384   # 64 MiB per RAW chunk
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('cache', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--partition-bytes', type=int, default=249497890816)
parser.add_argument('--payload', type=Path, help='also write the region\'s bytes raw here (the host check reads them, CacheImport.h openFile)')
parser.add_argument('--reserve-mib', type=int, default=1024, help='sheng-log-reserve (1024 by default)')
args = parser.parse_args()
if args.partition_bytes % BLOCK:
    sys.exit('the partition is not whole 4096-byte blocks')
# The entries and prime-done; prime-bad names the host's paths and prime.log is the host's run.
files = sorted(p for p in args.cache.rglob('*') if p.is_file() and (p.name == 'prime-done' or p.relative_to(args.cache).parts[0] == 'v1'))
index, data = bytearray(), bytearray()
for path in files:
    name = path.relative_to(args.cache).as_posix().encode()
    raw = path.read_bytes()
    stored = zlib.compress(raw, 9)
    index += struct.pack('<QIIH', len(data), len(stored), len(raw), len(name)) + name
    data += stored
index_blocks = (len(index) + BLOCK - 1) // BLOCK
index += b'\0' * (index_blocks * BLOCK - len(index))
# The index's digest: an entry's own zlib stream carries the check of its bytes.
digest = hashlib.sha256(bytes(index)).digest()
header = b'SHNGIMP2' + struct.pack('<IIQQ', 2, len(files), len(index), len(data)) + digest
payload = header.ljust(BLOCK, b'\0') + bytes(index) + bytes(data) + b'\0' * (-len(data) % BLOCK)
partition = args.partition_bytes // BLOCK
reserve = args.reserve_mib * 256
region = reserve - LOG_BLOCKS - EXPORT_BLOCKS - GUARD_BLOCKS
first = partition - reserve
blocks = len(payload) // BLOCK
if region <= 0 or blocks > region:
    sys.exit('the import needs %d blocks, the region holds %d' % (blocks, region))
if args.payload:
    args.payload.write_bytes(payload)
chunks = [(DONT_CARE, first, b'')]
for at in range(0, blocks, MAX_RAW_BLOCKS):
    count = min(MAX_RAW_BLOCKS, blocks - at)
    chunks.append((RAW, count, payload[at * BLOCK:(at + count) * BLOCK]))
chunks.append((DONT_CARE, partition - first - blocks, b''))
with args.output.open('wb') as out:
    out.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BLOCK, partition, len(chunks), 0))
    for kind, count, chunk in chunks:
        out.write(struct.pack('<HHII', kind, 0, count, 12 + len(chunk)))
        out.write(chunk)
print('PASS: %s: %d entries, index %d KiB, data %d MiB, %d blocks of %d at partition block %d; sha256 %s' % (
    args.output, len(files), len(index) >> 10, len(data) >> 20, blocks, region, first, digest.hex()[:16]))
