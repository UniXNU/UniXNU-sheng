// SPDX-License-Identifier: MIT
// Report the counter and Darwin clocks around one bounded sleep.
#include <inttypes.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

static uint64_t counter(void) {
    uint64_t ticks;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(ticks));
    return ticks;
}
static uint64_t clock_ns(clockid_t clock) {
    struct timespec time;
    if (clock_gettime(clock, &time)) return 0;
    return (uint64_t)time.tv_sec * 1000000000 + (uint64_t)time.tv_nsec;
}
int main(void) {
    mach_timebase_info_data_t timebase;
    if (mach_timebase_info(&timebase) != KERN_SUCCESS) return 1;
    uint64_t frequency;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    const uint64_t ticks = counter(), absolute = mach_absolute_time(), continuous = mach_continuous_time();
    const uint64_t monotonic = clock_ns(CLOCK_MONOTONIC), raw = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const uint64_t realtime = clock_ns(CLOCK_REALTIME);
    sleep(1);
    printf("clock-probe cntfrq=%" PRIu64 " numer=%u denom=%u counter=%" PRIu64
           " absolute=%" PRIu64 " continuous=%" PRIu64 " monotonic-ns=%" PRIu64
           " uptime-raw-ns=%" PRIu64 " realtime-ns=%" PRIu64 "\n",
           frequency, timebase.numer, timebase.denom, counter() - ticks,
           mach_absolute_time() - absolute, mach_continuous_time() - continuous,
           clock_ns(CLOCK_MONOTONIC) - monotonic, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - raw,
           clock_ns(CLOCK_REALTIME) - realtime);
    return 0;
}
