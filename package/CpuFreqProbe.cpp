// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off CPU frequency control for the SM8550 EPSS frequency domains and the EPSS L3. `sheng-cpufreq=<n>`:
//   1  read-only: report each domain's enable bit, current/requested level and its highest LUT entry
//   2  additionally request that highest non-turbo level in domain 0 (the boot CPU's cluster)
//   3  the same in every domain once it reads enabled, and the highest L3 level
//   4  mode 3's L3 vote, and a governor for the domains (below)
// Optional `sheng-cpufreq-cap=<MHz>`: mode 3 requests the highest non-turbo level at or below it (domains and L3).
// Register layout is the qcom-cpufreq-hw "epss" data of the pinned mainline tree (reg_enable 0x0, reg_domain_state 0x20,
// reg_dcvs_ctrl 0xb0, reg_freq_lut 0x100 with 4-byte rows, reg_perf_state 0x320, per-core perf_state rows +4*i for the
// policy's other CPUs when dcvs_ctrl bit 0 is set); the three domain windows come from the cpufreq@17d91000 node of
// sm8550.dtsi, their CPU counts from the cpus' qcom,freq-domain (3, 4, 1). The L3 is osm-l3.c "qcom,epss-l3" (the sm8550
// node interconnect@17d90000): REG_ENABLE 0x0, LUT 0x100 with 4-byte rows, vote EPSS_REG_L3_VOTE 0x90; only those three
// offsets are touched there. LUT row: bits 31:30 source, 18:16 core count (1 = turbo), 7:0 L value; an L value is a
// multiple of the 19.2 MHz bi_tcxo_div2 clock, source 0 is GPLL0/2.
// A cluster whose cores are all off reads enable 0 and state 0 (int-143 read at 7 s, before the secondary cores ran), so the
// probe maps the windows once and sheng_cpufreq_poll(), called every log round, votes each domain the first time it reads
// enabled and refreshes the row. Nothing here is validated on the device until a boot has shown it.
// Mode 4's governor runs every 16 ms: a CPU's busy share is 1 - its idle time over the window (sheng_cpu_idle_mach, a kernel helper
// over recount), a domain's the share of its busiest CPU. The domain gets the lowest LUT level at or above 1.25 x busy x its current
// frequency (schedutil's margin), the highest allowed level once a CPU is 90 % busy (a hispeed jump); it goes up at once and down only
// after 4 windows in a row that asked for less. Levels stay between `sheng-cpufreq-min=<MHz>` (default 0) and mode 3's highest
// non-turbo level (or the cap). The XNU CPU numbers follow the DTS cpu order: cpus 0-2 are the A510 cores (domain 0, the E cluster),
// cpus 3-6 the A715/A710 cores (domain 1), cpu 7 the X3 (domain 2).
// Touch boost: when the touch report count moves, the P domains (1, 2) hold their highest allowed level until
// `sheng-cpufreq-boost-ms=<ms>` (default 1000, 0 = off) after the last report, so an app launch or a scroll starts at full speed
// instead of ramping by 1.25x per window. Outside a boost the P domains stop at the lowest level at or above half their highest
// one, and go lower only after 12 windows in a row under 10 % busy.
// Frame floor: GPU submits (NativeAdreno's submit count) in 2 of the last 4 windows keep every domain at or above
// `sheng-cpufreq-frame-pct=<n>` (default 60) % of its highest level for `sheng-cpufreq-frame-ms=<ms>` (default 300, 0 = off), the E
// domain included: the render server also runs on the A510 cores (i566 profile: 23 % of backboardd's samples on cpus 0-2) and they
// sat at 307 MHz. i565 held the P domains at the top during frames and counted a domain's busy share as the sum of its CPUs: d1 then
// never left 2803 MHz (99 % busy), so both are gone; a touch still holds the P domains at the top.
// Throttle count: in a window where a domain's busiest CPU was at least half busy, its domain state (the frequency the hardware
// runs, which also drops while the whole cluster idles) under 80 % of the vote counts as a throttled window, with the lowest seen.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <mach/mach_time.h>

