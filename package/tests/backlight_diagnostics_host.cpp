// SPDX-License-Identifier: BSD-2-Clause-Patent
// The runner inserts the actual production declarations/methods below. Only host services are faked.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include "GeniI2c.h"
#include "Ktz8866Diagnostics.h"
#include "LidClamshell.h"

static unsigned checks;
static void check(bool ok, const char *why) {
    ++checks;
    if (!ok) {std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1);}
}
static int numberBudget = -1;
struct OSObject {
    unsigned refs = 1;
    bool immortal = false;
    virtual ~OSObject() = default;
    void retain() {if (!immortal) ++refs;}
    void release() {if (!immortal && !--refs) delete this;}
};
struct OSBoolean : OSObject {
    bool value;
    explicit OSBoolean(bool v) : value(v) {immortal = true;}
};
static OSBoolean yes(true), no(false);
static OSBoolean *kOSBooleanTrue = &yes, *kOSBooleanFalse = &no;
struct OSNumber : OSObject {
    uint64_t value;
    explicit OSNumber(uint64_t v) : value(v) {}
    static OSNumber *withNumber(uint64_t v, unsigned bits) {
        if (!numberBudget) return nullptr;
        if (numberBudget > 0) --numberBudget;
        return new OSNumber(bits == 32 ? (uint32_t)v : v);
    }
    uint32_t unsigned32BitValue() const {return (uint32_t)value;}
};
struct OSDictionary : OSObject {
    std::map<std::string, OSObject *> entries;
    static OSDictionary *withCapacity(unsigned) {return new OSDictionary;}
    ~OSDictionary() override {for (auto &entry : entries) entry.second->release();}
    bool setObject(const char *key, OSObject *value) {
        if (!value) return false;
        value->retain();
        const auto old = entries.find(key);
        if (old != entries.end()) old->second->release();
        entries[key] = value;
        return true;
    }
    OSObject *getObject(const char *key) const {
        const auto it = entries.find(key);
        return it == entries.end() ? nullptr : it->second;
    }
};
#define OSDynamicCast(type, value) dynamic_cast<type *>(value)
#define OSSafeReleaseNULL(value) do {if (value) {(value)->release(); (value) = nullptr;}} while (0)
using IOReturn = int;
using AbsoluteTime = uint64_t;
constexpr IOReturn kIOReturnSuccess = 0, kIOReturnBadArgument = -1;
struct IOService : OSObject {
    OSDictionary properties;
    unsigned replacements = 0;
    virtual IOReturn setProperties(OSObject *) {return 0;}
    bool setProperty(const char *key, OSObject *value) {++replacements; return properties.setObject(key, value);}
    bool setProperty(const char *key, bool value) {return setProperty(key, value ? kOSBooleanTrue : kOSBooleanFalse);}
    bool setProperty(const char *key, unsigned long long value, unsigned bits) {
        OSNumber *n = OSNumber::withNumber(value, bits);
        if (!n) return false;
        const bool ok = setProperty(key, n); n->release(); return ok;
    }
    bool setProperty(const char *, const char *) {return true;}
    void registerService() {}
};
static bool timerAllocationFails;
static unsigned timerObjects;
[[maybe_unused]] static void IOLog(const char *, ...) {}
struct IOTimerEventSource : OSObject {
    IOTimerEventSource() {++timerObjects;}
    ~IOTimerEventSource() override {--timerObjects;}
    template<class Action> static IOTimerEventSource *timerEventSource(OSObject *, Action) {
        return timerAllocationFails ? nullptr : new IOTimerEventSource;
    }
    bool armed = false;
    uint32_t ms = 0, us = 0;
    uint64_t deadline = 0;
    void setTimeoutMS(uint32_t v) {armed = true; ms = v;}
    void setTimeoutUS(uint32_t v) {armed = true; us = v;}
    void cancelTimeout() {armed = false;}
    void wakeAtTime(AbsoluteTime v) {armed = true; deadline = v;}
};
struct IOWorkLoop {
    bool reject = false;
    int addEventSource(IOTimerEventSource *) {return reject ? 1 : 0;}
};
struct IOInterruptEventSource {};
static constexpr uint64_t NSEC_PER_MSEC = 1000000;
static uint64_t clockNow;
static uint64_t mach_absolute_time() {return clockNow;}
static void nanoseconds_to_absolutetime(uint64_t n, uint64_t *a) {*a = n;}
static bool PE_parse_boot_argn(const char *, void *, unsigned) {return false;}

