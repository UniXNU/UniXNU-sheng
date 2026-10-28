// SPDX-License-Identifier: BSD-2-Clause-Patent
// A fault while reading a user stack must not block the persistent log writer.
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/misc_protos.h>
#include <pexpert/pexpert.h>

extern int nprocs;
extern void boot_process_capture(void);
static unsigned started;

static void capture_worker(void *argument, wait_result_t result) {
  (void)argument;
  (void)result;
  thread_set_thread_name(current_thread(), "boot_process_probe");
  for (;;) {
    if (nprocs > 0) boot_process_capture();
    assert_wait_timeout((event_t)&started, THREAD_UNINT, 2000, NSEC_PER_MSEC);
    thread_block(THREAD_CONTINUE_NULL);
  }
}

void boot_process_start(void) {
  unsigned enabled = 0;
  if (!PE_parse_boot_argn("boot-ui-trace", &enabled, sizeof enabled) || enabled != 1) return;
  if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
  thread_t thread;
  if (kernel_thread_start_priority(capture_worker, NULL, MINPRI_KERNEL, &thread) == KERN_SUCCESS) {
    thread_deallocate(thread);
    printf("sheng-log: process stack capture enabled\n");
  } else {
    __atomic_store_n(&started, 0, __ATOMIC_RELEASE);
    printf("sheng-log: process stack worker could not start\n");
  }
}