extern "C" {
char native_cpufreq_row[120];
volatile uint32_t native_cpufreq_stage;
// Mode 4, per domain over the last 10 s: level changes, mean MHz, mean busy % of the busiest CPU
volatile uint32_t native_cpufreq_gov[9];
// Mode 4: touch boosts started since boot
volatile uint32_t native_cpufreq_boosts;
// Mode 4: frame floors started, 16 ms windows spent in one
volatile uint32_t native_cpufreq_frame[2];
// Mode 4, per domain: busy windows (busiest CPU >= 50 %), throttled ones among them, lowest domain state (MHz) in a throttled one
volatile uint32_t native_cpufreq_throttle[9];
extern volatile uint32_t native_adreno_method_stats[20][4] __attribute__((weak_import));
extern volatile uint32_t native_hid_input_events[8] __attribute__((weak_import));
uint64_t sheng_cpu_idle_mach(int cpu);
}

namespace {
constexpr uint64_t kDomainBase[3] = {0x17d91000, 0x17d92000, 0x17d93000};
constexpr unsigned kDomainCpus[3] = {3, 4, 1};
constexpr uint64_t kL3Base = 0x17d90000;
constexpr uint64_t kWindowSize = 0x1000;
constexpr uint32_t kRegEnable = 0x0, kRegDomainState = 0x20, kRegDcvsCtrl = 0xb0, kRegFreqLut = 0x100, kRegPerfState = 0x320;
constexpr uint32_t kRegL3Vote = 0x90;
constexpr unsigned kLutEntries = 40;
constexpr uint32_t kXoKHz = 19200;        // bi_tcxo_div2
constexpr uint32_t kGpllHalfKHz = 300000; // GPLL0 600 MHz / CLK_HW_DIV 2

struct Window {
    IOMemoryMap *map;
    uintptr_t va;
    int voted;          // level written, -1 none
    uint32_t votedKHz;
    uint32_t preKHz;    // domain state the first time it read enabled, before the vote
};
Window gDomain[3], gL3;
unsigned gMode;
uint32_t gCapKHz;
IOLock *gLock;

uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t lutKHz(uint32_t row) {return (row >> 30) ? kXoKHz * (row & 0xff) : kGpllHalfKHz;}

// Highest non-turbo LUT level (at or below the cap when one is set); the table ends at two equal frequencies.
int pickLevel(uintptr_t va, bool turboMatters, uint32_t *khzOut, unsigned *rowsOut) {
    int best = -1; uint32_t bestKHz = 0, prev = 0; unsigned rows = 0;
    for (unsigned i = 0; i < kLutEntries; ++i) {
        uint32_t row = rd32(va, kRegFreqLut + i * 4);
        uint32_t khz = lutKHz(row);
        if (i > 0 && khz == prev) break;
        rows = i + 1;
        prev = khz;
        if (turboMatters && ((row >> 16) & 7) == 1) continue;
        if (gCapKHz && khz > gCapKHz) continue;
        if (khz >= bestKHz) {best = (int)i; bestKHz = khz;}
    }
    if (khzOut) *khzOut = bestKHz;
    if (rowsOut) *rowsOut = rows;
    return best;
}

bool mapWindow(Window &w, uint64_t base) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, kWindowSize);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? static_cast<uintptr_t>(w.map->getVirtualAddress()) : 0;
    w.voted = -1;
    return w.va != 0;
}

void voteDomain(unsigned d) {
    Window &w = gDomain[d];
    uint32_t khz = 0;
    int level = pickLevel(w.va, true, &khz, nullptr);
    if (level < 0) return;
    w.preKHz = (rd32(w.va, kRegDomainState) & 0xff) * kXoKHz;
    wr32(w.va, kRegPerfState, (uint32_t)level);
    if (rd32(w.va, kRegDcvsCtrl) & 1)
        for (unsigned c = 1; c < kDomainCpus[d]; ++c) wr32(w.va, kRegPerfState + c * 4, (uint32_t)level);
    w.voted = level;
    w.votedKHz = khz;
}