struct Transfer {bool reading; uint8_t reg, value;};
struct Hardware {
    uint32_t regs[0x1000 / 4] = {};
    uint8_t chip[256] = {};
    uint8_t rx = 0;
    bool rxReady = false;
    std::vector<Transfer> trace;
    std::vector<uint32_t> controls;
    Hardware() {
        regs[GENI_FW_REVISION / 4] = GENI_PROTO_I2C << 8;
        regs[GENI_CLK_CFG / 4] = 1;
        regs[GENI_SCL_COUNTERS / 4] = 1;
        chip[2] = 0xfa; chip[3] = 0x85; chip[0x15] = 0x6a;
        chip[4] = 1000 & 7; chip[5] = 1000 >> 3;
    }
};
static uint32_t hostRead(void *context, uint32_t reg) {
    Hardware &h = *static_cast<Hardware *>(context);
    if (reg == GENI_RX_FIFO_STATUS) return h.rxReady ? 1 : 0;
    if (reg == GENI_RX_FIFO) {h.rxReady = false; return h.rx;}
    return h.regs[reg / 4];
}
static void hostWrite(void *context, uint32_t reg, uint32_t value) {
    Hardware &h = *static_cast<Hardware *>(context);
    h.regs[reg / 4] = value;
    if (reg == GENI_M_IRQ_CLEAR) h.regs[GENI_M_IRQ_STATUS / 4] &= ~value;
    if (reg == GENI_M_CMD_CTRL) h.controls.push_back(value);
    if (reg == GENI_TX_FIFO)
        h.trace.push_back({h.regs[GENI_TX_LEN / 4] == 1, (uint8_t)value, (uint8_t)(value >> 8)});
}
static uint32_t rd(Hardware *h, uint32_t reg) {return hostRead(h, reg);}
static void wr(Hardware *h, uint32_t reg, uint32_t v) {hostWrite(h, reg, v);}
static struct {Hardware *va;} gSe[2];
extern "C" {
volatile int32_t native_backlight_stats[16];
volatile int32_t native_backlight_request[6];
volatile int32_t native_backlight_ktz[2][KtzDiagCount];
size_t host_bootlog_ktz(char *, size_t);
}
static volatile uint32_t gIrqStatus[2], gLidClosed, gSystemAsleep;
static volatile uint64_t gRequest;
static void bump(int i) {native_backlight_stats[i] = native_backlight_stats[i] + 1;}
static constexpr uint8_t kBrtLsb = 4, kBrtMsb = 5;
/* PRODUCTION_CONSTANTS */
class ShengBacklight : public IOService {
public:
/* PRODUCTION_STATE */
    static inline const void *gAppleClass = nullptr;
    bool impersonate() {return false;}
    void publishCalibration() {} // Untouched calibration is independently hash-checked against the saved input.
/* PRODUCTION_METHODS */
};
static OSDictionary *chipDictionary(ShengBacklight &s, unsigned i) {
    return dynamic_cast<OSDictionary *>(s.properties.getObject(i ? "KTZ8866B" : "KTZ8866A"));
}
static int32_t number(const OSDictionary *d, const char *key) {
    check(d != nullptr, "dictionary exists");
    const OSNumber *n = dynamic_cast<OSNumber *>(d->getObject(key));
    check(n != nullptr, key);
    return (int32_t)n->unsigned32BitValue();
}
static bool boolean(const OSDictionary *d, const char *key) {
    check(d != nullptr, "boolean dictionary exists");
    const OSBoolean *b = dynamic_cast<OSBoolean *>(d->getObject(key));
    check(b != nullptr, key);
    return b->value;
}
struct Fixture {
    ShengBacklight s;
    IOTimerEventSource fade, tick, request, lid, diagnostic;
    Hardware hw[2];
    Fixture(bool a = true, bool b = true) {
        clockNow = 0; gRequest = 0; gLidClosed = 0; gSystemAsleep = 0;
        for (unsigned i = 0; i < 16; ++i) native_backlight_stats[i] = 0;
        for (unsigned i = 0; i < 6; ++i) native_backlight_request[i] = 0;
        s.fade = &fade; s.tick = &tick; s.request = &request; s.lid = &lid; s.diagnostic = &diagnostic;
        s.published = true; s.level = s.to = s.target = 1000;
        for (unsigned i = 0; i < 2; ++i) {
            gIrqStatus[i] = 0; gSe[i].va = &hw[i];
            for (unsigned k = 0; k < KtzDiagCount; ++k) native_backlight_ktz[i][k] = 0;
            const GeniIo io = {hostRead, hostWrite, &hw[i]};
            check(geni_i2c_prepare(&s.engine[i].bus, &io, 0x11, nullptr) == 0, "GENI prepare");
            s.engine[i].usable = i ? b : a;
            s.engine[i].started = true; s.engine[i].written = 1000;
            ktz_snapshot_init(&s.snapshot[i], s.engine[i].usable); s.publishKtz(i);
        }
    }
    void sample() {ShengBacklight::diagnosticFired(&s, &diagnostic);}
    void finish(unsigned i) {
        GeniI2c &bus = s.engine[i].bus;
        check(bus.phase == GeniTxWait, "transfer begins through real GENI TX state");
        const bool reading = bus.reading;
        const uint8_t reg = bus.reg, value = bus.value;
        s.advance(i, GENI_M_TX_WATERMARK);
        if (!reading) hw[i].chip[reg] = value;
        s.advance(i, GENI_M_CMD_DONE);
        if (reading) {
            check(bus.phase == GeniReadDone, "register address uses repeated-start read");
            hw[i].rx = hw[i].chip[reg]; hw[i].rxReady = true;
            s.advance(i, GENI_M_CMD_DONE);
        }
    }
    void drain(unsigned i) {
        unsigned count = 0;
        while (geni_i2c_busy(&s.engine[i].bus) && count++ < 16) finish(i);
        check(count <= 16, "bounded completion without synchronous service loop");
    }
    void both() {drain(0); drain(1);}
    void fail(unsigned i, unsigned how) {
        if (how == 0) s.advance(i, GENI_M_CMD_FAILURE | GENI_M_CMD_DONE);
        else if (how == 1) {
            s.irqMode = true;
            const uint64_t first = s.engine[i].deadline;
            clockNow = first - 1; s.service();
            check(hw[i].controls.empty(), "no timeout before existing 25ms deadline");
            for (unsigned j = 0; j < 3; ++j) {
                clockNow = s.engine[i].deadline; s.service();
                if (j < 2) check(geni_i2c_busy(&s.engine[i].bus), "cancel/abort remain asynchronous");
            }
            check(clockNow == first + 50 * NSEC_PER_MSEC, "original three 25ms timeout phases");
            check(hw[i].controls.size() == 2 && hw[i].controls[0] == GENI_CMD_CTRL_CANCEL &&
                  hw[i].controls[1] == GENI_CMD_CTRL_ABORT, "original cancel then abort");
        } else {
            s.advance(i, GENI_M_TX_WATERMARK | GENI_M_CMD_DONE);
            s.advance(i, GENI_M_CMD_DONE); // RX FIFO is empty.
        }
    }
};

