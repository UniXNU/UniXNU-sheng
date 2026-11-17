// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host stand-in for the kernel clock KeyboardCover.cpp reads: absolute time is a nanosecond count the test sets.
#pragma once
#include <cstdint>
inline uint64_t hostClockNs = 0;
inline uint64_t mach_absolute_time() {return hostClockNs;}
inline void absolutetime_to_nanoseconds(uint64_t abs, uint64_t *ns) {*ns = abs;}