void voteL3() {
    uint32_t khz = 0;
    int level = pickLevel(gL3.va, false, &khz, nullptr);
    if (level < 0) return;
    wr32(gL3.va, kRegL3Vote, (uint32_t)level);
    gL3.voted = level;
    gL3.votedKHz = khz;
}

void refreshRow() {
    char *out = native_cpufreq_row;
    const size_t cap = sizeof(native_cpufreq_row);
    unsigned n = (unsigned)snprintf(out, cap, "CPUFREQ");
    for (unsigned d = 0; d < 3 && n < cap; ++d) {
        Window &w = gDomain[d];
        if (!w.va) {n += (unsigned)snprintf(out + n, cap - n, " d%u:nomap", d); continue;}
        uint32_t enable = rd32(w.va, kRegEnable) & 1, state = (rd32(w.va, kRegDomainState) & 0xff) * kXoKHz / 1000;
        if (w.voted >= 0)
            n += (unsigned)snprintf(out + n, cap - n, " d%u:e%u c%u v%d@%u pre%u", d, enable, state, w.voted, w.votedKHz / 1000,
                                    w.preKHz / 1000);
        else
            n += (unsigned)snprintf(out + n, cap - n, " d%u:e%u c%u p%u", d, enable, state, rd32(w.va, kRegPerfState));
    }
    if (n < cap) {
        if (!gL3.va) snprintf(out + n, cap - n, " L3:nomap");
        else if (gL3.voted >= 0) snprintf(out + n, cap - n, " L3:e%u v%d@%u", rd32(gL3.va, kRegEnable) & 1, gL3.voted, gL3.votedKHz / 1000);
        else snprintf(out + n, cap - n, " L3:e%u", rd32(gL3.va, kRegEnable) & 1);
    }
}

// Mode 4: the governor (see the header).
constexpr int kCpuDomain[] = {0, 0, 0, 1, 1, 1, 1, 2};
constexpr unsigned kCpus = sizeof kCpuDomain / sizeof kCpuDomain[0];
struct Levels {
    unsigned count;
    uint8_t index[kLutEntries];   // LUT rows in ascending frequency, non-turbo, within the floor and the cap
    uint32_t khz[kLutEntries];
};
Levels gLevels[3];
uint32_t gFloorKHz;
unsigned gBoostMs, gFrameMs, gFramePct;

void buildLevels(unsigned d) {
    Levels &l = gLevels[d];
    l.count = 0;
    uint32_t prev = 0;
    for (unsigned i = 0; i < kLutEntries; ++i) {
        const uint32_t row = rd32(gDomain[d].va, kRegFreqLut + i * 4), khz = lutKHz(row);
        if (i > 0 && khz == prev) break;
        prev = khz;
        if (((row >> 16) & 7) == 1 || (gCapKHz && khz > gCapKHz)) continue;
        unsigned at = l.count;
        while (at > 0 && l.khz[at - 1] > khz) {l.khz[at] = l.khz[at - 1]; l.index[at] = l.index[at - 1]; --at;}
        l.khz[at] = khz; l.index[at] = (uint8_t)i; ++l.count;
    }
    // Drop the levels under the floor, keeping at least the highest.
    unsigned first = 0;
    while (first + 1 < l.count && l.khz[first] < gFloorKHz) ++first;
    for (unsigned i = first; i < l.count; ++i) {l.khz[i - first] = l.khz[i]; l.index[i - first] = l.index[i];}
    l.count -= first;
}

void setLevel(unsigned d, unsigned at) {
    Window &w = gDomain[d];
    const uint32_t level = gLevels[d].index[at];
    wr32(w.va, kRegPerfState, level);
    if (rd32(w.va, kRegDcvsCtrl) & 1)
        for (unsigned c = 1; c < kDomainCpus[d]; ++c) wr32(w.va, kRegPerfState + c * 4, level);
    w.voted = (int)level;
    w.votedKHz = gLevels[d].khz[at];
}