static void testEligibilityAndSuccess() {
    Fixture f;
    check(number(chipDictionary(f.s, 0), "Error") == KtzNotSampled, "not sampled is explicit");
    check(!chipDictionary(f.s, 0)->getObject("CFG1"), "initial zero is never advertised as CFG1");
    f.s.published = false; f.sample(); check(!f.s.snapshot[0].active, "no sampling before publication");
    f.s.published = true; f.s.fading = true; f.sample(); check(!f.s.snapshot[0].active, "no sampling during fade");
    f.s.fading = false; gRequest = 1ull << 63; f.sample(); check(!f.s.snapshot[0].active, "pending request prevents sample");
    gRequest = 0; f.s.engine[1].written = 999; f.sample(); check(!f.s.snapshot[0].active, "both engines must reach target");
    f.s.engine[1].written = 1000;
    f.hw[1].chip[2] = 0; f.hw[1].chip[0x15] = 0x42; f.hw[1].chip[4] = 0xff; f.hw[1].chip[5] = 0x80;
    f.sample();
    check(f.diagnostic.ms == 1000, "one-second diagnostic timer");
    check(f.s.snapshot[0].active && f.s.snapshot[1].active, "independent samples started");
    OSDictionary *inProgress = chipDictionary(f.s, 0); inProgress->retain();
    const unsigned replacements = f.s.replacements;
    for (unsigned k = 0; k < 5; ++k) {
        check(f.s.engine[0].bus.reg == ktz_snapshot_registers[k], "fixed 02/03/15/04/05 order");
        check(f.s.engine[0].bus.reading, "diagnostics never write configuration");
        f.finish(0);
        if (k < 4) check(f.s.replacements == replacements, "one dictionary replacement at sample end");
    }
    check(chipDictionary(f.s, 0) != inProgress, "dictionary replaced atomically");
    check(!inProgress->getObject("CFG1") && boolean(inProgress, "InProgress"), "old dictionary stays immutable");
    inProgress->release();
    check(boolean(chipDictionary(f.s, 0), "SnapshotValid"), "complete sample valid");
    check(number(chipDictionary(f.s, 0), "ReadbackLevel") == 1000, "actual brightness read");
    check(number(chipDictionary(f.s, 0), "LastSuccessfulWrite") == -1, "reads do not count as writes");
    check(!boolean(chipDictionary(f.s, 0), "LastSuccessfulWriteValid"), "unknown write explicit");
    f.sample(); check(f.s.snapshot[0].sequence == 1, "busy peer blocks concurrent refresh");
    f.drain(1);
    check(number(chipDictionary(f.s, 1), "ReadbackLevel") == 1031, "LSB high bits masked");
    check(number(chipDictionary(f.s, 1), "CFG1") == 0 && boolean(chipDictionary(f.s, 1), "SnapshotValid"), "measured zero can be valid");
    check(number(chipDictionary(f.s, 0), "CFG1") == 0xfa, "chip configuration independent");
    char log[1024] = {};
    host_bootlog_ktz(log, sizeof log);
    check(std::strstr(log, "KTZ a seq=1 v=1f e=0/0 cfg=250/133 cur=106 br=1000 tgt=1000 ok=-1"), "actual BootLog a line");
    check(std::strstr(log, "KTZ b seq=1 v=1f e=0/0 cfg=0/133 cur=66 br=1031"), "actual BootLog b line");
    check(!std::strstr(log, "mode="), "KTZ lines do not redefine GENI mode");
    // Inject an allocation failure while assembling the next dictionary: readers retain the old complete one.
    OSDictionary *previous = chipDictionary(f.s, 0); previous->retain();
    numberBudget = 3; f.s.publishKtz(0); numberBudget = -1;
    check(chipDictionary(f.s, 0) == previous, "allocation failure never publishes partial dictionary");
    previous->release();
}

