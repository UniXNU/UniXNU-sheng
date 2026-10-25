// SPDX-License-Identifier: BSD-2-Clause-Patent
// Starts the battery power source (ShengBattery.cpp) whenever the audio DSP runs (sheng-adsp=2: the values come from the DSP's battery
// manager); sheng-battery=0 leaves it off, sheng-battery=1 asks for it without the DSP argument. It registers that file's classes with the
// kernel's module (its static initializer, which the build takes out of __mod_init_func), then creates the service, which is published
// once the first refresh is complete.
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_battery_static_init(void) __asm__("__GLOBAL__sub_I_ShengBattery.cpp");
extern "C" void sheng_battery_start(void);
// See ShengBattery.cpp for the meaning of each entry.
extern "C" {volatile int32_t native_battery_stats[16];}

extern "C" void sheng_battery_probe(void) {
    unsigned enabled = 0, adsp = 0;
    if (!PE_parse_boot_argn("sheng-battery", &enabled, sizeof enabled)) enabled = PE_parse_boot_argn("sheng-adsp", &adsp, sizeof adsp) && adsp == 2;
    if (enabled != 1 || native_battery_stats[0]) return;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {native_battery_stats[0] = -10; return;}
    native_battery_static_init();
    if (OSMetaClass::postModLoad(handle)) {native_battery_stats[0] = -11; return;}
    sheng_battery_start();
}
