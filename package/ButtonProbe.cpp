// SPDX-License-Identifier: BSD-2-Clause-Patent
// Power and volume buttons of the sheng board (boot argument sheng-buttons=1), sent as the Consumer Power, Volume Increment and Volume
// Decrement bits of the HID input device's report 2 (drivers/NativeHidInput).
// The devicetree facts: power is the PMK8550 PON (SPMI SID 0, peripheral 0x13, interrupt 7 "pwrkey"), volume down its RESIN (interrupt 6),
// volume up the PM8550 GPIO 6 (SID 1, peripheral 0x8d, active low with a pull-up, gpio-keys). All three are PMIC peripheral interrupts the
// SPMI arbiter (spmi@c400000, EE 0) gathers into its "periph_irq", PDC pin 1, which the PDC forwards as SPI 481 (qcom,pdc-ranges 0 -> 480).
// Register layout as the mainline spmi-pmic-arb (version 7) and qcom-pdc (version 3.2: the enable bit sits in IRQ_i_CFG) drivers use it:
// per APID in the "chnls" window +0x100 ACC_ENABLE, +0x104 IRQ_STATUS, +0x108 IRQ_CLEAR; the owner's accumulated status words at "intr" +
// 0x1000 * EE + 4 * word; in the peripheral 0x10 RT_STS, 0x11 SET_TYPE, 0x12 POLARITY_HIGH, 0x13 POLARITY_LOW, 0x14 LATCHED_CLR, 0x15 EN_SET.
// The interrupt handler acknowledges and wakes one thread; the thread reads the three levels and reports a change. Nothing polls.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "BootResetCore.h"
}

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
// [0] stage (1 mapped, 2 configured, 3 armed, -9 masked after an interrupt storm, other negatives: the step that failed), [1] interrupts, [2] reports, [3] pressed mask,
// [4] power APID, [5] GPIO APID, [6] interrupts of other APIDs cleared, [7] PDC version, [8] last error, [9] spurious interrupts
extern "C" {volatile int32_t native_buttons_stats[10];}