static void testFailedWritesAreNotSuccess() {
    for (unsigned failedStep = 0; failedStep < 2; ++failedStep) {
        Fixture f;
        f.s.setLevel(1100); f.both();
        check(f.s.snapshot[0].lastSuccessfulWrite == 1100, "successful MSB records write");
        f.s.setLevel(1200);
        check(f.s.snapshot[0].lastSuccessfulWrite == 1100, "request alone not successful");
        if (failedStep) {f.finish(0); check(f.s.snapshot[0].lastSuccessfulWrite == 1100, "LSB alone not successful");}
        f.fail(0, 0); f.drain(1);
        check(f.s.engine[0].written == 1200, "existing give-up scheduling preserved");
        check(f.s.snapshot[0].lastSuccessfulWrite == 1100, "give-up is not last successful write");
        f.sample(); f.both();
        check(number(chipDictionary(f.s, 0), "SnapshotTarget") == 1200, "sample target differs from success");
        check(number(chipDictionary(f.s, 0), "LastSuccessfulWrite") == 1100, "dictionary reports actual successful write");
        check(number(chipDictionary(f.s, 0), "ReadbackLevel") != 1200, "readback can disprove failed target");
        f.s.setLevel(1300); f.both();
        check(f.s.snapshot[0].lastSuccessfulWrite == 1300, "later brightness recovers after failed write");
    }
}