void governorThread(void *, wait_result_t) {
    uint64_t lastIdle[kCpus] = {}, lastT = mach_absolute_time();
    for (unsigned c = 0; c < kCpus; ++c) lastIdle[c] = sheng_cpu_idle_mach((int)c);
    unsigned cur[3] = {}, down[3] = {}, quiet[3] = {};
    bool ready[3] = {};
    uint64_t boostAbs = 0, boostEnd = 0;
    nanoseconds_to_absolutetime((uint64_t)gBoostMs * 1000000, &boostAbs);
    uint32_t touches = &native_hid_input_events ? native_hid_input_events[6] : 0;
    uint64_t frameAbs = 0, frameEnd = 0;
    nanoseconds_to_absolutetime((uint64_t)gFrameMs * 1000000, &frameAbs);
    uint32_t submits = &native_adreno_method_stats ? native_adreno_method_stats[5][0] : 0, recent = 0;
    uint64_t winNs = 0, khzNs[3] = {}, busyNs[3] = {};
    uint32_t changes[3] = {};
    for (;;) {
        IOSleep(16);
        const uint64_t t = mach_absolute_time(), dt = t - lastT;
        lastT = t;
        if (!dt) continue;
        if (boostAbs && &native_hid_input_events && native_hid_input_events[6] != touches) {
            touches = native_hid_input_events[6];
            if (t >= boostEnd) native_cpufreq_boosts = native_cpufreq_boosts + 1;
            boostEnd = t + boostAbs;
        }
        if (frameAbs && &native_adreno_method_stats) {
            const uint32_t now = native_adreno_method_stats[5][0];
            recent = ((recent << 1) | (now != submits)) & 15;
            submits = now;
            if (__builtin_popcount(recent) >= 2) {
                if (t >= frameEnd) native_cpufreq_frame[0] = native_cpufreq_frame[0] + 1;
                frameEnd = t + frameAbs;
            }
        }
        const bool framing = t < frameEnd;
        if (framing) native_cpufreq_frame[1] = native_cpufreq_frame[1] + 1;
        const bool boost = t < boostEnd;
        uint64_t busy[3] = {};   // per mille of the busiest CPU
        for (unsigned c = 0; c < kCpus; ++c) {
            const uint64_t idle = sheng_cpu_idle_mach((int)c), di = idle - lastIdle[c];
            lastIdle[c] = idle;
            const uint64_t b = di >= dt ? 0 : (dt - di) * 1000 / dt;
            const int d = kCpuDomain[c];
            if (b > busy[d]) busy[d] = b;
        }
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(dt, &ns);
        winNs += ns;
        IOLockLock(gLock);
        for (unsigned d = 0; d < 3; ++d) {
            Window &w = gDomain[d];
            if (!w.va || !(rd32(w.va, kRegEnable) & 1)) continue;
            Levels &l = gLevels[d];
            if (!ready[d]) {
                buildLevels(d);
                if (!l.count) continue;
                w.preKHz = (rd32(w.va, kRegDomainState) & 0xff) * kXoKHz;
                cur[d] = l.count - 1;
                setLevel(d, cur[d]);
                ready[d] = true;
            }
            unsigned want = l.count - 1;
            if (busy[d] < 900) {
                const uint64_t need = l.khz[cur[d]] * busy[d] * 125 / 100000;
                want = 0;
                while (want + 1 < l.count && l.khz[want] < need) ++want;
            }
            if (busy[d] >= 500 && cur[d] < l.count) {
                const uint32_t stateKHz = (rd32(w.va, kRegDomainState) & 0xff) * kXoKHz;
                volatile uint32_t *th = &native_cpufreq_throttle[d * 3];
                th[0] = th[0] + 1;
                if (stateKHz && stateKHz * 5 < l.khz[cur[d]] * 4) {
                    th[1] = th[1] + 1;
                    if (!th[2] || stateKHz / 1000 < th[2]) th[2] = stateKHz / 1000;
                }
            }
            if (framing)
                while (want + 1 < l.count && (uint64_t)l.khz[want] * 100 < (uint64_t)l.khz[l.count - 1] * gFramePct) ++want;
            if (d > 0) {
                quiet[d] = busy[d] < 100 ? quiet[d] + 1 : 0;
                if (boost) want = l.count - 1;
                else if (quiet[d] < 12)
                    while (want + 1 < l.count && l.khz[want] * 2 < l.khz[l.count - 1]) ++want;
            }
            if (want > cur[d]) {
                cur[d] = want; down[d] = 0; setLevel(d, want); ++changes[d];
            } else if (want < cur[d]) {
                if (++down[d] >= 4) {cur[d] = want; down[d] = 0; setLevel(d, want); ++changes[d];}
            } else {
                down[d] = 0;
            }
            khzNs[d] += (uint64_t)l.khz[cur[d]] / 1000 * ns;
            busyNs[d] += busy[d] * ns / 10;
        }
        IOLockUnlock(gLock);
        if (winNs >= 10000000000ull) {
            for (unsigned d = 0; d < 3; ++d) {
                native_cpufreq_gov[d * 3] = changes[d];
                native_cpufreq_gov[d * 3 + 1] = (uint32_t)(khzNs[d] / winNs);
                native_cpufreq_gov[d * 3 + 2] = (uint32_t)(busyNs[d] / winNs);
                changes[d] = 0; khzNs[d] = busyNs[d] = 0;
            }
            winNs = 0;
        }
    }
}
}

