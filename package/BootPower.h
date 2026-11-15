// SPDX-License-Identifier: MIT
#ifndef SHENG_BOOT_POWER_H
#define SHENG_BOOT_POWER_H
#include <stdint.h>

enum { BootPowerOff = 0x84000008u, BootPowerReset = 0x84000009u };

/* Notifications must not issue a firmware transition. The platform calls this
 * only after XNU has synchronized filesystems and completed driver shutdown. */
static int boot_power_dispatch(unsigned type, int off_available, int reset_available,
                               int (*invoke)(uint32_t)) {
  switch (type) {
    case kPEHaltCPU:
    case kPEUPSDelayHaltCPU:
      return off_available ? invoke(BootPowerOff) : -1;
    case kPERestartCPU:
    case kPEPanicRestartCPU:
    case kPEPanicRestartCPUNoCallouts:
      return reset_available ? invoke(BootPowerReset) : -1;
    case kPEPanicSync:
    case kPEPagingOff:
    case kPEPanicBegin:
    case kPEPanicEnd:
    case kPEPanicDiagnosticsDone:
    case kPEPanicDiagnosticsInProgress:
      return 0;
    default:
      return -1;
  }
}
#endif
