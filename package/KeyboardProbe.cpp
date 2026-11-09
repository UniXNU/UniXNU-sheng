// SPDX-License-Identifier: BSD-2-Clause-Patent
// The sheng keyboard cover (boot argument sheng-keyboard=1): keyboard and touchpad behind the Nanosic WN8030 controller (Wn8030.h) on
// I2C9 (QUP2 SE1, 0x884000, client 0x4c). The devicetree facts: interrupt GPIO 62 (rising edge), reset GPIO 58 and sleep GPIO 57 (both
// active low), status GPIO 56, I2C on GPIO 60/61 (qup2_se1, no pull), supplies vreg_l12b_1p8 (vddio) and vreg_l13b_3p3 (dvdd), the lid
// and tablet-mode hall sensors on GPIO 171 and 172 (active low, pull-up). One kernel thread brings it up in order:
//   1  pins: reset and sleep held low, I2C function, inputs
//   2  supplies through active TCS 2 of the apps RSC (shared with the microphone and USB rails, under AppsSmmuLock)
//   3  QUP2 clocks (core, AHB, SE1) and the GENI I2C engine from the 19.2 MHz DFS level (sheng-keyboard-khz, default 1000: a frame read
//      must not hold the bus long, or the controller merges touch frames)
//   4  the engine interrupt (SPI 583) and the TLMM summary interrupt (SPI 208, shared) for GPIO 62, 171 and 172
//   5  controller power-on: reset released, 20 ms, sleep released; boot ROM id 0xc8 in state 0
//   6  the RAM image (the pinned MCU_Upgrade.bin): header, 99 data blocks, verify, boot
//   7  serving: one 68-byte frame read per controller interrupt
// While serving, an attach message creates the HID device (KeyboardCover.cpp) and a detach message terminates it. Input passes while the
// cover is neither closed over the screen (lid) nor folded behind the tablet (tablet mode); otherwise keys and fingers are released, the
// touchpad and LEDs are turned off and the device reports Keyboard Power 0, so the system shows the on-screen keyboard. The cover's
// controller is told to stay awake every 12 s while attached. Every transfer is driven by the engine interrupt; the thread sleeps
// between events. Authentication requests of the cover are counted and left unanswered.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/c++/OSMetaClass.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <string.h>
#include "AppsSmmuLock.h"
extern "C" {
#include "GeniI2c.h"
#include "Wn8030.h"
#include "QcomRpmh.h"
#include "TouchBoard.h"
}

extern kmod_info_t g_kernel_kmod_info;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" void sheng_keyboard_cover_static_init(void) __asm__("__GLOBAL__sub_I_KeyboardCover.cpp");
extern "C" IOService *sheng_keyboard_cover_allocate(void);
extern "C" void sheng_keyboard_cover_input(IOService *, const uint8_t *, uint32_t);
extern "C" void sheng_keyboard_cover_enable(IOService *, bool);
extern "C" void sheng_keyboard_cover_output_sink(void (*)(void *, uint8_t), void *);
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" int armcpu_spi_share(uint32_t, void (*)(void *), void *) __attribute__((weak_import));
extern "C" bool sm8550_gcc_update32(volatile uint32_t *, uint32_t, uint32_t) __attribute__((weak_import));
// The controller's RAM image, generated at build time from .cache/firmware/nanosic/MCU_Upgrade.bin.
extern "C" const uint8_t native_keyboard_firmware[];
extern "C" const uint32_t native_keyboard_firmware_size;

// [0] stage (see the list above; negative: the step that failed), [1] the failure's detail, [2] engine: protocol << 24 | clock config,
// [3] boot ROM id | state << 8 | state after the header << 16 | state after verify << 24, [4] ms to load the image, [5] controller
// interrupts, [6] frames read, [7] keyboard | consumer << 8 | touchpad << 16 reports passed (low 8 bits each), [8] attached (1/0) |
// attaches << 8 | HID devices that failed to start << 24, [9] lid closed | tablet mode << 1 | hall interrupts << 8, [10] input enabled,
// [11] I2C failures, [12] engine interrupts, [13] last I2C error | its M_IRQ status << 8, [14] commands sent, [15] authentication
// requests | other frames << 16, [16] touching palms sent as lifted
extern "C" {volatile int32_t native_keyboard_stats[17];}
// The cover's last accelerometer sample (x, y, z in 1/256 g, tablet axes) and the number of samples.
extern "C" {volatile int32_t native_keyboard_accel[4];}
// The range of the controller's own touchpad coordinates over every touching contact: X min, X max, Y min, Y max (-1 before the first).
extern "C" {volatile int32_t native_keyboard_touch_range[4] = {-1, -1, -1, -1};}

