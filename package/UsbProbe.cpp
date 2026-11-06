// SPDX-License-Identifier: BSD-2-Clause-Patent
// sheng board entry of the QcomDwc3 driver: the controller's classes register here, after the hardware answered, the way a module
// load would register them (see HidInputProbe.cpp for the same deferred metaclass registration).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" bool qcom_dwc3_bring_up(void);
extern "C" void qcom_dwc3_publish(void);
extern "C" void qcom_dwc3_diag(bool);
extern "C" void qcom_dwc3_static_init(void) __asm__("__GLOBAL__sub_I_QcomDwc3.cpp");
extern "C" volatile uint32_t native_usb_stats[54];
extern "C" void qcom_dwc3_host_load(int);
static void worker(void *, wait_result_t) {
    IOSleep(3000);
    if (qcom_dwc3_bring_up()) {
        // AppleUSBXDCI has no personality of its own, so nothing has loaded it: its constructors and metaclasses, and those of
        // IOUSBDeviceFamily below it, run on this load.
        const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBXDCI");
        native_usb_stats[21] = (uint32_t)loaded;
        if (loaded != kOSReturnSuccess && loaded != kOSKextReturnStartStopError) {native_usb_stats[0] = (uint32_t)-9; thread_terminate(current_thread());}
        // The same for host mode: Apple's xHCI driver and the USB host family below it.
        qcom_dwc3_host_load((int)OSKextLoadKextWithIdentifier("com.apple.driver.usb.AppleUSBXHCI"));
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (!handle) native_usb_stats[0] = (uint32_t)-10;
        else {
            qcom_dwc3_static_init();
            if (OSMetaClass::postModLoad(handle)) native_usb_stats[0] = (uint32_t)-11;
            else {
                qcom_dwc3_publish();
                unsigned mode = 0;
                PE_parse_boot_argn("sheng-usb", &mode, sizeof mode);
                if (mode == 2 || mode == 3) qcom_dwc3_diag(mode == 3);
            }
        }
    }
    thread_terminate(current_thread());
}
extern "C" void sheng_usb_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("sheng-usb", &enabled, sizeof enabled) || !enabled) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
