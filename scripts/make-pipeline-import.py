#!/usr/bin/env python3
"""Android sparse image of userdata that writes the import region alone: the pipeline caches compiled ahead on the host.

The region is the read-only part of the reserve at the end of userdata (drivers/ShengUfs native_ufs_import_read): from the reserve's
first block up to a 1 MiB guard below the log's export region. Block 0: "SHNGIMP1", u32 version 1, u32 0, u64 pack bytes, the pack's
SHA-256; the pack (a SHEXP001 file from UniXNU scripts/make-pipeline-cache.py) from block 1, zero-padded to a block. Every other block
of the partition is DONT_CARE, so fastboot leaves the GPT, the root image, the Data store and the log as they are. The render server's
plugin reads the region at the next boot (CacheExport.h importRegion) and imports it once.

usage: make-pipeline-import.py PACK OUTPUT.img [--partition-bytes N] [--reserve-mib M]
Flash: fastboot flash userdata OUTPUT.img (the image is the partition's size; only the region's blocks are written)."""
import argparse
import hashlib
from pathlib import Path
import struct
import sys

BLOCK = 4096
LOG_BLOCKS, EXPORT_BLOCKS, GUARD_BLOCKS = 8192, 65536, 256   # drivers/ShengUfs: kLogReserveBlocks, kExportBlocks, kImportGuardBlocks
RAW, DONT_CARE = 0xCAC1, 0xCAC3
MAX_RAW_BLOCKS = 16384   # 64 MiB per RAW chunk
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('pack', type=Path)
parser.add_argument('output', type=Path)
parser.add_argument('--partition-bytes', type=int, default=249497890816)
parser.add_argument('--reserve-mib', type=int, default=1024, help='sheng-log-reserve (1024 by default)')
args = parser.parse_args()
pack = args.pack.read_bytes()
if pack[:8] != b'SHEXP001' or struct.unpack_from('<I', pack, 12)[0] != len(pack) - 16:
    sys.exit('not a SHEXP001 pack')
if args.partition_bytes % BLOCK:
    sys.exit('the partition is not whole 4096-byte blocks')
partition = args.partition_bytes // BLOCK
reserve = args.reserve_mib * 256
region = reserve - LOG_BLOCKS - EXPORT_BLOCKS - GUARD_BLOCKS
first = partition - reserve
blocks = 1 + (len(pack) + BLOCK - 1) // BLOCK
if region <= 0 or blocks > region:
    sys.exit('the pack needs %d blocks, the region holds %d' % (blocks, region))
header = b'SHNGIMP1' + struct.pack('<IIQ', 1, 0, len(pack)) + hashlib.sha256(pack).digest()
payload = header.ljust(BLOCK, b'\0') + pack + b'\0' * (-len(pack) % BLOCK)
# Chunks: DONT_CARE up to the region, RAW over the pack, DONT_CARE to the end of the partition.
chunks = [(DONT_CARE, first, b'')]
for at in range(0, blocks, MAX_RAW_BLOCKS):
    count = min(MAX_RAW_BLOCKS, blocks - at)
    chunks.append((RAW, count, payload[at * BLOCK:(at + count) * BLOCK]))
chunks.append((DONT_CARE, partition - first - blocks, b''))
with args.output.open('wb') as out:
    out.write(struct.pack('<IHHHHIIII', 0xED26FF3A, 1, 0, 28, 12, BLOCK, partition, len(chunks), 0))
    for kind, count, data in chunks:
        out.write(struct.pack('<HHII', kind, 0, count, 12 + len(data)))
        out.write(data)
print('PASS: %s: %d blocks of %d at partition block %d (byte %#x), pack %d bytes sha256 %s; %d chunks, the rest DONT_CARE' % (
    args.output, blocks, region, first, first * BLOCK, len(pack), hashlib.sha256(pack).hexdigest()[:16], len(chunks)))
