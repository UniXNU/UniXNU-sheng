// SPDX-License-Identifier: BSD-2-Clause-Patent
// Panel brightness for the sheng board (boot argument sheng-backlight=1, started by BacklightProbe.cpp). The panel's backlight is two Kinetic KTZ8866 drivers at I2C
// address 0x11, one on the QUP wrap1 serial engine 1 (i2c@a84000, "ktz8866a") and one on the I2C hub engine 2 (i2c@988000, "ktz8866b");
// both get the same 11-bit level in BL_BRT_LSB (0x04, bits 2:0) and BL_BRT_MSB (0x05), as the mainline ktz8866 driver writes it.
// The engines run the GENI I2C firmware the boot loaders left loaded and clocked; this code votes their clocks on again, keeps the clock
// and SCL timing the boot loader set and only issues FIFO transfers (i2c-qcom-geni: M_CMD opcode 1 write / 2 read, slave address in
// bits 15:9, STOP_STRETCH bit 2, TX/RX length registers 0x26c/0x270).
// The class cannot be called AppleARMBacklight: AppleARMPlatform already defines that class, and a duplicate name panics the kernel when
// this file's classes are registered (int-229). Whether CoreBrightness finds ShengBacklight is what native_backlight_stats[1] (brightness
// sets) shows. CoreBrightness (backboardd) looks for a backlight service with backlight-control = true, reads min and max from
// IODisplayParameters brightness and sets {brightness, FadePeriod (16.16 seconds)} through setProperties. A fade is stepped every 16 ms by a
// timer on the service's own work loop.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <pexpert/pexpert.h>

// [0] stage (1 mapped, 2 engines ready, 3 published, negative: the step that failed), [1] brightness requests, [2] last level written,
// [3] level read back from engine a at start, [4] from engine b, [5] transfer errors, [6] last M_IRQ status of a failed transfer,
// [7] engine a protocol and clock configuration (proto << 24 | M_CLK_CFG), [8] the same for engine b, [9] last error code
extern "C" {extern volatile int32_t native_backlight_stats[10];}

