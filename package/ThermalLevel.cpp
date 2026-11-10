// SPDX-License-Identifier: BSD-2-Clause-Patent
// The board's temperatures as macOS's thermal pressure level, off by default. `thermal-level=1` publishes the service
// thermalmonitord takes its level from, with the one key that carries it. It needs the temperature probe: `thermal=1`
// (ThermalProbe.cpp) must be set as well, otherwise nothing is published and the row says st=-1.
//
// macOS has no thermal input on this board without it: the level is never written and stays nominal while the firmware lowers the
// CPU clusters. thermalmonitord does not compute a level; it copies the value of the SMC key mTPL into the notification
// com.apple.system.thermalpressurelevel. It does not read that key from AppleSMC but from a second service of the same kext,
// AppleSMCSensorDispatcher, which it polls once a second (ThermalLevel.h has the protocol, the addresses it was read from, the
// mapping and why). This file is that service: an IOService and a user client with Apple's class names, because the daemon matches
// the service by class and its sandbox entitlement names the user client class. The original kext is not part of the image
// (the profile's roots leave com.apple.driver.AppleSMC out), and the classes here register only with the boot argument.
//
// Only thermalmonitord uses this service: the name is in no other binary of the system volume or the shared cache, and on a Mac the
// user client's only creator is thermalmonitord. AppleSMC itself, which powerd and many others open, is not published: thermalmonitord
// keeps logging "Couldn't connect to smc!" for its sensor exchange and goes on.
//
// What a client gets: selector 0 the number of keys (1), selector 1 their codes (mTPL), selector 2 their values (the level, computed
// at that moment, at most twice a second), selector 3 accepts a write request and does nothing with it. Any other key is simply not in
// the list, which the daemon reads as "no such key" (-1). Selectors 4 to 6 of the original (three more SMC keys) and anything above
// are unsupported. A caller must hold the entitlement the original requires, com.apple.private.smcsensor.user-access.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <mach/kmod.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "ThermalLevel.h"

extern kmod_info_t g_kernel_kmod_info;
extern "C" {
// ThermalProbe.cpp: [0] stage (2: sampled), [1] samples; the hottest sensor of each group at the last sample.
extern volatile uint32_t native_thermal_info[9] __attribute__((weak_import));
extern volatile int32_t native_thermal_now[THERMAL_GROUPS] __attribute__((weak_import));
void sheng_thermal_sample(void) __attribute__((weak_import));
int proc_selfpid(void);
// This file's classes: the build takes the initializer out of __mod_init_func, the probe runs it.
void sheng_thermal_level_static_init(void) __asm__("__GLOBAL__sub_I_ThermalLevel.cpp");
// 0 off, otherwise the row's st (ThermalLevel.h thermal_level_row).
volatile int32_t native_thermal_level_state;
void sheng_thermal_level_probe(void);
unsigned sheng_thermal_level_row(char *row, unsigned size);
}

namespace {
constexpr uint64_t kMinIntervalNs = 500 * 1000 * 1000ull;   // the daemon asks once a second; nobody makes the sensors be read faster
IOLock *gLock;
thermal_level_state gLevel;
thermal_level_use gUse;
uint64_t gLastAbs, gMinIntervalAbs;

// The level now. Called with gLock held.
unsigned currentLevel() {
    const uint64_t now = mach_absolute_time();
    if (gLevel.rounds && now - gLastAbs < gMinIntervalAbs) return gLevel.level;
    gLastAbs = now;
    int32_t groups[THERMAL_GROUPS];
    for (unsigned g = 0; g < THERMAL_GROUPS; ++g) groups[g] = THERMAL_NONE;
    bool sampled = false;
    uint32_t samples = 0;
    if (sheng_thermal_sample && &native_thermal_info[0] && &native_thermal_now[0]) {
        sheng_thermal_sample();
        sampled = native_thermal_info[0] == 2;
        samples = native_thermal_info[1];
        if (sampled) for (unsigned g = 0; g < THERMAL_GROUPS; ++g) groups[g] = native_thermal_now[g];
    }
    return thermal_level_step(&gLevel, groups, sampled, samples);
}
}  // namespace

