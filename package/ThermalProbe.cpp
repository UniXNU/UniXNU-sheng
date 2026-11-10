// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only temperature measurement for the sheng board, off by default. `thermal=1` maps the three SM8550 TSENS controllers and the
// three EPSS frequency domains and samples them whenever the boot log asks (sheng_thermal_sample, once a log round);
// `thermal-ms=<n>` adds a thread that samples every n milliseconds, which is what gives the limit statistics below enough samples.
// Nothing is written to any of these blocks and no interrupt is taken.
//
// Why: the firmware lowers the CPU clusters under load (the governor's throttle count in CpuFreqProbe.cpp: the domain state register
// under 80 % of the vote while the cluster is busy), and nothing in the package read the SoC's own sensors, so whether that limit is
// thermal was not known. The battery row's temperature does not answer it: it is the pack's, and the charging firmware refreshes the
// value it reports only every several minutes (ShengBattmgr.c reads it in every 60 s cycle and gets the same number; macOS runs 49 and
// 51 show one step each, 32.0 to 34.5 and 25.3 to 32.5 degrees, 480 s into the run, together with the capacity and three firmware
// notifications; run 61 ended at 458 s without one). This probe puts the die temperatures next to the frequency the hardware runs:
//   - every sensor of the three controllers in millidegrees, and the hottest of each group (ThermalProbe.h has the register layout,
//     the sensor table and its sources);
//   - per frequency domain the running frequency (qcom-cpufreq-hw.c "epss": reg_domain_state 0x20, bits 7:0 times 19.2 MHz, the
//     register its LMh code reads as the throttled frequency) and the voted one (reg_perf_state 0x320, one row per CPU when
//     reg_dcvs_ctrl 0xb0 bit 0 is set, looked up in reg_freq_lut 0x100);
//   - per domain, whether the firmware's limit interrupt is pending in the interrupt controller. sm8550.dtsi gives cpufreq@17d91000
//     three level interrupts "dcvsh-irq-0..2" (SPI 30, 31, 19); the Linux driver takes one when the hardware limits the domain and
//     clears it by writing reg_intr_clr 0x308. Nothing here handles or clears it, so a pending bit (GICD_ISPENDR of the distributor at
//     0x17100000) says the hardware has limited that domain at least once since it was last cleared. A clear bit proves nothing:
//     whether the block has the interrupt enabled is not known;
//   - the three words next to that clear register (0x300, 0x304, 0x30c), raw: mainline names only 0x308, so their meaning is not
//     established; they are logged so that a status bit that follows the pending interrupt can be recognised;
//   - the limit statistics (ThermalProbe.h thermal_limit): the temperature of a domain's hottest core in the samples where its
//     busiest CPU was at least 90 % busy and it ran under 80 % of its vote, against the busy samples where it did not.
// The mainline device tree's trip points for comparison: CPU cores 90 and 95 degrees (passive) and 110 (critical), GPU 85, 90, 110.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "ThermalProbe.h"

extern "C" {
// [0] stage (0 off, 1 mapped, 2 sampled; 0x10 + n: window n did not map, n = controller for TM, 3 + controller for SROT),
// [1] samples, [2] thermal | thermal-ms << 8, [3..5] SROT hardware version per controller, [6..8] SROT control word.
volatile uint32_t native_thermal_info[9];
// Every sensor in millidegrees Celsius, THERMAL_NONE (INT32_MIN) when disabled or without a stable value.
volatile int32_t native_thermal_mc[THERMAL_CONTROLLERS][THERMAL_SENSORS];
// Per controller, the sensors whose status word reports a threshold crossed: bits 15:0 upper, bits 31:16 critical or max.
volatile uint32_t native_thermal_over[THERMAL_CONTROLLERS];
// The hottest sensor of each group (ThermalProbe.h thermal_group_name) at the last sample, and since boot.
volatile int32_t native_thermal_now[THERMAL_GROUPS], native_thermal_peak[THERMAL_GROUPS];
// Per frequency domain: [0] running kHz, [1] voted kHz, [2] enable register bit 0, [3] dcvs control, [4..6] the words at 0x300, 0x304
// and 0x30c, [7] limit interrupt pending (0 or 1; 2: the distributor is not mapped).
volatile uint32_t native_thermal_lmh[3][8];
// Per frequency domain, the limit statistics: [0] busy samples, [1] limited ones, [2..4] minimum, mean and maximum temperature over
// the limited samples, [5..6] mean and maximum over the busy samples that were not limited (millidegrees, THERMAL_NONE when there is
// no such sample), [7] the lowest running frequency in a limited sample (kHz).
volatile int32_t native_thermal_limit[3][8];
// Idle time of a CPU since boot in mach units (the kernel helper the governor uses).
uint64_t sheng_cpu_idle_mach(int cpu) __attribute__((weak_import));
void sheng_thermal_probe(void);
void sheng_thermal_sample(void);
unsigned sheng_thermal_row(unsigned index, char *row, unsigned size);
}

