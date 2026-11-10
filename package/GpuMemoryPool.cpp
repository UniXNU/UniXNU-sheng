// The GPU's memory pool outside the kernel's window. DarwinBoot lists the largest runs of free memory that lie outside the window in
// /chosen/darwinboot-memory-outside ((base, size) pairs of 64-bit values, unused pairs zero): on sheng the free part of the low DRAM
// bank, which is too far from the window for the kernel's page array. Each run is handed to the GPU driver, which serves buffer
// objects from it before it takes pageable memory of the window (Adreno740 NativeAdrenoPool). The tree has the table only for an
// OS profile that asks for it (payload.adt owned_memory_outside_ranges); `gpu-pool=0` makes the driver ignore the runs.
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <string.h>

extern "C" IOReturn native_adreno_pool_add(uint64_t physical, uint64_t bytes) __attribute__((weak));   // Adreno740 NativeAdrenoPool.cpp
// For the boot log: [0] runs in the table, [1] runs the driver took, [2] KiB the driver took, [3] the last refusal.
extern "C" {volatile uint32_t native_gpu_pool_board[4];}

extern "C" void sheng_gpu_pool_probe(void)
{
    if (!native_adreno_pool_add) return;
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) return;
    OSData *table = OSDynamicCast(OSData, chosen->getProperty("darwinboot-memory-outside"));
    if (table && table->getLength() >= 16) {
        const uint8_t *bytes = static_cast<const uint8_t *>(table->getBytesNoCopy());
        for (unsigned i = 0; i + 16 <= table->getLength(); i += 16) {
            uint64_t base = 0, size = 0;
            memcpy(&base, bytes + i, 8);
            memcpy(&size, bytes + i + 8, 8);
            if (!size) continue;
            native_gpu_pool_board[0] = native_gpu_pool_board[0] + 1;
            const IOReturn status = native_adreno_pool_add(base, size);
            if (status == kIOReturnSuccess) {
                native_gpu_pool_board[1] = native_gpu_pool_board[1] + 1;
                native_gpu_pool_board[2] = native_gpu_pool_board[2] + (uint32_t)(size >> 10);
            } else {
                native_gpu_pool_board[3] = (uint32_t)status;
            }
            IOLog("sheng gpu pool: 0x%llx + 0x%llx: %x\n", (unsigned long long)base, (unsigned long long)size, status);
        }
    }
    chosen->release();
}
