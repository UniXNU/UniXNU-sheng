// SPDX-License-Identifier: BSD-2-Clause-Patent
// The sheng board's wall clock for the platform expert (ArmDTPlatform armdt_platform_clock_register: a read function the kernel
// calls when it initialises the calendar and at wake, and a set function it calls whenever the system sets the time).
// Wall time is the PMK8550 seconds counter plus an offset; BoardClock.h has the registers, their sources and the arithmetic.
//
// What is known about the hardware (boot logs of macOS runs 53 to 61, the [pmic rtc] and [reset] rows): the counter is enabled and
// advances one per second; it kept counting through the warm restarts between runs 53, 54 and 55 and between runs 56 and 61 (timed
// restarts, a panic restart, the boots of the log reader in between), and started again from 0 once, when run 55 was ended by a
// restart forced with the keys; the SDAM that holds the offset cell belongs to this execution environment (owner 0) and its reboot
// reason byte is written from here at every restart into the boot loader. What is not known: whether the four bytes at 0xbc are
// free on this firmware, whether they survive the restart that zeroes the counter, and what a power-off does to either. So the clock
// does nothing unless the boot argument asks:
//   board-clock=<bits>, default 0
//     any bit    measure: read the counter and the cell when the kernel asks for the time and when the system sets it, keep the offset
//                in memory (a later read, at wake, then answers), and show everything in the boot log's [board clock] row. Read only;
//                board-clock=1 is this alone.
//     bit 1 (2)  start the calendar from the counter plus the stored offset when the cell holds one. The platform expert still never
//                starts before the kernel's build date.
//     bit 2 (4)  when the system sets the time, store the new offset in the cell. Written only when this execution environment owns
//                the SDAM and the cell is empty or already holds an offset (BoardClock.h bc_cell), in one transfer, and read back.
// With 0 the hooks are registered but touch no hardware: the calendar starts at the build date as before, and the row only counts
// the times the system set the clock.
// A stored offset with a counter that restarted gives a time in the past (by the counter's value at the restart), never in the
// future; the next time the system sets the clock, the offset is corrected.
//
// The clock registers from a static initialiser, because the kernel reads the time (bsd_init, IOKitInitializeTime) before the board
// probes run. The arbiter is mapped the first time a hook needs it. The boot log samples the same two peripherals through the same
// observer channels (WifiProbe.cpp sheng_charger_pmic_sample); the stable reads of BoardClock.h keep a transfer of that sampler
// from being taken for this code's answer.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <pexpert/pexpert.h>
#include "BoardClock.h"

extern "C" {
// ArmDTPlatform. Either function may be null.
void armdt_platform_clock_register(int (*read)(uint64_t *seconds), void (*set)(uint64_t seconds)) __attribute__((weak_import));
// [0] state bits (1 registered, 2 arbiter mapped, 4 the kernel asked for the time, 8 the system set the time, 0x10 the arbiter is
// not the layout this code knows or did not map), [1] the board-clock argument, [2] the clock's APID | owner << 16 (APID 0xffff:
// the arbiter maps no such peripheral), [3] the same for the SDAM, [4] result of the first read for the kernel (BoardClock.h results,
// 0xff not tried), [5] the counter then, [6] the cell then, [7] the seconds given to the kernel (0 none), [8] times the system set
// the clock, [9] the seconds of the last set, [10] the counter then, [11] the offset for it, [12] result of the last store (0 stored,
// 10 the cell already held it, 0xff not tried), [13] the cell after it, [14] stores done, [15] the new offset less the cell before
// the last set (signed seconds: how far the stored clock was off; 0 when the cell held no offset).
volatile uint32_t native_board_clock[16];
void sheng_board_clock_probe(void);
unsigned sheng_board_clock_row(char *row, unsigned size);
}

namespace {
constexpr uint64_t kSpmiCore = 0xc400000, kSpmiObs = 0xc440000, kSpmiChn = 0xc500000, kSpmiCnfg = 0xc42d000;
enum : uint32_t {kUseStored = 2, kStore = 4};
enum : uint32_t {kRegistered = 1, kMapped = 2, kAsked = 4, kWasSet = 8, kNoArbiter = 0x10};

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
Window gCore, gCnfg, gObs, gChannel;      // gChannel: the SDAM's write channel only
br_arbiter gArbiter;
int gRtc = -1, gSdam = -1, gRtcOwner = -1, gSdamOwner = -1;
uint32_t gMode;
bool gModeRead, gOpened;
uint32_t gOffset;                         // the offset in memory: 0 none
volatile uint8_t gBusy;

void flag(uint32_t bit) {native_board_clock[0] = native_board_clock[0] | bit;}

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}

uint32_t clockMode() {
    if (!gModeRead) {
        uint32_t value = 0;
        PE_parse_boot_argn("board-clock", &value, sizeof(value));
        gMode = value;
        gModeRead = true;
        native_board_clock[1] = value;
        native_board_clock[4] = BC_NOT_TRIED;
        native_board_clock[12] = BC_NOT_TRIED;
    }
    return gMode;
}

// The hooks are rare and short; one waits briefly for the other instead of sleeping, and gives up rather than block the caller.
bool enter() {
    for (unsigned i = 0; i < 2000; ++i) {
        if (!__atomic_test_and_set(&gBusy, __ATOMIC_ACQUIRE)) return true;
        IODelay(10);
    }
    return false;
}
void leave() {__atomic_clear(&gBusy, __ATOMIC_RELEASE);}