namespace {
constexpr uint64_t kCore = 0xc400000, kObsrvr = 0xc440000, kChnls = 0xc500000, kIntr = 0xc4c0000, kCnfg = 0xc42d000, kPdc = 0xb220000;
constexpr uint32_t kPonPpid = 0x013, kGpioPpid = 0x18d;
constexpr uint8_t kPowerBit = 1u << 7, kResinBit = 1u << 6, kGpioBit = 1u << 0;
constexpr uint32_t kAccEnable = 0x100, kIrqStatus = 0x104, kIrqClear = 0x108;
constexpr uint8_t kRtSts = 0x10, kSetType = 0x11, kPolHigh = 0x12, kPolLow = 0x13, kLatchedClr = 0x14, kEnSet = 0x15;
constexpr uint32_t kPdcPin = 1, kSpi = 481, kPdcVersion = 0x1000, kPdcCfg = 0x110, kPdcEnableBank = 0x10;
// Report 2 bits (NativeHidInput): 1 Power, 2 Volume Increment, 3 Volume Decrement.
constexpr uint32_t kReportPower = 1u << 1, kReportVolumeUp = 1u << 2, kReportVolumeDown = 1u << 3;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
Window gCore, gCnfg, gObs, gChn, gIntr, gPdc;
br_arbiter gArb;
int gPon = -1, gGpio = -1;
uint32_t gApids;
volatile uint32_t gEvents;
void (*volatile gSink)(void *, uint32_t);
void *volatile gSinkContext;

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
volatile uint32_t *apidReg(int apid, uint32_t reg) {return gChn.va + (BR_CHNL_APID_STRIDE * (uint32_t)apid + reg) / 4;}
void fail(int step, int error) {native_buttons_stats[8] = error; native_buttons_stats[0] = -step;}

// The APID that serves ppid with its interrupts owned by this EE, as the mainline driver chooses it; -1 when there is none.
int findApid(uint32_t ppid) {
    int found = -1;
    for (uint32_t i = 0; i < gApids; ++i) {
        uint32_t map = gCore.va[(BR_APID_MAP + 4 * i) / 4];
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        const bool irqOwner = (map >> 24) & 1;
        if (irqOwner && (gCnfg.va[(BR_CNFG_OWNER + 4 * i) / 4] & 7u) == gArb.ee) found = (int)i;
    }
    return found;
}
// Both edges of the given interrupt bits, latched state cleared, then enabled in the peripheral.
int armPeripheral(int apid, uint8_t bits) {
    uint8_t type = 0, high = 0, low = 0;
    int rc = br_read8(gObs.va, &gArb, apid, kSetType, &type);
    if (!rc) rc = br_read8(gObs.va, &gArb, apid, kPolHigh, &high);
    if (!rc) rc = br_read8(gObs.va, &gArb, apid, kPolLow, &low);
    if (!rc) rc = br_write8(gChn.va, apid, kSetType, type | bits);
    if (!rc) rc = br_write8(gChn.va, apid, kPolHigh, high | bits);
    if (!rc) rc = br_write8(gChn.va, apid, kPolLow, low | bits);
    if (!rc) rc = br_write8(gChn.va, apid, kLatchedClr, bits);
    if (!rc) rc = br_write8(gChn.va, apid, kEnSet, bits);
    if (!rc) *apidReg(apid, kAccEnable) = 1;
    return rc;
}

// Primary interrupt context: acknowledge in the arbiter and the peripheral, then wake the thread. An APID that is not ours but raised
// the line only gets its arbiter status cleared (mainline cleanup_irq): reading another APID's registers can end in a synchronous
// external abort (int-227 panicked reading ACC_ENABLE of APIDs it did not own).
void buttonsIrq(void *) {
    bool ours = false;
    for (uint32_t word = 0; word * 32 < gApids; ++word) {
        uint32_t pending = gIntr.va[word];
        while (pending) {
            const int apid = (int)(word * 32 + __builtin_ctz(pending));
            pending &= pending - 1;
            if (apid != gPon && apid != gGpio) {
                *apidReg(apid, kIrqClear) = 0xff;
                native_buttons_stats[6] = native_buttons_stats[6] + 1;
                continue;
            }
            const uint32_t status = *apidReg(apid, kIrqStatus);
            *apidReg(apid, kIrqClear) = status;
            if (status) br_write8(gChn.va, apid, kLatchedClr, (uint8_t)status);
            ours = true;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    // A line that keeps firing without one of our peripherals behind it would hold this core in the handler: after 1000 such interrupts
    // in a row the PDC pin is masked, which drops the line, and stage -9 records it.
    static uint32_t idle;
    if (!ours) {
        native_buttons_stats[9] = native_buttons_stats[9] + 1;
        if (++idle >= 1000) {
            gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = 4u;
            __asm__ volatile("dsb sy" ::: "memory");
            native_buttons_stats[0] = -9;
        }
        return;
    }
    idle = 0;
    native_buttons_stats[1] = native_buttons_stats[1] + 1;
    __atomic_fetch_add(&gEvents, 1, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gEvents);
}

uint32_t pressed() {
    uint8_t pon = 0, gpio = 0xff;
    uint32_t mask = 0;
    if (!br_read8(gObs.va, &gArb, gPon, kRtSts, &pon)) {
        if (pon & kPowerBit) mask |= kReportPower;
        if (pon & kResinBit) mask |= kReportVolumeDown;
    }
    if (gGpio >= 0 && !br_read8(gObs.va, &gArb, gGpio, kRtSts, &gpio) && !(gpio & kGpioBit)) mask |= kReportVolumeUp;
    return mask;
}

bool setup() {
    gArb.ee = 0;
    if (!mapWindow(gCore, kCore, 0x3000) || !mapWindow(gCnfg, kCnfg, 0x4000) || !mapWindow(gIntr, kIntr, 0x1000) || !mapWindow(gPdc, kPdc, 0x2000)) {
        fail(1, -12); return false;
    }
    gArb.core = gCore.va; gArb.cnfg = gCnfg.va;
    const uint32_t version = gCore.va[0];
    gApids = gCore.va[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
    if (version < BR_VERSION_V7_MIN || version >= BR_VERSION_V8_MIN || !gApids || gApids > 1024) {fail(2, (int)version); return false;}
    if (!mapWindow(gObs, kObsrvr, ((uint64_t)BR_OBS_APID_STRIDE * gApids + 0xfff) & ~0xfffull) ||
        !mapWindow(gChn, kChnls, (uint64_t)BR_CHNL_APID_STRIDE * gApids)) {fail(3, -12); return false;}
    native_buttons_stats[0] = 1;
    gPon = findApid(kPonPpid); gGpio = findApid(kGpioPpid);
    native_buttons_stats[4] = gPon; native_buttons_stats[5] = gGpio;
    if (gPon < 0) {fail(4, -2); return false;}
    int rc = armPeripheral(gPon, kPowerBit | kResinBit);
    if (!rc && gGpio >= 0) rc = armPeripheral(gGpio, kGpioBit);
    if (rc) {fail(5, rc); return false;}
    // PDC pin 1: level high, enabled.
    const uint32_t pdcVersion = gPdc.va[kPdcVersion / 4];
    native_buttons_stats[7] = (int32_t)pdcVersion;
    if (pdcVersion >= 0x30200) {
        gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = (1u << 3) | 4u;
    } else {
        gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = 4u;
        gPdc.va[(kPdcEnableBank + 4 * (kPdcPin / 32)) / 4] |= 1u << (kPdcPin % 32);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    native_buttons_stats[0] = 2;
    if (!armcpu_spi_attach) {fail(6, -78); return false;}
    // The probe runs before the interrupt controller driver has started (int-228: kIOReturnNotReady at 4 s), so wait for it, up to a minute.
    for (unsigned tries = 0;; ++tries) {
        rc = armcpu_spi_attach(32 + kSpi, false, buttonsIrq, nullptr, nullptr, nullptr);
        if (rc != kIOReturnNotReady || tries == 240) break;
        IOSleep(250);
    }
    if (rc) {fail(6, rc); return false;}
    native_buttons_stats[0] = 3;
    return true;
}

void buttonsThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng-buttons");
    if (!setup()) return;
    uint32_t seen = gEvents, last = 0;
    for (;;) {
        const uint32_t mask = pressed();
        if (mask != last) {
            last = mask;
            native_buttons_stats[3] = (int32_t)mask;
            void (*sink)(void *, uint32_t) = __atomic_load_n(&gSink, __ATOMIC_ACQUIRE);
            if (sink) {sink(gSinkContext, mask); native_buttons_stats[2] = native_buttons_stats[2] + 1;}
        }
        assert_wait((event_t)&gEvents, THREAD_UNINT);
        if (gEvents != seen) clear_wait(current_thread(), THREAD_AWAKENED);
        else thread_block(THREAD_CONTINUE_NULL);
        seen = gEvents;
    }
}
}  // namespace

extern "C" void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {
    gSinkContext = context;
    __atomic_store_n(&gSink, sink, __ATOMIC_RELEASE);
}

extern "C" void sheng_buttons_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("sheng-buttons", &enabled, sizeof enabled) || enabled != 1 || native_buttons_stats[0]) return;
    native_buttons_stats[0] = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(buttonsThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(7, -12);
}