namespace {
constexpr uint64_t kDomainBase[3] = {0x17d91000, 0x17d92000, 0x17d93000};
constexpr unsigned kDomainFirstCpu[3] = {0, 3, 7}, kDomainCpus[3] = {3, 4, 1};
constexpr unsigned kCpus = 8;
constexpr uint32_t kRegEnable = 0x0, kRegDomainState = 0x20, kRegDcvsCtrl = 0xb0, kRegFreqLut = 0x100, kRegPerfState = 0x320;
constexpr uint32_t kRegNearIntrClr[3] = {0x300, 0x304, 0x30c};
constexpr unsigned kLutEntries = 40;
constexpr uint32_t kXoKHz = 19200;
// The distributor's set-pending register that holds interrupt IDs 32 to 63, and the three limit interrupts in it (SPI n is ID 32 + n).
constexpr uint64_t kGicd = 0x17100000;
constexpr uint32_t kGicdIspendr1 = 0x204;
constexpr unsigned kLimitSpi[3] = {30, 31, 19};

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
Window gTm[THERMAL_CONTROLLERS], gSrot[THERMAL_CONTROLLERS], gDomain[3], gGicd;
uint32_t gCtrl[THERMAL_CONTROLLERS];
bool gReady;
volatile uint8_t gBusy;                 // one sampler at a time: a second caller leaves the values of the one in progress
unsigned gPeriodMs;
uint64_t gLastAbs, gLastIdle[kCpus];
thermal_limit gLimit[3];

bool mapWindow(Window &w, uint64_t base) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, THERMAL_WINDOW);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
uint32_t rd(const Window &w, uint32_t reg) {return w.va[reg / 4];}

void sampleTemperatures() {
    int32_t mc[THERMAL_CONTROLLERS][THERMAL_SENSORS];
    for (unsigned c = 0; c < THERMAL_CONTROLLERS; ++c) {
        uint32_t upper = 0, critical = 0;
        for (unsigned s = 0; s < THERMAL_SENSORS; ++s) {
            mc[c][s] = THERMAL_NONE;
            if (!thermal_sensor_enabled(gCtrl[c], s)) continue;
            uint32_t status[THERMAL_READ_TRIES];
            unsigned reads = 0;
            do status[reads] = rd(gTm[c], THERMAL_TM_STATUS + 4 * s);
            while (!(status[reads++] & THERMAL_STATUS_VALID) && reads < THERMAL_READ_TRIES);
            int32_t value = THERMAL_NONE;
            if (thermal_pick(status, reads, &value)) mc[c][s] = value;
            native_thermal_mc[c][s] = mc[c][s];
            // threshold bits of the last word read: bit 2 upper, bits 3 and 4 critical and max
            const uint32_t flags = (status[reads - 1] >> THERMAL_STATUS_FLAGS_SHIFT) & THERMAL_STATUS_FLAGS_MASK;
            if (flags & 4) upper |= 1u << s;
            if (flags & 0x18) critical |= 1u << s;
        }
        native_thermal_over[c] = upper | critical << 16;
    }
    int32_t now[THERMAL_GROUPS];
    thermal_groups(mc, now);
    for (unsigned g = 0; g < THERMAL_GROUPS; ++g) {
        native_thermal_now[g] = now[g];
        if (now[g] != THERMAL_NONE && (native_thermal_peak[g] == THERMAL_NONE || now[g] > native_thermal_peak[g])) native_thermal_peak[g] = now[g];
    }
}

