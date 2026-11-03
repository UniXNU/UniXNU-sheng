#!/usr/bin/env python3
"""Exercise the production Wi-Fi DMA arena with the board's configured window."""
from pathlib import Path
import hashlib
import json
import re
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'package/WifiProbe.cpp').read_text()
storage = (root / 'drivers/ShengUfs/NativeUfs.cpp').read_text()
blocks = int(re.search(r'kExtraBlocks = (\d+), kBlock = 2U << 20', storage)[1])
production = 'constexpr unsigned kArenaBlocks' + source.split('constexpr unsigned kArenaBlocks', 1)[1].split('uint64_t pNowNs()', 1)[0]
out = root / 'build/wifi-dma'
out.mkdir(exist_ok=True)
(out / 'report.json').unlink(missing_ok=True)
prefix = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <vector>
static uint8_t *native_ufs_extra_va;
static uint32_t native_ufs_extra_bytes;
static uint64_t native_ufs_extra_iova = 0x10000000;
static void *IOMallocData(size_t n) { return malloc(n); }
static void pLog(int, const char *) {}
'''
tests = r'''
static void run(unsigned mib, bool fits) {
  native_ufs_extra_bytes = mib << 20;
  native_ufs_extra_va = static_cast<uint8_t *>(malloc(native_ufs_extra_bytes)); assert(native_ufs_extra_va);
  assert(arenaInit());
  std::vector<void *> live;
  uint64_t bus;
  // Firmware/dump regions and rings, then concurrent receive storage. This is
  // a capacity/ownership fixture, not a firmware or RF simulation.
  for (size_t n : {size_t(8454144), size_t(7274496), size_t(2097152), size_t(2097152)}) {
    void *p = arenaAlloc(n, &bus); assert(p); live.push_back(p);
  }
  bool failed = false;
  for (unsigned i = 0; i < 4096; ++i) {
    auto p = static_cast<uint8_t *>(arenaAlloc(8192, &bus));
    if (!p) { failed = true; break; }
    assert(!(bus & 8191) && bus >= native_ufs_extra_iova + 4096);
    assert(bus + 8192 <= native_ufs_extra_iova + native_ufs_extra_bytes);
    assert(p[0] == 0 && p[8191] == 0); p[0] = 0xa5; p[8191] = 0x5a;
    live.push_back(p);
  }
  assert(failed != fits && (gArena.failures != 0) == failed);
  printf("DMA arena %u MiB: concurrent ring/RX fixture %s, peak %zu bytes\n", mib, failed ? "exhausted" : "fits", gArena.peak);
  // Free out of order to exercise splitting and adjacent free coalescing.
  for (size_t i = 0; i < live.size(); i += 2) arenaFree(live[i]);
  for (size_t i = 1; i < live.size(); i += 2) arenaFree(live[i]);
  assert(gArena.used == 4096 && gArena.count == 1);
  assert(gArena.blocks[0].off == 4096 && gArena.blocks[0].size == native_ufs_extra_bytes - 4096);
  auto p = static_cast<uint8_t *>(arenaAlloc(8192, &bus)); assert(p && !p[0] && !p[8191]); arenaFree(p);
  free(native_ufs_extra_va); native_ufs_extra_va = nullptr;
}
int main() {
  run(48, false);
  run(CONFIGURED_BLOCKS * 2, true);
  free(gArena.blocks);
}
'''.replace('CONFIGURED_BLOCKS', str(blocks))
(out / 'test.cpp').write_text(prefix + production + tests)
subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra',
    '-Wno-tautological-pointer-compare', '-fsanitize=address,undefined',
    str(out / 'test.cpp'), '-o', str(out / 'test')], check=True)
result = subprocess.run([str(out / 'test')], capture_output=True, text=True)
print(result.stdout, end=''); print(result.stderr, end='')
if result.returncode: raise SystemExit(result.returncode)
(out / 'report.json').write_text(json.dumps({'passed': True,
    'window_bytes': blocks * (2 << 20), 'hardware_verified': False,
    'source_sha256': hashlib.sha256(source.encode()).hexdigest(),
    'storage_sha256': hashlib.sha256(storage.encode()).hexdigest(),
    'output': result.stdout}, indent=2) + '\n')