static void testReadFailures() {
    for (unsigned engine = 0; engine < 2; ++engine) for (unsigned index = 0; index < 5; ++index) for (unsigned how = 0; how < 3; ++how) {
        Fixture f; f.sample(); f.drain(1 - engine);
        for (unsigned k = 0; k < index; ++k) f.finish(engine);
        f.fail(engine, how);
        const int32_t error = how == 0 ? GeniErrBus : how == 1 ? GeniErrTimeout : GeniErrNoData;
        check(!f.s.snapshot[engine].active && !geni_i2c_busy(&f.s.engine[engine].bus), "read failure terminates sample");
        check(f.s.snapshot[engine].error == error && f.s.snapshot[engine].errors == 1, "per-chip error retained");
        check(f.s.snapshot[engine].validMask == (1u << index) - 1, "only successful reads valid");
        check(f.s.snapshot[1 - engine].validMask == KtzAllValid && f.s.snapshot[1 - engine].error == 0, "peer failure isolation");
        OSDictionary *d = chipDictionary(f.s, engine);
        check(!boolean(d, "SnapshotValid") && !d->getObject("ReadbackLevel"), "partial read never claims level");
        const char *keys[] = {"CFG1", "CFG2", "CurrentReg", "BrightnessLSB", "BrightnessMSB"};
        for (unsigned k = index; k < 5; ++k) check(!d->getObject(keys[k]), "invalid raw register key omitted");
        f.s.service(); check(f.s.snapshot[engine].sequence == 1, "service never retries a failed snapshot");
        f.s.setLevel(777); f.both();
        check(f.s.snapshot[engine].lastSuccessfulWrite == 777, "brightness continues after failed/timed-out read");
        f.sample(); f.both();
        check(f.s.snapshot[engine].sequence == 2 && f.s.snapshot[engine].error == 0 && f.s.snapshot[engine].errors == 1,
              "next low-frequency sample recovers and preserves error count");
    }
}

static void testCancellationAndFade() {
    for (unsigned index = 0; index < 5; ++index) {
        Fixture f; f.sample();
        for (unsigned k = 0; k < index; ++k) {f.finish(0); f.finish(1);}
        const uint8_t inFlight = f.s.engine[0].bus.reg;
        f.s.setLevel(0);
        for (unsigned i = 0; i < 2; ++i) {
            check(f.s.snapshot[i].error == KtzCancelled && f.s.snapshot[i].validMask == 0, "new target invalidates whole sample");
            check(!chipDictionary(f.s, i)->getObject("CFG1"), "cancelled data never shown as CFG1");
            check(f.s.engine[i].bus.reg == inFlight && f.s.engine[i].step == ShengBacklight::StepSnapshot, "only existing read remains in flight");
        }
        f.sample(); check(f.s.snapshot[0].sequence == 1, "cancelled in-flight transfer blocks refresh");
        f.finish(0);
        check(f.s.engine[0].step == ShengBacklight::StepWriteLsb && f.s.engine[0].bus.reg == 4 && !f.s.engine[0].bus.reading,
              "off write follows exactly the existing transfer");
        f.finish(1); f.both();
        check(f.s.snapshot[0].lastSuccessfulWrite == 0 && f.s.snapshot[1].lastSuccessfulWrite == 0, "both engines turn off");
        for (const Hardware &h : f.hw) {
            check(h.trace.size() == index + 3, "cancel starts no remaining diagnostic reads");
            for (unsigned k = 0; k < h.trace.size(); ++k)
                check(k <= index ? h.trace[k].reading : !h.trace[k].reading, "no read/write interleaving in a sample");
        }
    }
    // Intent can arrive on another thread before the associated timer gets the work-loop gate.
    for (unsigned intent = 0; intent < 3; ++intent) {
        Fixture f; f.sample();
        if (intent == 0) gSystemAsleep = 1;
        else if (intent == 1) gLidClosed = 1;
        else gRequest = (1ull << 63) | (uint64_t)900 << 16;
        f.finish(0);
        check(f.s.snapshot[0].error == KtzCancelled && f.s.snapshot[1].error == KtzCancelled, "intent cancels both samples before timer");
        check(!geni_i2c_busy(&f.s.engine[0].bus), "completion cannot chain another read before intent timer");
        if (intent < 2) ShengBacklight::lidFired(&f.s, &f.lid);
        else ShengBacklight::requestFired(&f.s, &f.request);
        f.finish(1); f.both();
        check(f.s.snapshot[0].lastSuccessfulWrite == (intent < 2 ? 0 : 900), "intent applies after existing transfer only");
    }
    Fixture f; f.sample();
    f.s.beginFade(1500, 100);
    check(f.s.fading && f.s.snapshot[0].error == KtzCancelled, "fade invalidates even before first level changes");
    f.both(); f.sample(); check(f.s.snapshot[0].sequence == 1, "fade blocks new sample");
    clockNow += 100 * NSEC_PER_MSEC; ShengBacklight::fadeStep(&f.s, &f.fade); f.both();
    check(!f.s.fading, "fade completion recorded");
    f.sample(); f.both(); check(f.s.snapshot[0].sequence == 2, "sampling resumes after fade settles");
}

