// SPDX-License-Identifier: BSD-2-Clause-Patent
// The battery as iOS reads it (boot argument sheng-battery=1, started by BatteryProbe.cpp; needs sheng-adsp=2): an IOPMPowerSource whose
// values come from the battery manager in the audio DSP over the PMIC GLINK channel (ShengBattmgr.c). powerd finds every IOPMPowerSource,
// reads CurrentCapacity/MaxCapacity, IsCharging, ExternalConnected, voltage, amperage and AdapterDetails and listens for the status change
// message updateStatus sends; the status bar, the lock screen's charging state and Settings > Battery follow from that.
//
// Design: everything runs on the service's work loop. The DSP thread's callbacks only copy a message into a small ring and signal an
// IOInterruptEventSource (no provider: a software event), so the DSP thread never waits for this driver. One request is in flight at a
// time; a 2 s timer gives up on an unanswered one. Battery notifications start a refresh at once, and a 60 s timer refreshes in any case
// (the fields that hardly change, every tenth time). Charging itself is the DSP firmware's; the one setting written is the input current
// limit on a computer's USB port (ShengBattmgr.c; boot argument sheng-usb-sdp-ma, 0 leaves the firmware's limit).
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "ShengAdsp.h"
#include "ShengBattmgr.h"
}

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *) __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));
// [0] stage (1 class up, 2 channel registered, 3 channel up, 4 published; negative: the step that failed), [1] percent, [2] mV,
// [3] mA, [4] temperature (raw), [5] flags (bit 0 present, 1 external, 2 charging, 3 full, 4 notifications enabled), [6] refresh
// cycles, [7] answers, [8] timeouts | errors << 16, [9] notifications, [10] USB type (raw), [11] full mAh, [12] design mAh,
// [13] cycles, [14] messages dropped (ring full) | ignored << 16, [15] send refusals
extern "C" {extern volatile int32_t native_battery_stats[16];}
// The raw readings behind the values: [0] capacity property (hundredths of a percent), [1] gauge 1 remaining, [2] gauge 1 full, [3] gauge 2
// remaining, [4] gauge 2 full, [5] temperature property, [6] current property (uA, discharge positive), [7] valid field bits, [8] the field
// the last timeout skipped (-1 none)
extern "C" {volatile int32_t native_battery_raw[10];}
// The charger as the firmware reports it (-1 no reading): [0] USB online, [1] USB type, [2] adapter type, [3] the board's adapter type,
// [4] input current limit mA, [5] the most the port offers mA, [6] USB mV, [7] USB mA, [8] battery status, [9] charge type, [10] thermal
// level, [11] charge current mA, [12] adapter watts, [13] bits: 0 cell 1 authenticated, 1 cell 2, 2 PD adapter authenticated, 3 input
// suspended (each with bit +8 set when read), [14] limit sets | tries << 8 | port type << 16, [15] last limit result
extern "C" {volatile int32_t native_charger_stats[16];}

namespace {
constexpr uint32_t kRequestMs = 2000, kRefreshMs = 60000, kSlowEvery = 10, kRing = 16, kMessage = 64;
}

class ShengBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(ShengBattery)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *request = nullptr, *refresh = nullptr;
    IOInterruptEventSource *inbox = nullptr;
    IOSimpleLock *ringLock = nullptr;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    uint32_t ringHead = 0, ringCount = 0;
    volatile uint32_t channelUp = 0, channelChanged = 0;
    bool wasUp = false, published = false;
    uint32_t refreshes = 0;
    ShengBattmgr bm;
    uint32_t sdpLimit = 0;
    ShengAdspChannel *channel = nullptr;

    static void dspState(void *context, int up) {
        ShengBattery *self = static_cast<ShengBattery *>(context);
        __atomic_store_n(&self->channelUp, up ? 1u : 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&self->channelChanged, 1u, __ATOMIC_RELEASE);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
    }
    static void dspReceive(void *context, const uint8_t *data, uint32_t length) {
        ShengBattery *self = static_cast<ShengBattery *>(context);
        IOSimpleLockLock(self->ringLock);
        if (self->ringCount < kRing && length <= kMessage) {
            auto &slot = self->ring[(self->ringHead + self->ringCount) % kRing];
            memcpy(slot.data, data, length);
            slot.length = length;
            self->ringCount++;
        } else {
            native_battery_stats[14] = native_battery_stats[14] + 1;
        }
        IOSimpleLockUnlock(self->ringLock);
        self->inbox->interruptOccurred(nullptr, nullptr, 0);
    }

    void pump() {
        uint8_t out[SHENG_BATTMGR_REQUEST_BYTES];
        const uint32_t n = sheng_battmgr_next(&bm, out);
        if (!n) return;
        if (sheng_adsp_send(channel, out, n)) native_battery_stats[15] = native_battery_stats[15] + 1;  // the timer skips the field
        request->setTimeoutMS(kRequestMs);
    }
    void number(const char *key, int64_t value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        OSNumber *n = OSNumber::withNumber((unsigned long long)value, 32);
        if (symbol && n) setPSProperty(symbol, n);
        OSSafeReleaseNULL(n); OSSafeReleaseNULL(symbol);
    }
    void flag(const char *key, bool value) {
        const OSSymbol *symbol = OSSymbol::withCString(key);
        if (symbol) setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(symbol);
    }
    void recordRaw() {
        static const int fields[8] = {BmCapacity, BmGauge1Remaining, BmGauge1Full, BmGauge2Remaining, BmGauge2Full, BmTemperature, BmCurrent, -1};
        for (unsigned i = 0; i < 7; ++i) native_battery_raw[i] = (bm.valid >> fields[i]) & 1 ? bm.value[fields[i]] : -999999;
        native_battery_raw[7] = (int32_t)bm.valid;
        native_battery_raw[8] = bm.last_timeout;
        static const int charger[13] = {BmUsbOnline, BmUsbType, BmUsbAdapter, BmRealType, BmUsbLimit, BmUsbMax, BmUsbVoltage, BmUsbCurrent,
                                        BmStatus, BmChargeType, BmThermalLevel, BmChargeCurrent, BmPowerMax};
        static const int milli[13] = {0, 0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0};
        for (unsigned i = 0; i < 13; ++i)
            native_charger_stats[i] = (bm.valid >> charger[i]) & 1 ? (milli[i] ? bm.value[charger[i]] / 1000 : bm.value[charger[i]]) : -1;
        static const int bits[4] = {BmAuthentic, BmSlaveAuthentic, BmPdVerified, BmInputSuspend};
        int32_t flags = 0;
        for (unsigned i = 0; i < 4; ++i)
            if ((bm.valid >> bits[i]) & 1) flags |= (bm.value[bits[i]] != 0) << i | 1 << (i + 8);
        native_charger_stats[13] = flags;
        native_charger_stats[14] = (int32_t)(bm.limit_sets | bm.limit_tries << 8 | (uint32_t)(bm.limit_port & 0xff) << 16);
        native_charger_stats[15] = bm.limit_result;
    }
    void publish() {
        ShengBatterySnapshot s;
        sheng_battmgr_snapshot(&bm, &s);
        setBatteryInstalled(s.present);
        setExternalConnected(s.external);
        setExternalChargeCapable(s.external);
        setIsCharging(s.charging);
        setAtCriticalLevel(s.critical);
        setCurrentCapacity((unsigned)s.percent);
        setMaxCapacity(100);
        setVoltage((unsigned)(s.voltage_mv > 0 ? s.voltage_mv : 0));
        setAmperage(s.current_ma);
        if (s.cycles > 0) setCycleCount((unsigned)s.cycles);
        setTimeRemaining(s.charging ? (s.time_to_full_min >= 0 ? s.time_to_full_min : 0xffff)
                                    : (s.time_to_empty_min >= 0 ? s.time_to_empty_min : 0xffff));
        flag("FullyCharged", s.fully_charged);
        number("InstantAmperage", s.current_ma);
        if (s.temperature_valid) number("Temperature", s.temperature);
        if (s.raw_mah > 0) number("AppleRawCurrentCapacity", s.raw_mah);
        if (s.full_mah > 0) {number("AppleRawMaxCapacity", s.full_mah); number("NominalChargeCapacity", s.full_mah);}
        if (s.design_mah > 0) number("DesignCapacity", s.design_mah);
        OSDictionary *adapter = OSDictionary::withCapacity(5);
        if (adapter) {
            OSNumber *family = OSNumber::withNumber(s.adapter_family, 32);
            if (family) {adapter->setObject(kIOPMPSAdapterDetailsFamilyKey, family); family->release();}
            if (s.external) {
                OSNumber *w = OSNumber::withNumber((unsigned long long)s.adapter_watts, 32), *mv = OSNumber::withNumber((unsigned long long)s.adapter_mv, 32),
                         *ma = OSNumber::withNumber((unsigned long long)s.adapter_ma, 32);
                if (w) {adapter->setObject(kIOPMPSAdapterDetailsWattsKey, w); w->release();}
                if (mv) {adapter->setObject(kIOPMPSAdapterDetailsVoltage, mv); mv->release();}
                if (ma) {adapter->setObject(kIOPMPSAdapterDetailsAmperageKey, ma); ma->release();}
            }
            const OSSymbol *key = OSSymbol::withCString(kIOPMPSAdapterDetailsKey);
            if (key) setPSProperty(key, adapter);
            OSSafeReleaseNULL(key);
            adapter->release();
        }
        native_battery_stats[1] = s.percent; native_battery_stats[2] = s.voltage_mv; native_battery_stats[3] = s.current_ma;
        native_battery_stats[4] = s.temperature;
        native_battery_stats[5] = s.present | s.external << 1 | s.charging << 2 | s.fully_charged << 3 | bm.enabled << 4;
        native_battery_stats[10] = (bm.valid >> BmUsbType) & 1 ? bm.value[BmUsbType] : -1;
        native_battery_stats[11] = s.full_mah; native_battery_stats[12] = s.design_mah; native_battery_stats[13] = s.cycles;
        recordRaw();
        if (!published && !s.charge_known) return;  // no charge reading yet: 0 % would read as an empty battery
        if (!published) {
            // The properties are in place before anyone finds the service.
            published = true;
            registerService();
            native_battery_stats[0] = 4;
        }
        updateStatus();
    }
    void counters() {
        native_battery_stats[6] = (int32_t)bm.cycles_done;
        native_battery_stats[7] = (int32_t)bm.responses;
        native_battery_stats[8] = (int32_t)(bm.timeouts | bm.errors << 16);
        native_battery_stats[9] = (int32_t)bm.notifications;
        native_battery_stats[14] = (native_battery_stats[14] & 0xffff) | (int32_t)(bm.ignored << 16);
    }
    void handle(int result) {
        if (result == BmCycleDone) publish();
        pump();
        counters();
    }

    static void inboxAction(OSObject *owner, IOInterruptEventSource *, int) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self) return;
        if (__atomic_exchange_n(&self->channelChanged, 0u, __ATOMIC_ACQ_REL)) {
            const bool up = __atomic_load_n(&self->channelUp, __ATOMIC_ACQUIRE) != 0;
            if (up && !self->wasUp) {
                native_battery_stats[0] = self->published ? 4 : 3;
                uint8_t enable[SHENG_BATTMGR_ENABLE_BYTES];
                sheng_adsp_send(self->channel, enable, sheng_battmgr_enable_request(enable));
                sheng_battmgr_refresh(&self->bm, 1);
                self->pump();
                self->refresh->setTimeoutMS(kRefreshMs);
            }
            if (!up && self->wasUp) {self->request->cancelTimeout(); sheng_battmgr_init(&self->bm); self->bm.sdp_limit_ua = self->sdpLimit;}
            self->wasUp = up;
        }
        for (;;) {
            uint8_t data[kMessage];
            uint32_t length = 0;
            IOSimpleLockLock(self->ringLock);
            if (self->ringCount) {
                auto &slot = self->ring[self->ringHead];
                length = slot.length;
                memcpy(data, slot.data, length);
                self->ringHead = (self->ringHead + 1) % kRing;
                self->ringCount--;
            }
            IOSimpleLockUnlock(self->ringLock);
            if (!length) break;
            const int result = sheng_battmgr_receive(&self->bm, data, length);
            if (result == BmAnswer || result == BmCycleDone) self->request->cancelTimeout();
            self->handle(result);
        }
    }
    static void requestAction(OSObject *owner, IOTimerEventSource *) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self || !self->wasUp) return;
        const uint32_t before = self->bm.cycles_done;
        sheng_battmgr_timeout(&self->bm);
        self->handle(self->bm.cycles_done != before ? BmCycleDone : BmAnswer);
    }
    static void refreshAction(OSObject *owner, IOTimerEventSource *sender) {
        ShengBattery *self = OSDynamicCast(ShengBattery, owner);
        if (!self || !self->wasUp) return;
        sheng_battmgr_refresh(&self->bm, ++self->refreshes % kSlowEvery == 0);
        self->handle(BmAnswer);
        sender->setTimeoutMS(kRefreshMs);
    }