namespace {
constexpr uint64_t kGcc = 0x00100000ULL, kTlmm = 0x0f100000ULL, kSe = 0x00884000ULL, kRsc = 0x17a20000ULL, kCmdDb = 0x81c60000ULL;
constexpr uint32_t kCmdDbSize = 0x20000, kRscTcs = 0xd00, kTcs = 2;
// gcc-sm8550.c: QUP2 branch votes in 0x52010 (bit 0 wrap2 core, 1 S AHB, 2 M AHB, 3 core 2x, 5 SE1), their halt registers, the SE1 RCG.
constexpr uint32_t kVote = 0x52010, kVoteBits = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5);
constexpr uint32_t kHalts[] = {0x233d4, 0x233d0, 0x233cc, 0x23004, 0x1e13c};
constexpr uint32_t kRcg = 0x1e148;
constexpr uint32_t kSeIntid = 32 + 583, kTlmmIntid = 32 + 208;
constexpr unsigned kStatusPin = 56, kSleepPin = 57, kResetPin = 58, kSdaPin = 60, kSclPin = 61, kIrqPin = 62, kLidPin = 171, kTabletPin = 172;
constexpr uint32_t kKeepaliveMs = 12000, kDebounceMs = 15;
// TLMM per pin (pinctrl-sm8550.c): CTL bits 1:0 pull (1 down, 3 up), 5:2 function, 8:6 drive, 9 output enable; IO bit 0 in, bit 1 out;
// INTR_CFG bit 0 enable, 1 polarity, 3:2 detection (1 rising, 3 both), 4 raw status, 7:5 target (3 the application processor);
// INTR_STATUS bit 0, acknowledged with 0.
constexpr uint32_t kCtl = 0x0, kIo = 0x4, kIntrCfg = 0x8, kIntrStatus = 0xc;

struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};
bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
uint64_t nowMs() {uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
uint64_t deadlineMs(uint32_t ms) {uint64_t d; clock_interval_to_deadline(ms, kMillisecondScale, &d); return d;}
void stage(int step) {native_keyboard_stats[0] = step;}
bool fail(int step, int32_t detail) {native_keyboard_stats[1] = detail; native_keyboard_stats[0] = -step; return false;}
void bump(unsigned index, int32_t by = 1) {native_keyboard_stats[index] = native_keyboard_stats[index] + by;}

struct Keyboard {
    Region gccLow, gcc1e, gcc23, gccVote, se;
    Region pin[8];
    GeniI2c bus;
    IOSimpleLock *busLock;
    uint32_t khz;
    uint8_t backlight;
    // Interrupt state: written by the handlers, read by the thread.
    volatile int busResult;
    volatile uint32_t mcuIrqs, hallIrqs;
    volatile uint32_t capsWanted, capsRequests;
    // Thread state.
    uint32_t mcuSeen, hallSeen, capsSeen;
    bool attached, inputEnabled, lid, tablet, caps, hallPending;
    uint64_t keepalive, hallDue;   // absolute deadlines
    IOService *hid;
    bool classRegistered;
    uint8_t frame[WN8030_FRAME_BYTES];
    uint8_t command[1 + WN8030_DATA_COMMAND_BYTES];
};
Keyboard *gKb;
volatile uint32_t gWake;      // the thread's event
volatile uint32_t gBusWake;   // a transfer ended

enum PinIndex {PinStatus, PinSleep, PinReset, PinSda, PinScl, PinIrq, PinLid, PinTablet};
constexpr unsigned kPins[] = {kStatusPin, kSleepPin, kResetPin, kSdaPin, kSclPin, kIrqPin, kLidPin, kTabletPin};
constexpr PinIndex kInterruptPins[] = {PinIrq, PinLid, PinTablet}, kHallPins[] = {PinLid, PinTablet}, kBusPins[] = {PinSda, PinScl};
uintptr_t pin(PinIndex i) {return gKb->pin[i].va;}
void setOutput(PinIndex i, bool high) {wr32(pin(i), kIo, high ? 2u : 0u);}
bool level(PinIndex i) {return rd32(pin(i), kIo) & 1u;}

// The engine interrupt: the transfer core moves FIFO words and reports the end; the thread is woken only then.
void busIrq(void *) {
    Keyboard *kb = gKb;
    IOSimpleLockLock(kb->busLock);
    const int result = geni_i2c_poll(&kb->bus);
    if (result != GeniPending) __atomic_store_n(&kb->busResult, result, __ATOMIC_RELEASE);
    IOSimpleLockUnlock(kb->busLock);
    bump(12);
    if (result != GeniPending) thread_wakeup((event_t)&gBusWake);
}

// The shared TLMM summary interrupt: this driver's pins only.
void tlmmIrq(void *) {
    Keyboard *kb = gKb;
    bool wake = false;
    if (rd32(pin(PinIrq), kIntrStatus) & 1u) {
        wr32(pin(PinIrq), kIntrStatus, 0);
        __atomic_fetch_add(&kb->mcuIrqs, 1, __ATOMIC_RELEASE);
        wake = true;
    }
    for (PinIndex i : kHallPins) {
        if (!(rd32(pin(i), kIntrStatus) & 1u)) continue;
        wr32(pin(i), kIntrStatus, 0);
        __atomic_fetch_add(&kb->hallIrqs, 1, __ATOMIC_RELEASE);
        wake = true;
    }
    if (wake) thread_wakeup((event_t)&gWake);
}

// One transfer: started under the lock the interrupt handler takes, then the thread sleeps until the handler reports its end or the
// deadline of the current step passes (then the core cancels, then aborts).
template <typename Start> int transfer(Start start, uint32_t bytes) {
    Keyboard *kb = gKb;
    const uint32_t ms = 10 + bytes * 9 * 2 / kb->khz;
    IOInterruptState state = IOSimpleLockLockDisableInterrupt(kb->busLock);
    kb->busResult = GeniPending;
    const int started = start(&kb->bus);
    IOSimpleLockUnlockEnableInterrupt(kb->busLock, state);
    if (started) {bump(11); return GeniFailed;}
    int result = GeniPending;
    for (unsigned step = 0; step < 4 && result == GeniPending; ++step) {
        const uint64_t deadline = deadlineMs(ms);
        wait_result_t waited = THREAD_AWAKENED;
        while ((result = __atomic_load_n(&kb->busResult, __ATOMIC_ACQUIRE)) == GeniPending && waited != THREAD_TIMED_OUT) {
            assert_wait_deadline((event_t)&gBusWake, THREAD_UNINT, deadline);
            if ((result = __atomic_load_n(&kb->busResult, __ATOMIC_ACQUIRE)) != GeniPending) {clear_wait(current_thread(), THREAD_AWAKENED); break;}
            waited = thread_block(THREAD_CONTINUE_NULL);
        }
        if (result != GeniPending) break;
        // This step's deadline passed: cancel, then abort, then give up (geni_i2c_timeout).
        state = IOSimpleLockLockDisableInterrupt(kb->busLock);
        result = __atomic_load_n(&kb->busResult, __ATOMIC_ACQUIRE);
        if (result == GeniPending) result = geni_i2c_timeout(&kb->bus);
        IOSimpleLockUnlockEnableInterrupt(kb->busLock, state);
    }
    if (result != GeniDone) {
        bump(11);
        native_keyboard_stats[13] = (int32_t)(kb->bus.error | (kb->bus.error_status << 8));
    }
    return result == GeniPending ? GeniFailed : result;
}

bool romWrite(uint8_t value) {
    return transfer([value](GeniI2c *g) {return geni_i2c_start_write(g, WN8030_REG_WRITE, value);}, 2) == GeniDone;
}
int romRead() {
    if (transfer([](GeniI2c *g) {return geni_i2c_start_read(g, WN8030_REG_WRITE);}, 2) != GeniDone) return -1;
    return gKb->bus.data;
}
int romState() {return romWrite(WN8030_ROM_STATE) ? romRead() : -1;}
bool writeBuffer(const uint8_t *bytes, uint32_t length) {
    return transfer([bytes, length](GeniI2c *g) {return geni_i2c_start_write_buffer(g, bytes, length);}, length) == GeniDone;
}
bool readFrame() {
    static const uint8_t reg = WN8030_REG_READ;
    uint8_t *frame = gKb->frame;
    return transfer([frame](GeniI2c *g) {return geni_i2c_start_read_buffer(g, &reg, 1, frame, WN8030_FRAME_BYTES);},
                    1 + WN8030_FRAME_BYTES) == GeniDone;
}
bool control(uint8_t command, uint8_t value) {
    uint8_t out[1 + WN8030_COMMAND_BYTES];
    const uint32_t n = wn8030_control(out, command, &value, 1);
    const bool ok = n && writeBuffer(out, n);
    if (ok) bump(14);
    return ok;
}

bool setupPins() {
    for (unsigned i = 0; i < sizeof kPins / sizeof *kPins; ++i)
        if (!mapRegion(kTlmm + tb_tlmm_ctl(kPins[i]), 0x1000, gKb->pin[i])) return fail(1, (int32_t)kPins[i]);
    // Every pin interrupt off until it is armed.
    for (PinIndex i : kInterruptPins) wr32(pin(i), kIntrCfg, rd32(pin(i), kIntrCfg) & ~1u);
    setOutput(PinReset, false); wr32(pin(PinReset), kCtl, 1u << 9);           // output low, no pull, 2 mA
    setOutput(PinSleep, false); wr32(pin(PinSleep), kCtl, 1u << 9 | 1u);      // output low, pull-down
    wr32(pin(PinStatus), kCtl, 0);                                            // input, no pull
    wr32(pin(PinIrq), kCtl, rd32(pin(PinIrq), kCtl) & 3u);                    // input, pull as found
    for (PinIndex i : kBusPins) wr32(pin(i), kCtl, 1u << 2);          // qup2_se1, no pull, 2 mA
    for (PinIndex i : kHallPins) wr32(pin(i), kCtl, 3u);            // input, pull-up
    return true;
}

// The two supplies: one request per register of a rail (mode 7 HPM, voltage in mV, enable), as the RPMh regulator driver sends them.
bool setupRails() {
    struct CmdDbCopy {uint8_t *bytes = static_cast<uint8_t *>(IOMallocData(kCmdDbSize)); ~CmdDbCopy() {if (bytes) IOFreeData(bytes, kCmdDbSize);}} copy;
    if (!copy.bytes) return fail(2, -12);
    Region db, rsc;
    if (!mapRegion(kCmdDb, kCmdDbSize, db) || !mapRegion(kRsc, 0x10000, rsc)) return fail(2, -12);
    for (uint32_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = rd32(db.va, i); memcpy(copy.bytes + i, &w, 4);}
    const struct {const char *name; uint32_t millivolts;} rails[] = {{"ldob12", 1800}, {"ldob13", 3296}};
    QcomRpmhResource res[2];
    for (unsigned i = 0; i < 2; ++i) if (qcom_cmd_db_find(copy.bytes, kCmdDbSize, rails[i].name, &res[i])) return fail(2, 0x100 + (int32_t)i);
    const QcomRscLayout layout = qcom_rsc_layout(rd32(rsc.va, 0));
    AppsSmmuLock tcsOwner;
    if (!tcsOwner.ready()) return fail(2, -16);
    auto send = [&](uint32_t address, uint32_t value) {
        QcomRpmhWrite writes[16];
        const unsigned n = qcom_rpmh_active_request(&layout, kTcs, &address, &value, 1, writes, 16);
        for (unsigned i = 0; i < n; ++i) wr32(rsc.va, kRscTcs + writes[i].offset, writes[i].value);
        IOSleep(2);
        const unsigned m = qcom_rpmh_finish(&layout, kTcs, writes, 16);
        for (unsigned i = 0; i < m; ++i) wr32(rsc.va, kRscTcs + writes[i].offset, writes[i].value);
    };
    for (unsigned i = 0; i < 2; ++i) {
        send(res[i].address + 8, 7);
        send(res[i].address, rails[i].millivolts);
        send(res[i].address + 4, 1);
    }
    return true;
}

bool setupClocks() {
    Keyboard *kb = gKb;
    if (!mapRegion(kGcc, 0x1000, kb->gccLow) || !mapRegion(kGcc + 0x1e000, 0x1000, kb->gcc1e) ||
        !mapRegion(kGcc + 0x23000, 0x1000, kb->gcc23) || !mapRegion(kGcc + 0x52000, 0x1000, kb->gccVote)) return fail(3, -12);
    wr32(kb->gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff, rd32(kb->gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff) | 1u);
    for (unsigned i = 0; i < 10000 && !(rd32(kb->gccLow.va, TB_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);
    volatile uint32_t *vote = reinterpret_cast<volatile uint32_t *>(kb->gccVote.va + (kVote & 0xfff));
    if (sm8550_gcc_update32) {
        if (!sm8550_gcc_update32(vote, 0, kVoteBits)) return fail(3, -5);
    } else wr32(kb->gccVote.va, kVote & 0xfff, rd32(kb->gccVote.va, kVote & 0xfff) | kVoteBits);
    for (unsigned i = 0; i < 2000; ++i) {
        bool running = true;
        for (uint32_t reg : kHalts) {
            const uint32_t v = rd32((reg >> 12) == 0x1e ? kb->gcc1e.va : kb->gcc23.va, reg & 0xfff);
            if ((v & TB_CBCR_CLK_OFF) && !(v & 2u)) running = false;
        }
        if (running) return true;
        IODelay(10);
    }
    return fail(3, -60);   // never touch an engine without running clocks
}

// The engine at the chosen speed from 19.2 MHz (i2c-qcom-geni.c: 100 kHz divider 7, high 10, low 12, cycle 26; 400 kHz 2, 5, 11, 22;
// 1 MHz 1, 2, 8, 18). Without a 19.2 MHz DFS level the boot loader's configuration is kept when it has one.
bool setupBus() {
    Keyboard *kb = gKb;
    if (!mapRegion(kSe, TB_SE_SIZE, kb->se)) return fail(3, -12);
    const uintptr_t se = kb->se.va, rcg = kb->gcc1e.va + (kRcg & 0xfff);
    unsigned level = 0xff;
    for (unsigned l = 0; l < TB_DFS_LEVELS && level == 0xff; ++l)
        if (tb_dfs_level_hz(rd32(rcg, TB_RCG_PERF_CFG(l)), rd32(rcg, TB_RCG_PERF_M(l)), rd32(rcg, TB_RCG_PERF_N(l))) == 19200000ULL) level = l;
    const uint32_t proto = (rd32(se, GENI_FW_REVISION) >> 8) & 0xff;
    native_keyboard_stats[2] = (int32_t)(proto << 24 | (rd32(se, GENI_CLK_CFG) & 0xffffff));
    if (proto != GENI_PROTO_I2C) return fail(3, (int32_t)(0x200 | proto));
    if (level != 0xff) {
        const uint32_t divider = kb->khz == 1000 ? 1 : kb->khz == 400 ? 2 : 7;
        const uint32_t counters = kb->khz == 1000 ? (2u << 20 | 8u << 10 | 18u) : kb->khz == 400 ? (5u << 20 | 11u << 10 | 22u)
                                                                                                 : (10u << 20 | 12u << 10 | 26u);
        wr32(se, 0x28, rd32(se, 0x28) | 0x7fu);             // clock gating controls on
        wr32(se, 0xe30, rd32(se, 0xe30) | 0xfu);            // DMA clock gating controls on
        wr32(se, 0x24, 0x7fu);                              // default output control
        wr32(se, 0x20, 1u);                                 // force default
        wr32(se, 0xe18, 0);                                 // no GSI events
        wr32(se, 0x7c, level);
        wr32(se, GENI_CLK_CFG, divider << 4 | 1u);
        wr32(se, GENI_SCL_COUNTERS, counters);
    } else if (!(rd32(se, GENI_CLK_CFG) & 1u) || !rd32(se, GENI_SCL_COUNTERS)) return fail(3, 0x300);
    const GeniIo io = {ioRd, ioWr, reinterpret_cast<void *>(se)};
    uint32_t config = 0;
    if (geni_i2c_prepare(&kb->bus, &io, WN8030_ADDRESS, &config)) return fail(3, (int32_t)config);
    native_keyboard_stats[2] = (int32_t)config;
    return true;
}

// The engine interrupt is exclusive; the pin interrupts share the TLMM summary line with touch and Bluetooth. The probe can run
// before the interrupt controller driver started, so its readiness is waited for (as the buttons driver does), up to a minute.
bool setupInterrupts() {
    if (!armcpu_spi_attach || !armcpu_spi_share) return fail(4, -78);
    int rc = 0;
    for (unsigned tries = 0;; ++tries) {
        rc = armcpu_spi_attach(kSeIntid, false, busIrq, nullptr, nullptr, nullptr);
        if (rc != kIOReturnNotReady || tries == 240) break;
        IOSleep(250);
    }
    if (rc) return fail(4, rc);
    for (PinIndex i : kInterruptPins) {
        uint32_t cfg = rd32(pin(i), kIntrCfg) & ~((7u << 5) | (3u << 2) | (1u << 1) | 1u);
        cfg |= (3u << 5) | ((i == PinIrq ? 1u : 3u) << 2) | (1u << 1) | (1u << 4);   // AP, rising (controller) or both edges (hall)
        wr32(pin(i), kIntrCfg, cfg);
        wr32(pin(i), kIntrStatus, 0);
    }
    rc = armcpu_spi_share(kTlmmIntid, tlmmIrq, nullptr);
    if (rc) return fail(4, 0x1000 | rc);
    for (PinIndex i : kInterruptPins) wr32(pin(i), kIntrCfg, rd32(pin(i), kIntrCfg) | 1u);
    return true;
}

bool powerOn() {
    IOSleep(10);                       // supplies settle with the controller in reset
    setOutput(PinReset, true);
    IOSleep(20);                       // the boot ROM starts
    setOutput(PinSleep, true);
    IOSleep(5);
    if (!romWrite(WN8030_ROM_ID)) return fail(5, 1);
    const int id = romRead();
    const int state = romState();
    native_keyboard_stats[3] = (id & 0xff) | (state & 0xff) << 8;
    if (id != (int)WN8030_BOOT_ID) return fail(5, id);
    if (state != Wn8030RomIdle) return fail(5, 0x100 | state);
    return true;
}

bool loadImage() {
    Keyboard *kb = gKb;
    const uint64_t start = nowMs();
    uint32_t code = 0;
    const int checked = wn8030_image_check(native_keyboard_firmware, native_keyboard_firmware_size, &code);
    if (checked) return fail(6, checked);
    uint32_t n = wn8030_header_command(kb->command, native_keyboard_firmware, code);
    if (!writeBuffer(kb->command, n)) return fail(6, -10);
    const int header = romState();
    native_keyboard_stats[3] |= (header & 0xff) << 16;
    if (header != Wn8030RomHeader) return fail(6, 0x100 | (header & 0xff));
    for (uint32_t offset = 0, next = 0; offset < code; offset = next) {
        n = wn8030_data_command(kb->command, native_keyboard_firmware, code, offset, &next);
        if (!n || !writeBuffer(kb->command, n)) return fail(6, (int32_t)(0x10000 | offset));
    }
    if (!romWrite(WN8030_ROM_VERIFY)) return fail(6, -11);
    IOSleep(10);
    const int verified = romState();
    native_keyboard_stats[3] |= (verified & 0xff) << 24;
    if (verified != Wn8030RomVerified) return fail(6, 0x200 | (verified & 0xff));
    // Edges of the interrupt line before the firmware runs are not frames.
    kb->mcuSeen = __atomic_load_n(&kb->mcuIrqs, __ATOMIC_ACQUIRE);
    if (!romWrite(WN8030_ROM_BOOT)) return fail(6, -12);
    IOSleep(50);
    native_keyboard_stats[4] = (int32_t)(nowMs() - start);
    return true;
}

// The HID class registers once, after its family: its superclass is IOHIDFamily's IOHIDDevice.
bool registerClass() {
    Keyboard *kb = gKb;
    if (kb->classRegistered) return true;
    if (OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily")) return false;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    sheng_keyboard_cover_static_init();
    if (OSMetaClass::postModLoad(handle)) return false;
    kb->classRegistered = true;
    return true;
}

void ledRequest(void *, uint8_t caps) {
    Keyboard *kb = gKb;
    __atomic_store_n(&kb->capsWanted, caps ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&kb->capsRequests, 1, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gWake);
}

IOService *createHid() {
    if (!registerClass()) return nullptr;
    OSDictionary *match = IOService::serviceMatching("IOHIDResource");
    IOService *resource = match ? IOService::waitForMatchingService(match, 5000000000ULL) : nullptr;
    IOService *device = resource ? sheng_keyboard_cover_allocate() : nullptr;
    bool started = false, attached = false;
    if (device && device->init()) {
        attached = device->attach(resource);
        started = attached && device->start(resource);
    }
    if (!started && device) {
        if (attached) device->detach(resource);
        OSSafeReleaseNULL(device);
    }
    OSSafeReleaseNULL(resource);
    OSSafeReleaseNULL(match);
    return device;
}

void releaseInputs() {
    uint8_t keyboard[WN8030_KEYBOARD_REPORT], consumer[WN8030_CONSUMER_REPORT], touchpad[WN8030_TOUCHPAD_REPORT];
    wn8030_release_reports(keyboard, consumer, touchpad);
    IOService *hid = gKb->hid;
    if (!hid) return;
    sheng_keyboard_cover_input(hid, keyboard, sizeof keyboard);
    sheng_keyboard_cover_input(hid, consumer, sizeof consumer);
    sheng_keyboard_cover_input(hid, touchpad, sizeof touchpad);
}

// The controller's side of the input state: touchpad reports, indicator LEDs and the backlight.
void applyInputState() {
    Keyboard *kb = gKb;
    if (!kb->attached) return;
    const bool on = kb->inputEnabled;
    control(WN8030_CMD_TOUCHPAD, on ? 1 : 0);
    control(WN8030_CMD_INDICATOR, on && kb->caps ? WN8030_LED_CAPS_ON : WN8030_LED_CAPS_OFF);
    control(WN8030_CMD_INDICATOR, WN8030_LED_MIC_OFF);
    control(WN8030_CMD_BACKLIGHT, on ? kb->backlight : 0);
}

void attachCover() {
    Keyboard *kb = gKb;
    kb->attached = true;
    kb->caps = false;                 // the controller keeps the Caps Lock LED between attaches; the system starts with it off
    applyInputState();
    kb->hid = createHid();
    if (kb->hid) sheng_keyboard_cover_enable(kb->hid, kb->inputEnabled);
    else bump(8, 1 << 24);
    kb->keepalive = deadlineMs(kKeepaliveMs);
    native_keyboard_stats[8] = (native_keyboard_stats[8] & ~0xff) + 0x100 + 1;
}

void detachCover() {
    Keyboard *kb = gKb;
    kb->attached = false;
    if (IOService *hid = kb->hid) {
        kb->hid = nullptr;
        hid->terminate();
        hid->release();
    }
    native_keyboard_stats[8] = native_keyboard_stats[8] & ~0xff;
}

void readHall() {
    Keyboard *kb = gKb;
    kb->lid = !level(PinLid);
    kb->tablet = !level(PinTablet);
    native_keyboard_stats[9] = (native_keyboard_stats[9] & ~3) | (kb->lid ? 1 : 0) | (kb->tablet ? 2 : 0);
    const bool enabled = !kb->lid && !kb->tablet;
    if (enabled == kb->inputEnabled) return;
    kb->inputEnabled = enabled;
    native_keyboard_stats[10] = enabled;
    if (!enabled) releaseInputs();
    if (kb->hid) sheng_keyboard_cover_enable(kb->hid, enabled);
    applyInputState();
}

void handleFrame() {
    Keyboard *kb = gKb;
    if (!readFrame()) return;
    bump(6);
    Wn8030Frame f;
    switch (wn8030_parse(kb->frame, &f)) {
    case Wn8030Keyboard: case Wn8030Consumer: case Wn8030Touchpad:
        if (kb->hid && kb->inputEnabled) {
            uint8_t touchpad[WN8030_TOUCHPAD_REPORT];
            if (f.kind == Wn8030Touchpad) {
                for (uint32_t i = 0; i < WN8030_TOUCHPAD_FINGERS; ++i) {
                    const uint8_t *finger = f.report + 2 + 6 * i;
                    if (!(finger[0] & 1u)) continue;
                    const int32_t x = finger[2] | finger[3] << 8, y = finger[4] | finger[5] << 8;
                    volatile int32_t *r = native_keyboard_touch_range;
                    if (r[0] < 0 || x < r[0]) r[0] = x;
                    if (x > r[1]) r[1] = x;
                    if (r[2] < 0 || y < r[2]) r[2] = y;
                    if (y > r[3]) r[3] = y;
                }
                bump(16, (int32_t)wn8030_touchpad_report(f.report, touchpad));
            }
            sheng_keyboard_cover_input(kb->hid, f.kind == Wn8030Touchpad ? touchpad : f.report, f.length);
            bump(7, f.kind == Wn8030Keyboard ? 1 : f.kind == Wn8030Consumer ? 0x100 : 0x10000);
        }
        break;
    case Wn8030Attach:
        if (f.attached == 1 && !kb->attached) attachCover();
        else if (f.attached == 0 && kb->attached) detachCover();
        break;
    case Wn8030Accel:
        native_keyboard_accel[0] = f.x; native_keyboard_accel[1] = f.y; native_keyboard_accel[2] = f.z;
        native_keyboard_accel[3] = native_keyboard_accel[3] + 1;
        break;
    case Wn8030AuthRequest: case Wn8030AuthInit: case Wn8030AuthChallenge:
        bump(15);
        break;
    case Wn8030Invalid: break;
    default: bump(15, 0x10000); break;
    }
}

void serve() {
    Keyboard *kb = gKb;
    for (;;) {
        while (kb->mcuSeen != __atomic_load_n(&kb->mcuIrqs, __ATOMIC_ACQUIRE)) {
            ++kb->mcuSeen;
            native_keyboard_stats[5] = (int32_t)kb->mcuSeen;
            handleFrame();
        }
        const uint32_t hall = __atomic_load_n(&kb->hallIrqs, __ATOMIC_ACQUIRE);
        if (hall != kb->hallSeen) {
            kb->hallSeen = hall;
            kb->hallPending = true;
            kb->hallDue = deadlineMs(kDebounceMs);
            native_keyboard_stats[9] = (native_keyboard_stats[9] & 3) | (int32_t)(hall << 8);
        }
        const uint64_t now = mach_absolute_time();
        if (kb->hallPending && now >= kb->hallDue) {kb->hallPending = false; readHall();}
        const uint32_t requests = __atomic_load_n(&kb->capsRequests, __ATOMIC_ACQUIRE);
        if (requests != kb->capsSeen) {
            kb->capsSeen = requests;
            kb->caps = __atomic_load_n(&kb->capsWanted, __ATOMIC_ACQUIRE) != 0;
            if (kb->attached && kb->inputEnabled) control(WN8030_CMD_INDICATOR, kb->caps ? WN8030_LED_CAPS_ON : WN8030_LED_CAPS_OFF);
        }
        if (kb->attached && now >= kb->keepalive) {
            control(WN8030_CMD_KEYBOARD_POWER, 1);
            kb->keepalive = deadlineMs(kKeepaliveMs);
        }
        uint64_t deadline = 0;
        if (kb->attached) deadline = kb->keepalive;
        if (kb->hallPending && (!deadline || kb->hallDue < deadline)) deadline = kb->hallDue;
        if (deadline) assert_wait_deadline((event_t)&gWake, THREAD_UNINT, deadline);
        else assert_wait((event_t)&gWake, THREAD_UNINT);
        if (kb->mcuSeen != __atomic_load_n(&kb->mcuIrqs, __ATOMIC_ACQUIRE) || kb->hallSeen != __atomic_load_n(&kb->hallIrqs, __ATOMIC_ACQUIRE) ||
            kb->capsSeen != __atomic_load_n(&kb->capsRequests, __ATOMIC_ACQUIRE))
            clear_wait(current_thread(), THREAD_AWAKENED);
        else thread_block(THREAD_CONTINUE_NULL);
    }
}

void keyboardThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng-keyboard");
    Keyboard *kb = gKb;
    stage(1);
    if (!setupPins()) return;
    stage(2);
    if (!setupRails()) return;
    stage(3);
    if (!setupClocks() || !setupBus()) return;
    stage(4);
    if (!setupInterrupts()) return;
    // The hall state before the cover can attach.
    kb->lid = !level(PinLid);
    kb->tablet = !level(PinTablet);
    kb->inputEnabled = !kb->lid && !kb->tablet;
    native_keyboard_stats[9] = (kb->lid ? 1 : 0) | (kb->tablet ? 2 : 0);
    native_keyboard_stats[10] = kb->inputEnabled;
    stage(5);
    if (!powerOn()) return;
    stage(6);
    if (!loadImage()) return;
    sheng_keyboard_cover_output_sink(ledRequest, nullptr);
    stage(7);
    serve();
}
}  // namespace

extern "C" void sheng_keyboard_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("sheng-keyboard", &enabled, sizeof enabled) || enabled != 1 || gKb) return;
    Keyboard *kb = static_cast<Keyboard *>(IOMallocType(Keyboard));
    if (!kb) {fail(8, -12); return;}
    bzero(kb, sizeof *kb);
    kb->busLock = IOSimpleLockAlloc();
    if (!kb->busLock) {IOFreeType(kb, Keyboard); fail(8, -12); return;}
    unsigned khz = 1000, backlight = 0;
    PE_parse_boot_argn("sheng-keyboard-khz", &khz, sizeof khz);
    PE_parse_boot_argn("sheng-keyboard-backlight", &backlight, sizeof backlight);
    kb->khz = khz == 100 || khz == 400 ? khz : 1000;
    kb->backlight = static_cast<uint8_t>(backlight > WN8030_BACKLIGHT_MAX ? WN8030_BACKLIGHT_MAX : backlight);
    gKb = kb;
    thread_t thread = nullptr;
    if (kernel_thread_start(keyboardThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(8, -12);
}
