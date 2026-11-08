#!/usr/bin/env python3
"""Check the UFS log's message stream (package/BootLog.c, msg_mode 1) and its decoder (scripts/read-sheng-log.py).

The stream functions are compiled on the host against a simulated kernel message ring and a recording log window. Rounds of kernel
messages of varying size, a full wrap of the ring between rounds, a replaced ring and a failed write must leave a message area that the
decoder turns back into the newest bytes of the stream, while each round writes only the blocks its new bytes reached."""
from pathlib import Path
import importlib.util
import random
import struct
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'package/BootLog.c').read_text()
body = '// The message stream (msg_mode 1).' + source.split('// The message stream (msg_mode 1).', 1)[1].split('\n// Log groups (boot arguments', 1)[0]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum {LogBlock = 4096, LogMsgFirst = 1, LogMsgBlocks = 32};
struct boot_log_msgbuf {int msg_magic, msg_size, msg_bufx, msg_bufr; char *msg_bufc;};
static struct boot_log_msgbuf kernel_ring, *msgbufp = &kernel_ring;
static uint8_t *boot_log_msg_ring, window[(LogMsgFirst + LogMsgBlocks) * LogBlock];
static unsigned writes, blocks_written, fail_next;
static int native_ufs_log_write(uint64_t rel, uint32_t blocks, const void *src) {
  assert(rel >= LogMsgFirst && rel + blocks <= LogMsgFirst + LogMsgBlocks && blocks);
  if (fail_next) {--fail_next; return 0;}
  memcpy(window + rel * LogBlock, src, (size_t)blocks * LogBlock);
  ++writes; blocks_written += blocks;
  return 1;
}
''' + body + r'''
static void kputc(char c) {kernel_ring.msg_bufc[kernel_ring.msg_bufx++] = c; if (kernel_ring.msg_bufx >= kernel_ring.msg_size) kernel_ring.msg_bufx = 0;}
int main(int argc, char **argv) {
  const char *stream = argv[1], *out = argv[2];
  FILE *f = fopen(stream, "rb"); assert(f);
  static char text[1 << 22]; size_t n = fread(text, 1, sizeof text, f); fclose(f);
  boot_log_msg_ring = calloc(LogMsgBlocks, LogBlock);
  kernel_ring.msg_size = 131072; kernel_ring.msg_bufc = calloc(1, kernel_ring.msg_size);
  uint32_t resyncs = 0;
  // Rounds take the stream in steps: a quiet round, small rounds, a round of more than the ring, a replaced ring, a failed write.
  size_t at = 0; unsigned round = 0;
  FILE *log = fopen(out, "wb"); assert(log);
  while (at < n) {
    size_t step = round == 3 ? 0 : round == 7 ? 300000 : round % 5 == 0 ? 50000 : 1000 + round * 37 % 9000;
    if (step > n - at) step = n - at;
    if (round == 11) {   // the kernel's ring is replaced by a larger one (log_setsize)
      char *bigger = calloc(1, 262144);
      kernel_ring.msg_bufc = bigger; kernel_ring.msg_size = 262144; kernel_ring.msg_bufx = 0;
    }
    for (size_t i = 0; i < step; ++i) kputc(text[at + i]);
    at += step;
    if (round == 9) fail_next = 1;
    const unsigned before = blocks_written;
    msg_stream_round(&resyncs);
    const int ok = msg_stream_write();
    fprintf(log, "%u %zu %d %u %llu %llu %u\n", round, step, ok, blocks_written - before, (unsigned long long)msg_stream.head,
            (unsigned long long)msg_stream.written, resyncs);
    ++round;
  }
  fclose(log);
  char area[256]; snprintf(area, sizeof area, "%s.area", out);
  FILE *a = fopen(area, "wb"); assert(a);
  fwrite(window + LogMsgFirst * LogBlock, 1, (size_t)LogMsgBlocks * LogBlock, a); fclose(a);
  return 0;
}
'''


def main():
    spec = importlib.util.spec_from_file_location('reader', root / 'scripts/read-sheng-log.py')
    reader = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(reader)
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        rng = random.Random(5)
        lines = [f'[{i * 0.0084:12.6f}]: message {i} {"x" * rng.randrange(0, 90)}\n' for i in range(30000)]
        text = ''.join(lines).encode()
        (tmp / 'stream.txt').write_bytes(text)
        (tmp / 'stream.c').write_text(harness)
        subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Werror', '-fsanitize=address,undefined', str(tmp / 'stream.c'),
                        '-o', str(tmp / 'stream')], check=True)
        subprocess.run([str(tmp / 'stream'), str(tmp / 'stream.txt'), str(tmp / 'rounds')], check=True)
        rounds = [list(map(int, line.split())) for line in (tmp / 'rounds').read_text().splitlines()]
        area = (tmp / 'rounds.area').read_bytes()
        cap = len(area)
        assert cap == 32 * 4096
        total = sum(r[1] for r in rounds)
        assert total == len(text), (total, len(text))
        for number, step, ok, blocks, head, written, resyncs in rounds:
            if number == 3:
                assert blocks == 0, 'a quiet round writes nothing'
            elif number == 9:
                assert not ok and written < head, 'a failed write leaves its bytes pending'
            elif number == 10:
                assert ok and written == head, 'the next round writes the pending bytes as well'
            elif ok and number not in (0, 7, 10, 11):
                assert blocks <= step // 4096 + 2, (number, step, blocks)
        assert rounds[-1][6] == 1, 'the replaced ring restarts the stream once'
        head = rounds[-1][5]
        window = bytearray(4096 * (1 + 32))
        window[4096:] = area
        h = dict(msg_first=1, msg_blocks=32, msg_bytes=min(head, cap), msg_mode=1, msg_head=head)
        decoded = reader.messages(bytes(window), h)
        # Every round after the replaced ring stays below the kernel ring's size, so the area holds the newest bytes of the text.
        assert decoded == text[-cap:], 'decoded ring is not the newest stream bytes'
        # A whole window through the decoder: a version 5 header, two slots, the message ring.
        blob = bytearray(4096 * 8192)
        fields = (b'SHLOG001', 5, 168, 0x1234, 0x3d00000, 8192, 1, 32, min(head, cap), 33, 16, (8192 - 33) // 16, 2, 4, 2, 0, 0, 0, 2, b'host')
        at = reader.HEADER.size
        blob[0:at] = reader.HEADER.pack(*fields)
        blob[at:at + reader.EXPORT.size] = reader.EXPORT.pack(0, 65536, 0, 0); at += reader.EXPORT.size
        blob[at:at + 4] = struct.pack('<I', 65536); at += 4
        blob[at:at + 8] = reader.DIAG4.pack(0, 0); at += 8
        blob[at:at + 16] = reader.MSG5.pack(1, 1, head & 0xffffffff, head >> 32)
        blob[4096:4096 * 33] = area
        slots = ['#slot 0 t=0s tag=host\n[page]\nrow\n', '#slot 1 t=2s tag=host\n[page]\nrow\n']
        for n, slot in enumerate(slots):
            start = (33 + n * 16) * 4096
            blob[start:start + 16 * 4096] = b'stale older text' * 4096   # a partly written slot keeps older bytes behind the NUL
            blob[start:start + 4096] = slot.encode().ljust(4096, b'\0')
        run = reader.decode(bytes(blob), tmp / 'out')
        assert (run / 'msgbuf.txt').read_bytes() == decoded
        assert (run / 'slots.txt').read_text() == '\n'.join(slots)
        assert 'msg_mode=1' in (run / 'header.txt').read_text()
        # The linear form (version 4 and the panic flush) still decodes.
        h4 = dict(msg_first=1, msg_blocks=32, msg_bytes=100, version=4)
        assert reader.messages(bytes(window), h4) == area[:100]
    writes = sum(1 for r in rounds if r[3])
    print(f'PASS: {len(rounds)} rounds, {writes} with writes, {sum(r[3] for r in rounds)} message blocks written for '
          f'{len(text)} stream bytes; the decoder returns the newest {cap} bytes')


if __name__ == '__main__':
    main()