namespace {
constexpr uint64_t kGcc = 0x100000, kSeA = 0xa84000, kSeB = 0x988000;
constexpr uint32_t kGccVote = 0x52008;
// gcc-sm8550.c: wrap1 core_2x 18, core 19, m_ahb 20, s_ahb 21, s1 23; i2c hub s_ahb 7, core 8, s2 12.
constexpr uint32_t kVoteBits = (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 23) | (1u << 7) | (1u << 8) | (1u << 12);
constexpr uint32_t kHalt[] = {0x23294, 0x23284, 0x2327c, 0x23280, 0x1813c, 0x23140, 0x23144, 0x1703c};
constexpr uint32_t kFwRevision = 0x68, kIfDisable = 0x64, kClkCfg = 0x48, kDmaMode = 0x258, kByteGran = 0x254;
constexpr uint32_t kTxPack0 = 0x260, kTxPack1 = 0x264, kRxPack0 = 0x284, kRxPack1 = 0x288, kSclCounters = 0x278;
constexpr uint32_t kTxLen = 0x26c, kRxLen = 0x270, kCmd0 = 0x600, kIrqStatus = 0x610, kIrqClear = 0x618;
constexpr uint32_t kTxFifo = 0x700, kRxFifo = 0x780, kRxFifoStatus = 0x804, kTxWatermark = 0x80c;
constexpr uint32_t kCmdDone = 1u << 0, kTxWatermarkIrq = 1u << 30, kErrors = (1u << 1) | (1u << 2) | (1u << 3) | (0x3fu << 9);
constexpr uint32_t kProtoI2c = 3, kOpWrite = 1, kOpRead = 2, kStopStretch = 1u << 2;
constexpr uint8_t kAddress = 0x11, kBrtLsb = 0x04, kBrtMsb = 0x05;
constexpr uint32_t kMaxLevel = 2047, kStepMs = 16;
constexpr unsigned kSpins = 200000;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
Window gGcc, gSe[2];
IOLock *gBusLock;

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
uint32_t rd(volatile uint32_t *se, uint32_t reg) {return se[reg / 4];}
void wr(volatile uint32_t *se, uint32_t reg, uint32_t value) {se[reg / 4] = value; __asm__ volatile("dsb sy" ::: "memory");}
void fail(int step, int error) {native_backlight_stats[9] = error; native_backlight_stats[0] = -step;}

int waitIrq(volatile uint32_t *se, uint32_t bit) {
    for (unsigned n = 0; n < kSpins; ++n) {
        const uint32_t status = rd(se, kIrqStatus);
        if (status & kErrors) {native_backlight_stats[6] = (int32_t)status; wr(se, kIrqClear, status); return 2;}
        if (status & bit) {wr(se, kIrqClear, status & bit); return 0;}
    }
    native_backlight_stats[6] = (int32_t)rd(se, kIrqStatus);
    return 1;
}
// One write of up to 4 bytes to the slave; stop is held when a read follows.
int i2cWrite(volatile uint32_t *se, const uint8_t *bytes, unsigned length, bool holdStop) {
    wr(se, kIrqClear, 0xffffffffu);
    wr(se, kTxLen, length);
    wr(se, kCmd0, (kOpWrite << 27) | ((uint32_t)kAddress << 9) | (holdStop ? kStopStretch : 0));
    wr(se, kTxWatermark, 1);
    int rc = waitIrq(se, kTxWatermarkIrq);
    if (rc) return 10 + rc;
    uint32_t word = 0;
    for (unsigned i = 0; i < length; ++i) word |= (uint32_t)bytes[i] << (8 * i);
    wr(se, kTxFifo, word);
    wr(se, kTxWatermark, 0);
    rc = waitIrq(se, kCmdDone);
    return rc ? 20 + rc : 0;
}
int i2cReadRegister(volatile uint32_t *se, uint8_t reg, uint8_t *value) {
    int rc = i2cWrite(se, &reg, 1, true);
    if (rc) return rc;
    wr(se, kIrqClear, 0xffffffffu);
    wr(se, kRxLen, 1);
    wr(se, kCmd0, (kOpRead << 27) | ((uint32_t)kAddress << 9));
    rc = waitIrq(se, kCmdDone);
    if (rc) return 30 + rc;
    if (!(rd(se, kRxFifoStatus) & 0x1ffffffu)) return 40;
    *value = (uint8_t)rd(se, kRxFifo);
    return 0;
}
// Packing for 8-bit words, 4 per FIFO word, most significant bit first (the values i2c-qcom-geni programs).
bool prepareEngine(int index) {
    volatile uint32_t *se = gSe[index].va;
    const uint32_t proto = (rd(se, kFwRevision) >> 8) & 0xff, clock = rd(se, kClkCfg);
    native_backlight_stats[7 + index] = (int32_t)((proto << 24) | (clock & 0xffffff));
    if (proto != kProtoI2c || (rd(se, kIfDisable) & 1) || !(clock & 1) || !rd(se, kSclCounters)) return false;
    wr(se, kDmaMode, 0);
    wr(se, kTxPack0, 0x7f8fe); wr(se, kTxPack1, 0xffefe);
    wr(se, kRxPack0, 0x7f8fe); wr(se, kRxPack1, 0xffefe);
    wr(se, kByteGran, 0);
    return true;
}
int readLevel(volatile uint32_t *se) {
    uint8_t lsb = 0, msb = 0;
    if (i2cReadRegister(se, kBrtLsb, &lsb) || i2cReadRegister(se, kBrtMsb, &msb)) return -1;
    return (int)(((uint32_t)msb << 3) | (lsb & 7));
}
void writeLevel(uint32_t level) {
    IOLockLock(gBusLock);
    for (int i = 0; i < 2; ++i) {
        const uint8_t lsb[2] = {kBrtLsb, (uint8_t)(level & 7)}, msb[2] = {kBrtMsb, (uint8_t)((level >> 3) & 0xff)};
        int rc = i2cWrite(gSe[i].va, lsb, 2, false);
        if (!rc) rc = i2cWrite(gSe[i].va, msb, 2, false);
        if (rc) {native_backlight_stats[5] = native_backlight_stats[5] + 1; native_backlight_stats[9] = rc;}
    }
    IOLockUnlock(gBusLock);
    native_backlight_stats[2] = (int32_t)level;
}
}  // namespace

