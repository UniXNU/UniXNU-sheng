#!/usr/bin/env python3
"""Check the copy of the previous run's UFS log (package/BootLog.c prev_log_copy, kern.unixnu_prev_log) and its decoder
(scripts/read-sheng-log.py --prev).

prev_log_copy is compiled on the host against a simulated log window. A previous run that wrapped its slot ring must come back as its
header, message area, newest slots and startup checkpoints; read-sheng-log.py puts them back in place and decodes the same messages and
the same newest slot as a full window. A window the header does not describe, a run with few slots and a failed read are covered."""
from pathlib import Path
import importlib.util
import re
import struct
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'package/BootLog.c').read_text()
section = source.split('// The previous run\'s log (kern.unixnu_prev_log', 1)[1].split('// End of the previous run\'s log.', 1)[0]
section = '// The previous run\'s log (kern.unixnu_prev_log' + section
header_struct = re.search(r'struct boot_log_header \{.*?\n\};', source, re.S).group(0)
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum {LogBlock = 4096, LogMsgFirst = 1, LogMsgBlocks = 32, LogSlotFirst = 33, LogSlotBlocks = 16};
''' + header_struct + '\n' + section + r'''
enum {Blocks = 8192};
static uint8_t *medium;
static int fail_at = -1, reads;
static int fake_read(uint64_t rel, uint32_t n, void *dst) {
  assert(rel + n <= Blocks && n);
  if (reads++ == fail_at) return 0;
  memcpy(dst, medium + rel * LogBlock, (size_t)n * LogBlock);
  return 1;
}
static size_t live;
static void *fake_alloc(size_t n) {void *p = NULL; if (posix_memalign(&p, LogBlock, n)) return NULL; live += n; return p;}
static void fake_release(void *p, size_t n) {live -= n; free(p);}
static void put(const char *path, const void *data, size_t size) {FILE *f = fopen(path, "wb"); assert(f); fwrite(data, 1, size, f); fclose(f);}
static uint8_t *copy(uint32_t *bytes) {reads = 0; return prev_log_copy(0x3d00000, Blocks, fake_read, fake_alloc, fake_release, bytes);}
int main(int argc, char **argv) {
  assert(argc == 2);
  const char *dir = argv[1];
  char path[512];
  medium = calloc(Blocks, LogBlock);
  snprintf(path, sizeof path, "%s/window.bin", dir);
  FILE *f = fopen(path, "rb"); assert(f); assert(fread(medium, LogBlock, Blocks, f) == Blocks); fclose(f);
  uint32_t bytes = 0;
  uint8_t *blob = copy(&bytes);
  assert(blob && bytes);
  snprintf(path, sizeof path, "%s/prev.bin", dir); put(path, blob, bytes);
  fake_release(blob, bytes);
  // Another window (the root layout moved): nothing is kept.
  struct boot_log_header *h = (struct boot_log_header *)medium;
  h->first_lba += 1;
  assert(!copy(&bytes));
  h->first_lba -= 1;
  // No log on the medium (an erased header).
  memcpy(h->magic, "\0\0\0\0\0\0\0\0", 8);
  assert(!copy(&bytes));
  memcpy(h->magic, "SHLOG001", 8);
  // A run that wrote three slots.
  const uint32_t written = h->slots_written;
  h->slots_written = 3;
  blob = copy(&bytes); assert(blob);
  snprintf(path, sizeof path, "%s/prev3.bin", dir); put(path, blob, bytes);
  fake_release(blob, bytes);
  h->slots_written = written;
  // A read that fails half way: nothing is kept and nothing leaks.
  fail_at = 5;
  assert(!copy(&bytes));
  fail_at = -1;
  assert(live == 0);
  return 0;
}
'''


def window(reader, written):
    blob = bytearray(4096 * 8192)
    slot_count = (8192 - 33) // 16
    fields = (b'SHLOG001', 5, 168, 0xabcdef, 0x3d00000, 8192, 1, 32, 32 * 4096, 33, 16, slot_count, written, 1200, written, 0, 0, 0, 2,
              b'cd11-sleep')
    at = reader.HEADER.size
    blob[0:at] = reader.HEADER.pack(*fields)
    blob[at:at + reader.EXPORT.size] = reader.EXPORT.pack(0, 65536, 0, 0); at += reader.EXPORT.size
    blob[at:at + 4] = struct.pack('<I', 65536); at += 4
    blob[at:at + 8] = reader.DIAG4.pack(0, 0); at += 8
    head = 3 * 32 * 4096 + 777
    blob[at:at + 16] = reader.MSG5.pack(1, 0, head & 0xffffffff, head >> 32)
    area = bytes((i * 7 + 3) % 251 for i in range(32 * 4096))
    blob[4096:4096 * 33] = area
    for n in range(written):
        start = (33 + (n % slot_count) * 16) * 4096
        text = f'#slot {n} t={2 * n}s tag=cd11-sleep\n[page]\nSL st=2 n={n}\n'.encode()
        blob[start:start + 16 * 4096] = text.ljust(16 * 4096, b'\0')
    tail = 33 + slot_count * 16
    blob[tail * 4096:(tail + 1) * 4096] = b'SHSTG001\ntag=cd11-sleep\nsequence=7\nstage=late\n'.ljust(4096, b'\0')
    return bytes(blob)


def main():
    spec = importlib.util.spec_from_file_location('reader', root / 'scripts/read-sheng-log.py')
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        written = 600                       # more than the 509 slots: the ring wrapped
        full = window(reader, written)
        (tmp / 'window.bin').write_bytes(full)
        (tmp / 'prev.c').write_text(harness)
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', str(tmp / 'prev.c'),
                        '-o', str(tmp / 'prev')], check=True)
        subprocess.run([str(tmp / 'prev'), str(tmp)], check=True)
        prev = (tmp / 'prev.bin').read_bytes()
        assert len(prev) == 4096 + (33 + 64 * 16 + 15) * 4096, len(prev)
        rebuilt = reader.prev_window(prev)
        assert len(rebuilt) == len(full)
        assert rebuilt[:33 * 4096] == full[:33 * 4096], 'header and message area'
        slot_count = (8192 - 33) // 16
        for n in range(written - 64, written):
            at = (33 + (n % slot_count) * 16) * 4096
            assert rebuilt[at:at + 16 * 4096] == full[at:at + 16 * 4096], n
        older = (33 + ((written - 65) % slot_count) * 16) * 4096
        assert rebuilt[older:older + 16 * 4096] == bytes(16 * 4096), 'only the newest 64 slots'
        tail = (33 + slot_count * 16) * 4096
        assert rebuilt[tail:] == full[tail:], 'startup checkpoints'
        run_full = reader.decode(full, tmp / 'full')
        run_prev = reader.decode(rebuilt, tmp / 'prev-out')
        assert (run_prev / 'msgbuf.txt').read_bytes() == (run_full / 'msgbuf.txt').read_bytes()
        assert (run_prev / 'latest.txt').read_text() == (run_full / 'latest.txt').read_text()
        assert (run_prev / 'latest.txt').read_text().startswith(f'#slot {written - 1} ')
        assert (run_prev / 'startup-checkpoint.txt').read_text().startswith('SHSTG001')
        newest = [s for s in (run_prev / 'slots.txt').read_text().split('\n#') if 'slot' in s]
        assert len(newest) == 64, len(newest)
        three = reader.prev_window((tmp / 'prev3.bin').read_bytes())
        run3 = reader.decode(three, tmp / 'three')
        assert (run3 / 'slots.txt').read_text().count('#slot ') == 3
        try:
            reader.prev_window(b'SHLOG001' + bytes(4088))
        except SystemExit:
            pass
        else:
            raise AssertionError('a full window is not a previous-run copy')
    print(f'PASS: previous run with {written} slots kept as {len(prev)} bytes (header, messages, 64 slots, checkpoints); '
          'decoded as the full window decodes; other window, no log, three slots, failed read')


if __name__ == '__main__':
    main()