// Maps the arbiter and finds the two peripherals, once. The write channel is mapped only when storing is asked for and allowed.
bool openArbiter() {
    if (gOpened) return gRtc >= 0;
    gOpened = true;
    gArbiter.ee = 0;
    if (!mapWindow(gCore, kSpmiCore, 0x3000) || !mapWindow(gCnfg, kSpmiCnfg, 0x4000)) {flag(kNoArbiter); return false;}
    gArbiter.core = gCore.va;
    gArbiter.cnfg = gCnfg.va;
    const uint32_t version = gCore.va[0], apids = gCore.va[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
    if (version < BR_VERSION_V7_MIN || version >= BR_VERSION_V8_MIN || !apids || apids > 1024 ||
        !mapWindow(gObs, kSpmiObs, ((uint64_t)BR_OBS_APID_STRIDE * apids + 0xfff) & ~0xfffull)) {flag(kNoArbiter); return false;}
    gRtc = bc_find(&gArbiter, apids, BC_RTC_PPID, &gRtcOwner);
    gSdam = bc_find(&gArbiter, apids, BC_SDAM_PPID, &gSdamOwner);
    native_board_clock[2] = gRtc < 0 ? 0xffff : (uint32_t)gRtc | (uint32_t)gRtcOwner << 16;
    native_board_clock[3] = gSdam < 0 ? 0xffff : (uint32_t)gSdam | (uint32_t)gSdamOwner << 16;
    if ((gMode & kStore) && gSdam >= 0 && gSdamOwner == gArbiter.ee)
        mapWindow(gChannel, kSpmiChn + (uint64_t)BR_CHNL_APID_STRIDE * (uint32_t)gSdam, BR_CHNL_APID_STRIDE);
    flag(kMapped);
    return gRtc >= 0;
}

// The time for the kernel: 0 with *seconds set, or nonzero when this clock has none (the platform expert then uses its floor).
int clockRead(uint64_t *seconds) {
    const uint32_t m = clockMode();
    flag(kAsked);
    if (!m || !seconds || !enter()) return 1;
    const bool first = native_board_clock[4] == BC_NOT_TRIED;
    uint32_t counter = 0, cell = 0;
    uint64_t now = 0;
    int rc = openArbiter() ? bc_counter(gObs.va, &gArbiter, gRtc, &counter) : 1;
    if (!rc && gOffset) {
        // the system has set the time since boot: its offset is the newest
        rc = bc_time(counter, gOffset, &now);
    } else if (!rc) {
        rc = gSdam < 0 ? 1 : bc_read_stable(gObs.va, &gArbiter, gSdam, BC_OFFSET_CELL, 4, &cell);
        if (!rc) rc = bc_cell(counter, cell, UINT32_MAX);
        if (!rc) rc = bc_time(counter, cell, &now);
        if (!rc && !(m & kUseStored)) now = 0;   // measured, not used
    }
    if (first) {
        native_board_clock[5] = counter;
        native_board_clock[6] = cell;
        native_board_clock[4] = (uint32_t)rc;
        native_board_clock[7] = (uint32_t)now;
    }
    leave();
    if (rc || !now) return 1;
    *seconds = now;
    return 0;
}

// The system set the time: the offset that makes the counter read as that time, kept in memory and, when asked, in the cell.
void clockSet(uint64_t seconds) {
    const uint32_t m = clockMode();
    flag(kWasSet);
    native_board_clock[8] = native_board_clock[8] + 1;
    native_board_clock[9] = (uint32_t)seconds;
    if (!m || !enter()) return;
    uint32_t counter = 0, offset = 0, cell = 0;
    int rc = openArbiter() ? bc_counter(gObs.va, &gArbiter, gRtc, &counter) : 1;
    native_board_clock[10] = counter;
    if (!rc) rc = bc_offset(seconds, counter, &offset);
    native_board_clock[11] = offset;
    if (rc) {leave(); return;}
    gOffset = offset;
    if (gSdam >= 0 && !bc_read_stable(gObs.va, &gArbiter, gSdam, BC_OFFSET_CELL, 4, &cell)) {
        const int held = bc_cell(counter, cell, seconds + BC_SET_BACK);
        native_board_clock[13] = cell;
        native_board_clock[15] = held == BC_OK ? offset - cell : 0;
        if (m & kStore) {
            uint32_t after = cell;
            const int stored = held == BC_FOREIGN ? BC_FOREIGN : cell == offset ? BC_CURRENT : gSdamOwner != gArbiter.ee ? BC_NOT_OWNER :
                               !gChannel.va ? 1 : bc_store(gObs.va, gChannel.va, &gArbiter, gSdam, gSdamOwner, offset, &after);
            native_board_clock[12] = (uint32_t)stored;
            native_board_clock[13] = after;
            if (stored == BC_OK) native_board_clock[14] = native_board_clock[14] + 1;
        }
    }
    leave();
}

void registerClock() {
    if ((native_board_clock[0] & kRegistered) || !armdt_platform_clock_register) return;
    armdt_platform_clock_register(clockRead, clockSet);
    flag(kRegistered);
}
// Runs with the kernel's other static initialisers, before the platform expert starts.
struct Registration {Registration() {registerClock();}} gRegistration;
}  // namespace

// For the board's probe list: registers the clock if the static initialiser has not, and reads the argument for the boot log.
extern "C" void sheng_board_clock_probe(void) {
    registerClock();
    clockMode();
}

// The boot log's row (BoardClock.h bc_row); 0 until the kernel has asked for the time or the system has set it.
extern "C" unsigned sheng_board_clock_row(char *row, unsigned size) {
    if (!row || size < 2 || !(native_board_clock[0] & (kAsked | kWasSet))) return 0;
    uint32_t values[16];
    for (unsigned i = 0; i < 16; ++i) values[i] = native_board_clock[i];
    return bc_row(row, size, values);
}
