// SPDX-License-Identifier: BSD-2-Clause-Patent
// sheng's wake sources for macOS platform sleep (boot-arg platform-sleep=1), given to ArmCpu (drivers/ArmCpu, armcpu_sleep_register),
// which calls them on the boot core with interrupts masked while the other cores are off: the lid (TLMM GPIO 171 on the TLMM summary
// interrupt) and the power key (ButtonProbe.cpp, sheng_buttons_sleep_check). The logic is ShengSleep.h's.
// - prepare: saves the 210 pins' INTR_CFG, turns every enable off but the lid's (level high when the lid was closed as the sleep began).
// - should_wake: the lid's level, or the power key's check; a lid read closed has its status cleared, so the summary drops.
// - restore: writes the saved INTR_CFG back. Statuses latched while asleep stay, for their drivers once interrupts are on again.
// A system sleep/wake interest reads the lid as the sleep begins and, after a wake by the lid or the power key, claims the wake and asks
// the power manager for user activity, so that the system wakes fully (display on) rather than dark; it logs the wake and ArmCpu's
// counters ("sheng sleep:").
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMessage.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#if defined(XNU_TARGET_OS_OSX) && XNU_TARGET_OS_OSX
#include <IOKit/pwr_mgt/RootDomain.h>
#endif
extern "C" {
#include "ShengSleep.h"
#include "TouchBoard.h"
}

// ArmCpuSleep.h's layout, declared here: the board does not build against the driver's headers.
struct armcpu_sleep_board {
    void (*prepare)(void);
    int (*should_wake)(uint32_t intid);
    void (*restore)(void);
    const uint32_t *wake_spis;
    uint32_t wake_spi_count;
    void (*tick)(void);
    uint32_t tick_s;
};
struct armcpu_sleep_stats {
    uint32_t phase, entries, spurious, last_intid, last_reason, cpu_off_failures;
    uint64_t asleep_ticks;
    uint32_t masked, ticks, unarmed;
};
// ArmCpuSleep.h's ARMCPU_SLEEP_BOARD_SIZE and ARMCPU_SLEEP_STATS_SIZE, which its host test checks on the driver's side.
static_assert(sizeof(armcpu_sleep_board) == 56, "armcpu_sleep_board differs from ArmCpuSleep.h");
static_assert(sizeof(armcpu_sleep_stats) == 48, "armcpu_sleep_stats differs from ArmCpuSleep.h");
extern "C" int armcpu_sleep_register(const struct armcpu_sleep_board *) __attribute__((weak_import));
extern "C" volatile struct armcpu_sleep_stats armcpu_sleep_stats __attribute__((weak_import));
extern "C" int sheng_buttons_sleep_check(void) __attribute__((weak_import));   // ButtonProbe.cpp
extern "C" void boot_reset_watchdog_ping(void) __attribute__((weak_import));   // BootReset.c
extern "C" {extern volatile int32_t native_buttons_stats[10] __attribute__((weak_import));}
extern "C" {extern volatile int32_t native_keyboard_stats[17] __attribute__((weak_import));}
// [0] stage (1 probing, 2 registered, negatives: the step that failed), [1] sleeps, [2] lid wakes, [3] power wakes, [4] lid reads closed
// while asleep, [5] last registration answer, [6] will-sleep notifications, [7] has-powered-on notifications
extern "C" {volatile int32_t sheng_sleep_stats[8];}

