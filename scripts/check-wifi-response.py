#!/usr/bin/env python3
"""Exercise byte responses through user-client and BSD ioctl transports."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'package/WifiNet.cpp').read_text()
copy = source[source.index('int copy_out('):source.index('// A get that gives no buffer')]
answer = source[source.index('int answer_bytes('):source.index('int copy_in(')]
fixture = r'''
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
struct apple80211req { uint32_t req_len; uintptr_t req_data; bool req_kernel; };
static unsigned copies;
static bool fail;
static int copyout(const void *src, uintptr_t dst, size_t size) {
  ++copies;
  if (fail) return EFAULT;
  memcpy(reinterpret_cast<void *>(dst), src, size);
  return 0;
}
'''
checks = r'''
int main() {
  const char data[] = "XX";
  for (bool kernel : {false, true}) {
    for (unsigned capacity : {0u, 1u, 3u, 8u}) {
      unsigned char out[8]; memset(out, 0xa5, sizeof out);
      copies = 0; fail = kernel;
      apple80211req rq{capacity, reinterpret_cast<uintptr_t>(out), kernel};
      assert(!answer_bytes(rq, data, sizeof data));
      assert(rq.req_len == sizeof data);
      const unsigned n = capacity < sizeof data ? capacity : sizeof data;
      assert(!memcmp(out, data, n));
      for (unsigned i = n; i < sizeof out; ++i) assert(out[i] == 0xa5);
      assert(copies == (!kernel && n ? 1u : 0u));
    }
  }
  unsigned char out[3]{};
  apple80211req fault{3, reinterpret_cast<uintptr_t>(out), false};
  fail = true;
  assert(answer_bytes(fault, data, sizeof data) == EFAULT);
  apple80211req missing{3, 0, true};
  assert(answer_bytes(missing, data, sizeof data) == EINVAL);
  puts("PASS: kernel/user responses, truncation, length queries and copy faults");
}
'''
with tempfile.TemporaryDirectory(prefix='wifi-response-') as directory:
    path = Path(directory)
    (path / 'check.cpp').write_text('#include <initializer_list>\n' + fixture + copy + answer + checks)
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', str(path / 'check.cpp'), '-o', str(path / 'check')], check=True)
    subprocess.run([str(path / 'check')], check=True)