// The busiest CPU of each domain over the time since the last sample, in per mille; false when there is no interval to measure over.
bool busyShares(unsigned busy[3]) {
    if (!sheng_cpu_idle_mach) return false;
    const uint64_t t = mach_absolute_time(), dt = t - gLastAbs;
    const bool measured = gLastAbs != 0 && dt != 0;
    gLastAbs = t;
    for (unsigned d = 0; d < 3; ++d) {
        busy[d] = 0;
        for (unsigned c = kDomainFirstCpu[d]; c < kDomainFirstCpu[d] + kDomainCpus[d]; ++c) {
            const uint64_t idle = sheng_cpu_idle_mach((int)c), spent = idle - gLastIdle[c];
            gLastIdle[c] = idle;
            if (!measured) continue;
            const unsigned share = spent >= dt ? 0 : (unsigned)(1000 - spent * 1000 / dt);
            if (share > busy[d]) busy[d] = share;
        }
    }
    return measured;
}

void sampleDomains() {
    unsigned busy[3] = {0, 0, 0};
    const bool measured = busyShares(busy);
    const uint32_t pending = gGicd.va ? rd(gGicd, kGicdIspendr1) : 0;
    for (unsigned d = 0; d < 3; ++d) {
        const Window &w = gDomain[d];
        if (!w.va) continue;
        const uint32_t enable = rd(w, kRegEnable) & 1, ctrl = rd(w, kRegDcvsCtrl);
        const uint32_t running = (rd(w, kRegDomainState) & 0xff) * kXoKHz;
        uint32_t voted = 0;
        const unsigned rows = (ctrl & 1) ? kDomainCpus[d] : 1;
        for (unsigned i = 0; i < rows; ++i) {
            const uint32_t level = rd(w, kRegPerfState + 4 * i);
            if (level >= kLutEntries) continue;
            const uint32_t khz = thermal_lut_khz(rd(w, kRegFreqLut + 4 * level));
            if (khz > voted) voted = khz;
        }
        native_thermal_lmh[d][0] = running;
        native_thermal_lmh[d][1] = voted;
        native_thermal_lmh[d][2] = enable;
        native_thermal_lmh[d][3] = ctrl;
        for (unsigned i = 0; i < 3; ++i) native_thermal_lmh[d][4 + i] = rd(w, kRegNearIntrClr[i]);
        native_thermal_lmh[d][7] = gGicd.va ? (pending >> kLimitSpi[d] & 1) : 2;
        if (!enable || !measured) continue;
        thermal_limit &l = gLimit[d];
        thermal_limit_add(&l, busy[d], running, voted, native_thermal_now[THERMAL_GROUP_CPU_D0 + d]);
        native_thermal_limit[d][0] = (int32_t)l.busy;
        native_thermal_limit[d][1] = (int32_t)l.limited;
        native_thermal_limit[d][2] = l.limited_min;
        native_thermal_limit[d][3] = thermal_limit_limited_mean(&l);
        native_thermal_limit[d][4] = l.limited_max;
        native_thermal_limit[d][5] = thermal_limit_free_mean(&l);
        native_thermal_limit[d][6] = l.free_max;
        native_thermal_limit[d][7] = (int32_t)l.lowest_khz;
    }
}

void samplerThread(void *, wait_result_t) {
    for (;;) {
        IOSleep(gPeriodMs);
        sheng_thermal_sample();
    }
}

}  // namespace

