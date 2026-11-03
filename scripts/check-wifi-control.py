#!/usr/bin/env python3
"""Check production native callback lifetime, including XNU failed-connect cleanup."""
from pathlib import Path
import hashlib
import json
import subprocess
root = Path(__file__).resolve().parents[1]
source = (root / 'package/WifiProbe.cpp').read_text()
callbacks = 'errno_t controlConnect(' + source.split('errno_t controlConnect(', 1)[1].split('int controlReadMbuf(', 1)[0]
out = root / 'build/wifi-control'
out.mkdir(exist_ok=True)
(out / 'report.json').unlink(missing_ok=True)
prefix = r'''
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
using errno_t = int; using kern_ctl_ref = void *;
struct sockaddr_ctl { uint32_t sc_unit; };
struct ControlClient { uint64_t session; uint32_t unit; };
static ControlClient *gControlClient;
static void *gControlLock;
static bool allocation_fail; static int attach_status;
static unsigned allocated, freed, detached, lock_depth;
static void *IOMalloc(size_t n) { if (allocation_fail) return nullptr; allocated++; return malloc(n); }
static void IOFree(void *p, size_t n) { assert(p && n == sizeof(ControlClient)); freed++; free(p); }
static void IOLockLock(void *) { lock_depth++; }
static void IOLockUnlock(void *) { assert(lock_depth); lock_depth--; }
static int controlError(int status) { return status; }
static int nw_wifi_control_attach(uint64_t *session) { if (!attach_status) *session = 42; return attach_status; }
static void nw_wifi_control_detach(uint64_t session) { assert(session == 42); detached++; }
'''
fixtures = r'''
int main() {
  sockaddr_ctl address{7}; void *info = nullptr;
  allocation_fail = true;
  assert(controlConnect(nullptr, &address, &info) == ENOMEM);
  assert(!info && !controlDisconnect(nullptr, 7, info) && !detached && !allocated);
  allocation_fail = false; attach_status = EBUSY;
  assert(controlConnect(nullptr, &address, &info) == EBUSY && allocated == freed);
  assert(!info && !controlDisconnect(nullptr, 7, info) && !detached);
  attach_status = 0;
  assert(!controlConnect(nullptr, &address, &info) && info == gControlClient);
  assert(gControlClient->session == 42 && gControlClient->unit == 7);
  assert(!controlDisconnect(nullptr, 7, info) && !gControlClient && detached == 1);
  assert(allocated == freed && !lock_depth);
  puts("native control: failed allocation/attach disconnect callbacks, successful owner lifetime and lock balance passed");
}
'''
(out / 'test.cpp').write_text(prefix + callbacks + fixtures)
subprocess.run(['clang++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
    '-fsanitize=address,undefined', str(out / 'test.cpp'), '-o', str(out / 'test')], check=True)
result = subprocess.run([str(out / 'test')], capture_output=True, text=True)
print(result.stdout, end=''); print(result.stderr, end='')
if result.returncode: raise SystemExit(result.returncode)
(out / 'report.json').write_text(json.dumps({'passed': True, 'xnu_allocation_and_socket_calls_simulated': True,
    'source_sha256': hashlib.sha256(source.encode()).hexdigest(), 'output': result.stdout,
    'hardware_verified': False}, indent=2) + '\n')
