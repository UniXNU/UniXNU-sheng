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

// The ADT supplies the same firmware ownership ranges to this pool and XNU VM adoption.
static struct {uint64_t base, end;} reservedRanges[64];
static unsigned reservedCount;
// KiB of the runs left out as stock reserved memory, for the boot log.
extern "C" {volatile uint32_t native_gpu_pool_reserved_kib;}

static void addRun(uint64_t base, uint64_t end, IOReturn *last)
{
    for (unsigned i = 0; i < reservedCount; ++i) {
        const auto &r = reservedRanges[i];
        if (end <= r.base || base >= r.end) continue;
        native_gpu_pool_reserved_kib = native_gpu_pool_reserved_kib + (uint32_t)(((end < r.end ? end : r.end) - (base > r.base ? base : r.base)) >> 10);
        if (base < r.base) addRun(base, r.base, last);
        if (end > r.end) addRun(r.end, end, last);
        return;
    }
    if (end - base < (1ull << 20)) return;
    native_gpu_pool_board[0] = native_gpu_pool_board[0] + 1;
    const IOReturn status = native_adreno_pool_add(base, end - base);
    if (status == kIOReturnSuccess) {
        native_gpu_pool_board[1] = native_gpu_pool_board[1] + 1;
        native_gpu_pool_board[2] = native_gpu_pool_board[2] + (uint32_t)((end - base) >> 10);
    } else {
        native_gpu_pool_board[3] = (uint32_t)status;
        *last = status;
    }
    IOLog("sheng gpu pool: 0x%llx + 0x%llx: %x\n", (unsigned long long)base, (unsigned long long)(end - base), status);
}

// A kernel that adopts the memory outside its window as VM memory (window-outside-memory-adoption.patch) owns these runs: the
// driver gets none of them and takes its buffer objects from the VM as everything else does.
extern "C" {extern uint32_t ml_outside_memory_pages __attribute__((weak_import));}

extern "C" void sheng_gpu_pool_probe(void)
{
    if (!native_adreno_pool_add) return;
    if (&ml_outside_memory_pages && ml_outside_memory_pages) return;
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) return;
    OSData *kept = OSDynamicCast(OSData, chosen->getProperty("darwinboot-memory-outside-kept"));
    if (!kept || !kept->getLength() || kept->getLength() % 16 || kept->getLength() / 16 > 64) {
        native_gpu_pool_board[3] = kIOReturnBadArgument;
        IOLog("sheng gpu pool: require a firmware reservation table\n");
        chosen->release();
        return;
    }
    reservedCount = kept->getLength() / 16;
    const uint8_t *reservations = static_cast<const uint8_t *>(kept->getBytesNoCopy());
    for (unsigned i = 0; i < reservedCount; ++i) {
        uint64_t size;
        memcpy(&reservedRanges[i].base, reservations + i * 16, 8);
        memcpy(&size, reservations + i * 16 + 8, 8);
        reservedRanges[i].end = reservedRanges[i].base + size;
        if (!size || reservedRanges[i].end < reservedRanges[i].base) {
            native_gpu_pool_board[3] = kIOReturnBadArgument;
            chosen->release();
            return;
        }
    }
    OSData *table = OSDynamicCast(OSData, chosen->getProperty("darwinboot-memory-outside"));
    if (table && table->getLength() >= 16) {
        const uint8_t *bytes = static_cast<const uint8_t *>(table->getBytesNoCopy());
        for (unsigned i = 0; i + 16 <= table->getLength(); i += 16) {
            uint64_t base = 0, size = 0;
            memcpy(&base, bytes + i, 8);
            memcpy(&size, bytes + i + 8, 8);
            if (!size) continue;
            IOReturn last = kIOReturnSuccess;
            addRun(base, base + size, &last);
        }
    }
    chosen->release();
}