extern "C" void sheng_thermal_sample(void) {
    if (!gReady || __atomic_test_and_set(&gBusy, __ATOMIC_ACQUIRE)) return;
    sampleTemperatures();
    sampleDomains();
    native_thermal_info[1] = native_thermal_info[1] + 1;
    native_thermal_info[0] = 2;
    __atomic_clear(&gBusy, __ATOMIC_RELEASE);
}

extern "C" void sheng_thermal_probe(void) {
    unsigned mode = 0, periodMs = 0;
    PE_parse_boot_argn("thermal", &mode, sizeof(mode));
    PE_parse_boot_argn("thermal-ms", &periodMs, sizeof(periodMs));
    if (!mode || gReady || native_thermal_info[0]) return;
    native_thermal_info[2] = (mode & 0xff) | periodMs << 8;
    for (unsigned c = 0; c < THERMAL_CONTROLLERS; ++c) {
        if (!mapWindow(gTm[c], thermal_tm_base[c])) {native_thermal_info[0] = 0x10 + c; return;}
        if (!mapWindow(gSrot[c], thermal_srot_base[c])) {native_thermal_info[0] = 0x13 + c; return;}
        native_thermal_info[3 + c] = rd(gSrot[c], THERMAL_SROT_VERSION);
        gCtrl[c] = rd(gSrot[c], THERMAL_SROT_CTRL);
        native_thermal_info[6 + c] = gCtrl[c];
        for (unsigned s = 0; s < THERMAL_SENSORS; ++s) native_thermal_mc[c][s] = THERMAL_NONE;
    }
    for (unsigned g = 0; g < THERMAL_GROUPS; ++g) {native_thermal_now[g] = THERMAL_NONE; native_thermal_peak[g] = THERMAL_NONE;}
    for (unsigned d = 0; d < 3; ++d) {
        mapWindow(gDomain[d], kDomainBase[d]);
        thermal_limit_init(&gLimit[d]);
        for (unsigned i = 2; i < 7; ++i) native_thermal_limit[d][i] = THERMAL_NONE;
    }
    mapWindow(gGicd, kGicd);
    native_thermal_info[0] = 1;
    gReady = true;
    sheng_thermal_sample();
    if (periodMs) {
        gPeriodMs = periodMs < 10 ? 10 : periodMs;   // a sample is about seventy register reads; faster than this measures the probe
        thread_t thread = nullptr;
        if (kernel_thread_start(samplerThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    }
}

// The boot log's rows, one per index from 0 (ThermalProbe.h describes each): [thermal], [thermal lmh], [thermal limit] and one
// [thermal raw] per controller. Returns the row's length, 0 when there is no row with that index.
extern "C" unsigned sheng_thermal_row(unsigned index, char *row, unsigned size) {
    if (!gReady || !row || size < 2) return 0;
    if (index == 0) {
        int32_t now[THERMAL_GROUPS], peak[THERMAL_GROUPS];
        for (unsigned g = 0; g < THERMAL_GROUPS; ++g) {now[g] = native_thermal_now[g]; peak[g] = native_thermal_peak[g];}
        return thermal_row_groups(row, size, native_thermal_info[1], now, peak);
    }
    if (index == 1) {
        uint32_t lmh[3][8];
        for (unsigned d = 0; d < 3; ++d) for (unsigned i = 0; i < 8; ++i) lmh[d][i] = native_thermal_lmh[d][i];
        return thermal_row_lmh(row, size, lmh);
    }
    if (index == 2) {
        int32_t limit[3][8];
        for (unsigned d = 0; d < 3; ++d) for (unsigned i = 0; i < 8; ++i) limit[d][i] = native_thermal_limit[d][i];
        return thermal_row_limit(row, size, limit);
    }
    const unsigned c = index - 3;
    if (c >= THERMAL_CONTROLLERS) return 0;
    int32_t mc[THERMAL_SENSORS];
    for (unsigned s = 0; s < THERMAL_SENSORS; ++s) mc[s] = native_thermal_mc[c][s];
    return thermal_row_raw(row, size, c, native_thermal_info[3 + c], native_thermal_info[6 + c], native_thermal_over[c], mc);
}
