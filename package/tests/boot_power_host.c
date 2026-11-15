// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdio.h>
enum {
  kPEHaltCPU, kPERestartCPU, kPEHangCPU, kPEUPSDelayHaltCPU,
  kPEPanicRestartCPU, kPEPanicSync, kPEPagingOff, kPEPanicBegin,
  kPEPanicEnd, kPEPanicRestartCPUNoCallouts, kPEPanicDiagnosticsDone,
  kPEPanicDiagnosticsInProgress
};
#include "../BootPower.h"
static unsigned calls;
static uint32_t last;
static int invoke(uint32_t fid) { ++calls; last = fid; return -7; }
int main(void) {
  const unsigned off[] = {kPEHaltCPU, kPEUPSDelayHaltCPU};
  const unsigned reset[] = {kPERestartCPU, kPEPanicRestartCPU, kPEPanicRestartCPUNoCallouts};
  for (unsigned i = 0; i < sizeof off / sizeof off[0]; ++i) {
    unsigned before = calls;
    assert(boot_power_dispatch(off[i], 0, 1, invoke) == -1 && calls == before);
    assert(boot_power_dispatch(off[i], 1, 0, invoke) == -7 && calls == before + 1);
    assert(last == 0x84000008u);
  }
  for (unsigned i = 0; i < sizeof reset / sizeof reset[0]; ++i) {
    unsigned before = calls;
    assert(boot_power_dispatch(reset[i], 1, 0, invoke) == -1 && calls == before);
    assert(boot_power_dispatch(reset[i], 0, 1, invoke) == -7 && calls == before + 1);
    assert(last == 0x84000009u);
  }
  const unsigned notifications[] = {kPEPanicSync, kPEPagingOff, kPEPanicBegin, kPEPanicEnd,
                                    kPEPanicDiagnosticsDone, kPEPanicDiagnosticsInProgress};
  unsigned before = calls;
  for (unsigned i = 0; i < sizeof notifications / sizeof notifications[0]; ++i)
    assert(boot_power_dispatch(notifications[i], 1, 1, invoke) == 0);
  assert(boot_power_dispatch(kPEHangCPU, 1, 1, invoke) == -1);
  assert(boot_power_dispatch(999, 1, 1, invoke) == -1 && calls == before);
  puts("PASS: platform power dispatch, unsupported calls and notification isolation");
}