extern "C" void sheng_cpufreq_poll(void) {
    if (!gMode || !gLock) return;
    IOLockLock(gLock);
    for (unsigned d = 0; d < 3; ++d) {
        Window &w = gDomain[d];
        bool wanted = gMode == 3 || (gMode == 2 && d == 0);
        if (w.va && wanted && w.voted < 0 && (rd32(w.va, kRegEnable) & 1)) voteDomain(d);
    }
    if (gL3.va && gMode >= 3 && gL3.voted < 0 && (rd32(gL3.va, kRegEnable) & 1)) voteL3();
    refreshRow();
    native_cpufreq_stage = 2;
    IOLockUnlock(gLock);
}

extern "C" void sheng_cpufreq_probe(void) {
    unsigned mode = 0, capMHz = 0, minMHz = 0, boostMs = 1000, frameMs = 300, framePct = 60;
    PE_parse_boot_argn("sheng-cpufreq", &mode, sizeof(mode));
    PE_parse_boot_argn("sheng-cpufreq-cap", &capMHz, sizeof(capMHz));
    PE_parse_boot_argn("sheng-cpufreq-min", &minMHz, sizeof(minMHz));
    PE_parse_boot_argn("sheng-cpufreq-boost-ms", &boostMs, sizeof(boostMs));
    PE_parse_boot_argn("sheng-cpufreq-frame-ms", &frameMs, sizeof(frameMs));
    gBoostMs = boostMs;
    PE_parse_boot_argn("sheng-cpufreq-frame-pct", &framePct, sizeof(framePct));
    gFrameMs = frameMs;
    gFramePct = framePct > 100 ? 100 : framePct;
    gFloorKHz = minMHz * 1000;
    if (!mode || gLock) return;
    gLock = IOLockAlloc();
    if (!gLock) return;
    native_cpufreq_stage = 1;
    gCapKHz = capMHz * 1000;
    for (unsigned d = 0; d < 3; ++d) mapWindow(gDomain[d], kDomainBase[d]);
    if (mode >= 3) mapWindow(gL3, kL3Base);
    gMode = mode;
    sheng_cpufreq_poll();
    if (mode >= 4) {
        thread_t thread = nullptr;
        if (kernel_thread_start(governorThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    }
}
