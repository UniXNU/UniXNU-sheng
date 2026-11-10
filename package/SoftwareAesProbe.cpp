// SPDX-License-Identifier: BSD-2-Clause-Patent
// macOS: registers the software AES-CBC provider (UniXNU drivers/SoftwareAES, NativeAES, an IOAESAccelerator of the original
// IOCryptoAcceleratorFamily). AppleImage4 starts on the device tree's "defaults" node and, in kmod_expert_finalize, waits for an
// IOAESAccelerator service without a limit while it holds its expert lock (26C5035r and 24A446 alike). This board has no AES engine node
// (an Apple SoC's AppleS8000AES matches "aes,s8000"), so the wait never ended: "defaults" never finished matching, IOKit never went quiet,
// and the network configuration, the TimeSync clock manager and through it WindowServer waited on that (macOS run29/run30). NativeAES
// registers itself after its known-answer test passes; it serves software keys only and refuses hardware key selectors.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <kern/thread.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_aes_static_init(void) __asm__("__GLOBAL__sub_I_NativeAES.cpp");
// 0 not run, 1 started (NativeAES registered itself), negative: the step that failed.
extern "C" volatile int sheng_software_aes_state = 0;

namespace {

void fail(int step)
{
    sheng_software_aes_state = -step;
    IOLog("SoftwareAES: not started (%d)\n", -step);
    thread_terminate(current_thread());
}

void worker(void *, wait_result_t)
{
    // The family has no personality on this board; loading it makes the parent class before NativeAES registers.
    const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.iokit.IOCryptoAcceleratorFamily");
    if (loaded != kOSReturnSuccess && loaded != kOSKextReturnStartStopError) fail(1);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) fail(2);
    native_aes_static_init();
    if (OSMetaClass::postModLoad(handle)) fail(3);
    OSObject *object = OSMetaClass::allocClassWithName("NativeAES");
    IOService *aes = OSDynamicCast(IOService, object);
    IOService *parent = IOService::getResourceService();
    if (!aes || !parent || !aes->init()) {OSSafeReleaseNULL(object); fail(4);}
    if (!aes->attach(parent)) {aes->release(); fail(5);}
    // NativeAES::start runs the NIST known-answer tests and registers the service only when they pass.
    if (!aes->start(parent)) {aes->detach(parent); aes->release(); fail(6);}
    aes->release();
    sheng_software_aes_state = 1;
    IOLog("SoftwareAES: IOAESAccelerator registered\n");
    thread_terminate(current_thread());
}

}  // namespace

// The board entry (LegacyDisplayProbe.cpp's late probes), macOS only.
extern "C" void sheng_software_aes_probe(void)
{
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else sheng_software_aes_state = -7;
}
