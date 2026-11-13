#!/usr/bin/env python3
"""Check that a stalled VFS mirror cannot reschedule readers or block the log callback."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'package/BootDiagExport.c').read_text()
start = source.index('void boot_desktop_log_capture(unsigned now)')
at = source.index('{', start)
depth, end = 1, at + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
callback = source[start:end]
assert 'vnode_' not in callback and 'vn_rdwr' not in callback and 'IOSleep' not in callback
start = source.index('static void start_desktop_reader(void) {')
at = source.index('{', start)
depth, end = 1, at + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
callback = source[start:end] + '\n' + callback
harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int thread_t;
typedef int wait_result_t;
#define KERN_SUCCESS 0
#define __ATOMIC_RELAXED 0
static unsigned calls, released, enabled;
static int fail_start;
static void native_wifi_logf(int n, const char *f, ...) {(void)n;(void)f;}
static int PE_parse_boot_argn(const char *s, uint32_t *out, size_t n) {(void)s;(void)n;*out=enabled;return 1;}
static void desktop_capture_worker(void *p, wait_result_t r) {(void)p;(void)r;}
static int kernel_thread_start(void (*work)(void *, wait_result_t), void *arg, thread_t *thread) {
  (void)work;(void)arg;++calls;*thread=1;return fail_start;
}
static void thread_deallocate(thread_t t) {(void)t;++released;}
CALLBACK
int main(void) {
  enabled=0;boot_desktop_log_capture(1000);assert(calls==0);
  enabled=1;boot_desktop_log_capture(9);assert(calls==0);
  fail_start=1;boot_desktop_log_capture(10);assert(calls==1 && released==0);
  fail_start=0;boot_desktop_log_capture(11);assert(calls==2 && released==1);
  /* The worker may still be blocked in VFS throughout all these calls. */
  for(unsigned n=12;n<100000;++n)boot_desktop_log_capture(n);
  assert(calls==2 && released==1);
  return 0;
}
'''.replace('CALLBACK', callback)
with tempfile.TemporaryDirectory() as directory:
    test = Path(directory) / 'capture.c'
    test.write_text(harness)
    binary = test.with_suffix('')
    subprocess.run(['cc', '-std=c11', '-Wno-address', '-O2', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
print('Desktop capture callback - PASS')