public:
    bool startBattery() {
        sheng_battmgr_init(&bm);
        uint32_t ma = 0;
        if (PE_parse_boot_argn("sheng-usb-sdp-ma", &ma, sizeof ma)) bm.sdp_limit_ua = ma > 3000 ? 3000000u : ma * 1000u;
        sdpLimit = bm.sdp_limit_ua;
        loop = IOWorkLoop::workLoop();
        ringLock = IOSimpleLockAlloc();
        inbox = IOInterruptEventSource::interruptEventSource(this, &ShengBattery::inboxAction);
        request = IOTimerEventSource::timerEventSource(this, &ShengBattery::requestAction);
        refresh = IOTimerEventSource::timerEventSource(this, &ShengBattery::refreshAction);
        if (!loop || !ringLock || !inbox || !request || !refresh || loop->addEventSource(inbox) != kIOReturnSuccess ||
            loop->addEventSource(request) != kIOReturnSuccess || loop->addEventSource(refresh) != kIOReturnSuccess)
            return false;
        native_battery_stats[0] = 1;
        const ShengAdspClient client = {&ShengBattery::dspState, &ShengBattery::dspReceive};
        channel = sheng_adsp_channel ? sheng_adsp_channel("PMIC_RTR_ADSP_APPS", 1024, 4, 0, &client, this) : nullptr;
        if (!channel) return false;
        native_battery_stats[0] = 2;
        return true;
    }
};
OSDefineMetaClassAndStructors(ShengBattery, IOPMPowerSource)

// Called by BatteryProbe.cpp once this file's classes are registered.
extern "C" void sheng_battery_start(void) {
    ShengBattery *battery = OSTypeAlloc(ShengBattery);
    if (!battery || !battery->init() || !battery->attach(IOService::getPlatform())) {
        if (battery) battery->release();
        native_battery_stats[0] = -1;
        return;
    }
    if (!battery->startBattery()) {
        native_battery_stats[0] = native_battery_stats[0] > 0 ? -native_battery_stats[0] - 1 : -1;
        battery->detach(IOService::getPlatform());
        battery->release();
        return;
    }
    battery->release();  // the platform's attachment keeps it
}