namespace {
constexpr uint32_t kIo = 0x4, kIntrCfg = 0x8, kIntrStatus = 0xc;
#if defined(XNU_TARGET_OS_OSX) && XNU_TARGET_OS_OSX
constexpr IOOptionBits kWakeEventSource = 0x1;   // kIOPMWakeEventSource (IOPMPrivate.h)
#endif

IOMemoryMap *gTlmmMap;
uintptr_t gTlmm;
uint32_t gSaved[SHENG_SLEEP_GPIO_COUNT], gAsleep[SHENG_SLEEP_GPIO_COUNT];
volatile int gLidClosedAtSleep;
volatile int gSource;   // sheng_wake_source of the last wake
const uint32_t kWakeSpis[] = {SHENG_SLEEP_LID_INTID, SHENG_SLEEP_POWER_INTID};

volatile uint32_t *reg(uint32_t gpio, uint32_t offset) {
    return reinterpret_cast<volatile uint32_t *>(gTlmm + static_cast<uintptr_t>(gpio) * TB_GPIO_STRIDE + offset);
}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
int lidClosed() {return !sheng_sleep_lid_wakes(*reg(SHENG_SLEEP_LID_GPIO, kIo));}

void prepare() {
    gSource = ShengWakeNone;
    for (uint32_t i = 0; i < SHENG_SLEEP_GPIO_COUNT; ++i) gSaved[i] = sheng_sleep_pin_usable(i) ? *reg(i, kIntrCfg) : 0;
    sheng_sleep_tlmm_keep(gSaved, gAsleep);
    // Closed as the sleep began, or now: a sleep again from a dark wake gets no will-sleep notification.
    gAsleep[SHENG_SLEEP_LID_GPIO] = sheng_sleep_lid_cfg(gSaved[SHENG_SLEEP_LID_GPIO], gLidClosedAtSleep || lidClosed());
    for (uint32_t i = 0; i < SHENG_SLEEP_GPIO_COUNT; ++i) {
        if (sheng_sleep_pin_usable(i) && gAsleep[i] != gSaved[i]) *reg(i, kIntrCfg) = gAsleep[i];
    }
    // An edge latched before the wait, open or closed (ShengSleep.h sheng_sleep_clear_lid_status): cleared, so that only the lid
    // moving while asleep raises the summary.
    if (sheng_sleep_clear_lid_status(lidClosed())) *reg(SHENG_SLEEP_LID_GPIO, kIntrStatus) = 0;
    barrier();
    sheng_sleep_stats[1] = sheng_sleep_stats[1] + 1;
}

int shouldWake(uint32_t intid) {
    if (intid == SHENG_SLEEP_LID_INTID) {
        if (!lidClosed()) {gSource = ShengWakeLid; return 1;}
        *reg(SHENG_SLEEP_LID_GPIO, kIntrStatus) = 0;
        barrier();
        sheng_sleep_stats[4] = sheng_sleep_stats[4] + 1;
        return 0;
    }
    if (intid == SHENG_SLEEP_POWER_INTID && sheng_buttons_sleep_check && sheng_buttons_sleep_check()) {gSource = ShengWakePower; return 1;}
    return 0;
}

void restore() {
    for (uint32_t i = 0; i < SHENG_SLEEP_GPIO_COUNT; ++i) {
        if (sheng_sleep_pin_usable(i) && gAsleep[i] != gSaved[i]) *reg(i, kIntrCfg) = gSaved[i];
    }
    barrier();
    if (gSource == ShengWakeLid) sheng_sleep_stats[2] = sheng_sleep_stats[2] + 1;
    if (gSource == ShengWakePower) sheng_sleep_stats[3] = sheng_sleep_stats[3] + 1;
}

// Every 10 s while asleep, as BootReset.c's watchdog thread does while awake: an armed hypervisor watchdog (30 s) would reset the
// sleeping system. Nothing when the kernel does not ping it.
void tick() {
    if (boot_reset_watchdog_ping) boot_reset_watchdog_ping();
}

const armcpu_sleep_board gBoard = {prepare, shouldWake, restore, kWakeSpis, sizeof kWakeSpis / sizeof kWakeSpis[0], tick, 10};

// The wake sources a sleep can count on: the keyboard probe past its interrupt step (stage 5 and on) holds the TLMM summary, the
// buttons armed (stage 3) hold the SPMI line.
#if defined(XNU_TARGET_OS_OSX) && XNU_TARGET_OS_OSX
bool lidArmed() {return &native_keyboard_stats && native_keyboard_stats[0] >= 5;}
bool powerArmed() {return &native_buttons_stats && native_buttons_stats[0] == 3 && sheng_buttons_sleep_check;}
uint64_t counterMs(uint64_t ticks) {
    uint64_t hz;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(hz));
    return hz ? ticks / (hz / 1000) : 0;
}
#endif

