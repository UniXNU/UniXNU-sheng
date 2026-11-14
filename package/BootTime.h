// SPDX-License-Identifier: MIT
#ifndef SHENG_BOOT_TIME_H
#define SHENG_BOOT_TIME_H
#include <kern/clock.h>
static inline uint64_t boot_mach_ns(uint64_t ticks) {
  uint64_t ns = 0;
  absolutetime_to_nanoseconds(ticks, &ns);
  return ns;
}
static inline uint64_t boot_mach_us(uint64_t ticks) {return boot_mach_ns(ticks) / 1000;}
static inline uint64_t boot_mach_ms(uint64_t ticks) {return boot_mach_ns(ticks) / 1000000;}
#endif