class ShengBacklight : public IOService {
    OSDeclareDefaultStructors(ShengBacklight)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *fade = nullptr;
    uint32_t level = 0, from = 0, to = 0;
    uint64_t start = 0, period = 0;
    static void fadeStep(OSObject *owner, IOTimerEventSource *sender) {
        ShengBacklight *self = OSDynamicCast(ShengBacklight, owner);
        if (!self) return;
        const uint64_t elapsed = mach_absolute_time() - self->start;
        uint32_t next = self->to;
        if (elapsed < self->period)
            next = (uint32_t)((int64_t)self->from + ((int64_t)self->to - (int64_t)self->from) * (int64_t)elapsed / (int64_t)self->period);
        if (next != self->level) {writeLevel(next); self->level = next;}
        if (next != self->to) sender->setTimeoutMS(kStepMs);
    }
    static IOReturn startFade(OSObject *owner, void *target, void *milliseconds, void *, void *) {
        ShengBacklight *self = OSDynamicCast(ShengBacklight, owner);
        if (!self) return kIOReturnBadArgument;
        self->fade->cancelTimeout();
        self->from = self->level;
        self->to = (uint32_t)(uintptr_t)target;
        self->start = mach_absolute_time();
        nanoseconds_to_absolutetime((uint64_t)(uintptr_t)milliseconds * NSEC_PER_MSEC, &self->period);
        fadeStep(self, self->fade);
        return kIOReturnSuccess;
    }
    void publish(uint32_t value) {
        OSDictionary *brightness = OSDictionary::withCapacity(3), *parameters = OSDictionary::withCapacity(1);
        OSNumber *min = OSNumber::withNumber(0ULL, 32), *max = OSNumber::withNumber(kMaxLevel, 32), *now = OSNumber::withNumber(value, 32);
        if (brightness && parameters && min && max && now) {
            brightness->setObject("min", min); brightness->setObject("max", max); brightness->setObject("value", now);
            parameters->setObject("brightness", brightness);
            setProperty("IODisplayParameters", parameters);
        }
        OSSafeReleaseNULL(min); OSSafeReleaseNULL(max); OSSafeReleaseNULL(now); OSSafeReleaseNULL(brightness); OSSafeReleaseNULL(parameters);
    }
public:
    bool startBacklight(uint32_t initial) {
        loop = IOWorkLoop::workLoop();
        fade = IOTimerEventSource::timerEventSource(this, &ShengBacklight::fadeStep);
        if (!loop || !fade || loop->addEventSource(fade) != kIOReturnSuccess) return false;
        level = to = initial;
        // backlight-control makes QuartzCore's CADeviceHasBacklight true, and then IOMFBServer::register_libedr_callbacks does a
        // synchronous libEDR send to com.apple.libEDR.Notification, which backboardd never checks in here: the send blocks with the
        // display server lock held and the screen stops updating (int-236). Only publish it with sheng-backlight-control=1.
        uint32_t control = 0;
        if (PE_parse_boot_argn("sheng-backlight-control", &control, sizeof control) && control == 1) setProperty("backlight-control", true);
        publish(initial);
        registerService();
        return true;
    }
    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);
        if (!dictionary) return kIOReturnBadArgument;
        OSNumber *brightness = OSDynamicCast(OSNumber, dictionary->getObject("brightness"));
        if (!brightness) return kIOReturnSuccess;
        OSNumber *fadePeriod = OSDynamicCast(OSNumber, dictionary->getObject("FadePeriod"));
        const int32_t requested = (int32_t)brightness->unsigned32BitValue();
        const uint32_t target = requested < 0 ? 0 : (uint32_t)requested > kMaxLevel ? kMaxLevel : (uint32_t)requested;
        uint64_t milliseconds = fadePeriod ? ((uint64_t)fadePeriod->unsigned32BitValue() * 1000) >> 16 : 0;
        if (milliseconds > 5000) milliseconds = 5000;
        native_backlight_stats[1] = native_backlight_stats[1] + 1;
        loop->runAction(&ShengBacklight::startFade, this, (void *)(uintptr_t)target, (void *)(uintptr_t)milliseconds);
        publish(target);
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(ShengBacklight, IOService)

// Called by BacklightProbe.cpp once this file's classes are registered (its static initializer runs between preModLoad and postModLoad).
extern "C" void sheng_backlight_start(void) {
    gBusLock = IOLockAlloc();
    if (!gBusLock || !mapWindow(gGcc, kGcc, 0x60000) || !mapWindow(gSe[0], kSeA, 0x4000) || !mapWindow(gSe[1], kSeB, 0x4000)) {fail(1, -12); return;}
    native_backlight_stats[0] = 1;
    wr(gGcc.va, kGccVote, rd(gGcc.va, kGccVote) | kVoteBits);
    for (uint32_t halt : kHalt) {
        // The AHB branches run under hardware clock gating (HW_CTL, bit 1 of the branch register): their CLK_OFF bit says off while the
        // bus is idle, so, as clk-branch.c does in that mode, they are not waited for (int-231 failed here on wrap 1 s_ahb, 0x23280).
        if (rd(gGcc.va, halt) & (1u << 1)) continue;
        unsigned n = 0;
        while ((rd(gGcc.va, halt) & (1u << 31)) && n < kSpins) ++n;
        if (n == kSpins) {fail(2, (int)halt); return;}
    }
    if (!prepareEngine(0) || !prepareEngine(1)) {fail(3, -19); return;}
    native_backlight_stats[3] = readLevel(gSe[0].va);
    native_backlight_stats[4] = readLevel(gSe[1].va);
    native_backlight_stats[0] = 2;
    const int initial = native_backlight_stats[3] > 0 ? native_backlight_stats[3] : (int)kMaxLevel / 2;
    ShengBacklight *backlight = OSTypeAlloc(ShengBacklight);
    if (!backlight || !backlight->init() || !backlight->attach(IOService::getPlatform()) || !backlight->startBacklight((uint32_t)initial)) {
        if (backlight) backlight->release();
        fail(4, -1); return;
    }
    backlight->release();
    native_backlight_stats[0] = 3;
}