static void testUnusableAndStartup() {
    for (unsigned fault = 0; fault < 3; ++fault) {
        Fixture f;
        IOWorkLoop loop; loop.reject = fault == 2;
        f.s.loop = &loop; f.s.diagnostic = nullptr; timerAllocationFails = fault == 1;
        const unsigned before = timerObjects;
        f.s.hostDiagnosticSetup(); // Exact optional-timer fragment extracted from startBacklight.
#if defined(XNU_TARGET_OS_OSX) && XNU_TARGET_OS_OSX
        check((f.s.diagnostic != nullptr) == (fault == 0), "optional timer enabled only when creation and attach succeed");
#else
        check(f.s.diagnostic == nullptr, "iPadOS does not create diagnostic timer");
#endif
        if (f.s.diagnostic) {f.s.diagnostic->release(); f.s.diagnostic = nullptr;}
        check(timerObjects == before, "failed optional timer released");
        timerAllocationFails = false;
        f.s.published = false; f.s.publishFirst();
        check(f.s.published, "backlight remains publishable without optional timer");
    }
    for (unsigned bad = 0; bad < 2; ++bad) {
        Fixture f(bad != 0, bad != 1); f.sample(); f.both();
        check(f.s.snapshot[bad].error == KtzUnusable && f.s.snapshot[bad].sequence == 0, "unusable engine not sampled");
        check(!boolean(chipDictionary(f.s, bad), "Usable") && !chipDictionary(f.s, bad)->getObject("CFG1"), "unusable validity explicit");
        check(f.s.snapshot[1 - bad].validMask == KtzAllValid, "usable peer still sampled");
        f.s.setLevel(888); f.both(); check(f.s.snapshot[1 - bad].lastSuccessfulWrite == 888, "usable peer brightness unaffected");
    }
    for (unsigned fail = 0; fail < 3; ++fail) {
        Fixture f; f.s.published = false;
        for (unsigned i = 0; i < 2; ++i) {f.s.engine[i].started = false; f.s.engine[i].written = -1; f.s.kick(i);}
        check(f.s.engine[0].bus.reg == 4 && f.s.engine[1].bus.reg == 4, "startup still reads brightness first");
        if (fail == 1) f.fail(0, 0);
        else {f.finish(0); if (fail == 2) f.fail(0, 0); else f.finish(0);}
        // The original engine may already be writing the pre-publication target. Finish it normally.
        f.drain(0); f.drain(1);
        check(f.s.published, "startup publishes after both bounded reads or failures");
        check(native_backlight_stats[3] == (fail ? -1 : 1000) && native_backlight_stats[4] == 1000, "startup read stats retain semantics");
        check(f.s.snapshot[0].sequence == 0, "startup never launches configuration read");
    }
}