class AppleSMCSensorDispatcher : public IOService {
    OSDeclareDefaultStructors(AppleSMCSensorDispatcher)
public:
    bool publish() {
        setProperty("IOUserClientClass", "AppleSMCSensorDispatcherUserClient");
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(AppleSMCSensorDispatcher, IOService)

class AppleSMCSensorDispatcherUserClient : public IOUserClient {
    OSDeclareDefaultStructors(AppleSMCSensorDispatcherUserClient)
    int32_t pid = 0;

    static IOReturn sCount(OSObject *, void *, IOExternalMethodArguments *args) {
        args->scalarOutput[0] = THERMAL_LEVEL_KEYS;
        IOLockLock(gLock);
        gUse.counts++;
        IOLockUnlock(gLock);
        return kIOReturnSuccess;
    }
    static IOReturn sKeys(OSObject *, void *, IOExternalMethodArguments *args) {
        if (!args->structureOutput) return kIOReturnBadArgument;
        uint32_t keys[THERMAL_LEVEL_SLOTS];
        thermal_level_fill_keys(keys);
        memcpy(args->structureOutput, keys, sizeof keys);
        IOLockLock(gLock);
        gUse.lists++;
        IOLockUnlock(gLock);
        return kIOReturnSuccess;
    }
    static IOReturn sValues(OSObject *target, void *, IOExternalMethodArguments *args) {
        if (!args->structureOutput) return kIOReturnBadArgument;
        uint32_t values[THERMAL_LEVEL_SLOTS];
        IOLockLock(gLock);
        thermal_level_fill_values(values, currentLevel());
        thermal_level_use_read(&gUse, static_cast<AppleSMCSensorDispatcherUserClient *>(target)->pid);
        IOLockUnlock(gLock);
        memcpy(args->structureOutput, values, sizeof values);
        return kIOReturnSuccess;
    }
    // The original writes the request's keys to the SMC. There is none: the request is checked, counted and dropped.
    static IOReturn sWrite(OSObject *, void *, IOExternalMethodArguments *args) {
        if (!args->structureInput) return kIOReturnBadArgument;
        IOLockLock(gLock);
        const int good = thermal_level_use_write(&gUse, static_cast<const uint8_t *>(args->structureInput));
        IOLockUnlock(gLock);
        return good ? kIOReturnSuccess : kIOReturnBadArgument;
    }

public:
    // The original's check (AppleSMCSensorDispatcherUserClient::initWithTask): the entitlement must be the boolean true.
    bool initWithTask(task_t task, void *security, UInt32 type) override {
        OSObject *entitlement = copyClientEntitlement(task, THERMAL_LEVEL_ENTITLEMENT);
        const bool entitled = entitlement == kOSBooleanTrue;
        OSSafeReleaseNULL(entitlement);
        if (!entitled) {
            IOLockLock(gLock);
            gUse.refused++;
            IOLockUnlock(gLock);
            return false;
        }
        if (!IOUserClient::initWithTask(task, security, type)) return false;
        pid = proc_selfpid();
        return true;
    }
    bool start(IOService *provider) override {
        if (!OSDynamicCast(AppleSMCSensorDispatcher, provider) || !IOUserClient::start(provider)) return false;
        setProperty(kIOUserClientDefaultLockingKey, kOSBooleanTrue);   // as the original: one external method at a time
        IOLockLock(gLock);
        gUse.opens++;
        IOLockUnlock(gLock);
        return true;
    }
    IOReturn clientClose() override {
        IOLockLock(gLock);
        gUse.closes++;
        IOLockUnlock(gLock);
        terminate();
        return kIOReturnSuccess;
    }
    // The original's table for selectors 0 to 3: the exact scalar counts and structure sizes, checked by IOUserClient::externalMethod.
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override {
        static const IOExternalMethodDispatch methods[THERMAL_LEVEL_SELECTORS] = {
            {&AppleSMCSensorDispatcherUserClient::sCount, 0, 0, 1, 0},
            {&AppleSMCSensorDispatcherUserClient::sKeys, 0, 0, 0, THERMAL_LEVEL_ARRAY_BYTES},
            {&AppleSMCSensorDispatcherUserClient::sValues, 0, 0, 0, THERMAL_LEVEL_ARRAY_BYTES},
            {&AppleSMCSensorDispatcherUserClient::sWrite, 0, THERMAL_LEVEL_WRITE_BYTES, 0, 0},
        };
        if (selector >= THERMAL_LEVEL_SELECTORS) {
            IOLockLock(gLock);
            gUse.unsupported++;
            gUse.last_unsupported = selector;
            IOLockUnlock(gLock);
            return kIOReturnUnsupported;
        }
        return IOUserClient::externalMethod(selector, args, const_cast<IOExternalMethodDispatch *>(&methods[selector]), this, nullptr);
    }
};
OSDefineMetaClassAndStructors(AppleSMCSensorDispatcherUserClient, IOUserClient)

extern "C" void sheng_thermal_level_probe(void) {
    unsigned mode = 0;
    PE_parse_boot_argn("thermal-level", &mode, sizeof mode);
    if (mode != 1 || native_thermal_level_state) return;
    thermal_level_init(&gLevel);
    // Without readings there is nothing to say: the level stays unwritten, as without this file.
    if (!sheng_thermal_sample || !&native_thermal_info[0] || !&native_thermal_now[0] || (native_thermal_info[0] != 1 && native_thermal_info[0] != 2)) {
        native_thermal_level_state = -1;
        return;
    }
    nanoseconds_to_absolutetime(kMinIntervalNs, &gMinIntervalAbs);
    gLock = IOLockAlloc();
    if (!gLock) {native_thermal_level_state = -2; return;}
    // The class names are Apple's. They must be free: the original kext is not in the image, and nothing ran the initializer before.
    static const char *const kClassNames[2] = {"AppleSMCSensorDispatcher", "AppleSMCSensorDispatcherUserClient"};
    for (const char *name : kClassNames) {
        const OSSymbol *symbol = OSSymbol::withCString(name);
        const bool taken = symbol && OSMetaClass::getMetaClassWithName(symbol);
        OSSafeReleaseNULL(symbol);
        if (taken) {native_thermal_level_state = -12; return;}
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {native_thermal_level_state = -10; return;}
    sheng_thermal_level_static_init();
    if (OSMetaClass::postModLoad(handle)) {native_thermal_level_state = -11; return;}
    IOLockLock(gLock);
    currentLevel();
    IOLockUnlock(gLock);
    AppleSMCSensorDispatcher *service = OSTypeAlloc(AppleSMCSensorDispatcher);
    if (!service || !service->init()) {
        OSSafeReleaseNULL(service);
        native_thermal_level_state = -2;
        return;
    }
    if (!service->attach(IOService::getPlatform())) {
        service->release();
        native_thermal_level_state = -3;
        return;
    }
    if (!service->start(IOService::getPlatform()) || !service->publish()) {
        service->detach(IOService::getPlatform());
        service->release();
        native_thermal_level_state = -3;
        return;
    }
    native_thermal_level_state = 1;
    service->release();   // the platform's attachment keeps it
}

// The boot log's row (ThermalLevel.h thermal_level_row). Returns its length, 0 while the feature is off. The row is read without
// the lock: a number may be one request behind.
extern "C" unsigned sheng_thermal_level_row(char *row, unsigned size) {
    if (!native_thermal_level_state || !row || size < 2) return 0;
    return thermal_level_row(row, size, native_thermal_level_state, &gLevel, &gUse);
}