#if defined(XNU_TARGET_OS_OSX) && XNU_TARGET_OS_OSX
IOReturn sleepWake(void *, void *, natural_t message, IOService *, void *, vm_size_t) {
    IOPMrootDomain *root = IOService::getPMRootDomain();
    switch (message) {
    case kIOMessageSystemWillSleep:
        gLidClosedAtSleep = lidClosed();
        sheng_sleep_stats[6] = sheng_sleep_stats[6] + 1;
        IOLog("sheng sleep: sleeping, lid %s; wakes by lid %s, power key %s\n", gLidClosedAtSleep ? "closed" : "open",
              lidArmed() ? "yes" : "NO", powerArmed() ? "yes" : "NO");
        break;
    case kIOMessageSystemWillPowerOn:
        if (root && gSource != ShengWakeNone) root->claimSystemWakeEvent(root, kWakeEventSource, gSource == ShengWakeLid ? "sheng lid" : "sheng power");
        break;
    case kIOMessageSystemHasPoweredOn: {
        const volatile struct armcpu_sleep_stats &st = armcpu_sleep_stats;
        sheng_sleep_stats[7] = sheng_sleep_stats[7] + 1;
        // asleep_ticks are counter ticks (CNTFRQ), not the Mach timebase.
        IOLog("sheng sleep: woke by %s (intid %u) after %llu ms; sleeps %u, spurious %u, masked %u, ticks %u, unarmed %u, cpu_off failures "
              "%u, phase %u\n", sheng_sleep_woke_by(st.last_reason, gSource), st.last_intid, counterMs(st.asleep_ticks), st.entries,
              st.spurious, st.masked, st.ticks, st.unarmed, st.cpu_off_failures, st.phase);
        if (root && gSource != ShengWakeNone) root->requestUserActive(root, gSource == ShengWakeLid ? "sheng lid" : "sheng power");
        break;
    }
    default:
        break;
    }
    return kIOReturnSuccess;   // the notification's returnValue stays 0: acknowledged at once
}
#endif

// The probes run before ArmCpu has started (its boot core turns platform sleep on), so registration waits for it, up to a minute.
void sleepThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng sleep");
    int rc = -1;
    for (unsigned tries = 0; tries <= 240; ++tries) {
        rc = armcpu_sleep_register(&gBoard);
        if (rc == 0) break;
        IOSleep(250);
    }
    sheng_sleep_stats[5] = rc;
    if (rc) {sheng_sleep_stats[0] = -3; IOLog("sheng sleep: ArmCpu refused the wake sources (%d)\n", rc); return;}
#if defined(XNU_TARGET_OS_OSX) && XNU_TARGET_OS_OSX
    IONotifier *notifier = nullptr;
    for (unsigned tries = 0; tries <= 240 && !notifier; ++tries) {
        if (IOService::getPMRootDomain()) notifier = registerSleepWakeInterest(sleepWake, nullptr, nullptr);
        if (!notifier) IOSleep(250);
    }
    if (!notifier) {sheng_sleep_stats[0] = -4; IOLog("sheng sleep: no sleep/wake interest\n"); return;}
#endif
    sheng_sleep_stats[0] = 2;
    IOLog("sheng sleep: wake sources lid (intid %u) and power (intid %u)%s\n", SHENG_SLEEP_LID_INTID, SHENG_SLEEP_POWER_INTID,
          sheng_buttons_sleep_check ? "" : ", power key check missing");
}
}  // namespace

extern "C" void sheng_sleep_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("platform-sleep", &enabled, sizeof enabled) || enabled != 1 || sheng_sleep_stats[0]) return;
    sheng_sleep_stats[0] = 1;
    if (!armcpu_sleep_register) {sheng_sleep_stats[0] = -1; return;}
    IODeviceMemory *memory = IODeviceMemory::withRange(TB_TLMM_BASE, static_cast<IOByteCount>(SHENG_SLEEP_GPIO_COUNT) * TB_GPIO_STRIDE);
    gTlmmMap = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    gTlmm = gTlmmMap ? static_cast<uintptr_t>(gTlmmMap->getVirtualAddress()) : 0;
    if (!gTlmm) {sheng_sleep_stats[0] = -2; return;}
    thread_t thread = nullptr;
    if (kernel_thread_start(sleepThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else sheng_sleep_stats[0] = -5;
}