static void testRequestEvidence() {
    Fixture f;
    auto send = [&](const char *key, uint32_t value) {
        OSDictionary *d = OSDictionary::withCapacity(1);
        OSNumber *n = OSNumber::withNumber(value, 32); d->setObject(key, n); n->release();
        check(f.s.setProperties(d) == kIOReturnSuccess, "setProperties result unchanged"); d->release();
    };
    check(!f.s.properties.getObject("LastRequestedNitsFixed"), "no invented request before actual nits call");
    const uint32_t fixed = 0xffff8000u;
    send("brightness-nits", fixed);
    check((uint32_t)number(&f.s.properties, "LastRequestedNitsFixed") == fixed, "signed16.16 request preserves all 32 bits");
    check(boolean(&f.s.properties, "LastRequestWasNits"), "nits path marked");
    check(native_backlight_request[0] == (int32_t)ShengBacklight::levelForNits((int32_t)fixed), "forwarding curve unchanged");
    send("brightness", 1234);
    check(!boolean(&f.s.properties, "LastRequestWasNits"), "raw path clears flag");
    check((uint32_t)number(&f.s.properties, "LastRequestedNitsFixed") == fixed, "raw path does not fabricate nits");
    check(native_backlight_request[0] == 1234 && ((gRequest >> 16) & 0xffff) == 1234, "raw forwarding unchanged");
    send("LastRequestedNitsFixed", 0);
    check((uint32_t)number(&f.s.properties, "LastRequestedNitsFixed") == fixed, "observer property cannot be set through setProperties");
    check(!boolean(&f.s.properties, "LastRequestWasNits"), "unrelated keys do not change request kind");
}

static void testConcurrentPublication() {
    volatile int32_t rows[2][KtzDiagCount] = {};
    int32_t read[KtzDiagCount] = {};
    check(!ktz_diag_load(rows[0], read), "uninitialized array is unavailable");
    rows[0][KtzDiagEpoch] = 1;
    check(!ktz_diag_load(rows[0], read), "odd writer epoch returns without waiting");
    rows[0][KtzDiagEpoch] = 0;
    std::atomic<bool> done{false}, failed{false};
    std::atomic<unsigned> observed{0}, ready{0};
    auto reader = [&] {
        ready.fetch_add(1);
        do {
            for (unsigned i = 0; i < 2; ++i) {
                int32_t d[KtzDiagCount] = {};
                if (!ktz_diag_load(rows[i], d)) continue;
                observed.fetch_add(1);
                if (d[KtzDiagSequence] != d[KtzDiagTarget] || d[KtzDiagTarget] != d[KtzDiagLastWrite] ||
                    d[KtzDiagCfg1] != (d[KtzDiagSequence] & 255) || d[KtzDiagReadback] != ((d[KtzDiagSequence] & 255) << 3) + (d[KtzDiagSequence] & 7))
                    failed.store(true);
            }
        } while (!done.load());
    };
    std::thread a(reader), b(reader);
    while (ready.load() != 2) std::this_thread::yield();
    for (unsigned n = 1; n <= 30000; ++n) for (unsigned i = 0; i < 2; ++i) {
        KtzSnapshot s; ktz_snapshot_init(&s, 1);
        s.sequence = s.target = n; s.lastSuccessfulWrite = (int32_t)n; s.validMask = KtzAllValid; s.error = 0;
        for (auto &raw : s.raw) raw = (uint8_t)n;
        ktz_diag_store(rows[i], &s, 1);
    }
    done.store(true); a.join(); b.join();
    check(observed.load() > 0 && !failed.load(), "concurrent bounded reads see coherent per-chip rows");
    Fixture f;
    native_backlight_ktz[0][KtzDiagEpoch] = 1;
    char log[1024] = {};
    host_bootlog_ktz(log, sizeof log);
    check(std::strstr(log, "KTZ a unavailable\n"), "BootLog gives up boundedly on publisher in progress");
    check(std::strstr(log, "cfg=-1/-1 cur=-1 br=-1"), "BootLog makes invalid raw values explicit");
}

int main() {
    check(kMidNits == 140 && kMinLevel == 892 && kAttachMs == 250 && kAttachTries == 240,
          "preserved calibration and interrupt attachment constants");
    testEligibilityAndSuccess();
    testFailedWritesAreNotSuccess();
    testReadFailures();
    testCancellationAndFade();
    testUnusableAndStartup();
    testRequestEvidence();
    testConcurrentPublication();
    std::printf("PASS: %u backlight diagnostic checks; production scheduling, failure/timeout recovery, cancellation, validity, request evidence, concurrent publication\n", checks);
}
