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

// Memory the stock firmware keeps from the HLOS although the mainline board tree lists none of it: the modem's DSM region and the
// trusted UI and OEM virtual machines with their rings and bounce buffers (stock vendor_boot reserved-memory, no-map; the OEM VM's
// size is Kalama's), and the run above the hardware fence buffer up to the LLCC region, where a store faulted on the first page
// (run73: 0xe6720000). A CPU store into such memory is a synchronous external abort (the pool's zero fill panicked with "Unhandled
// implementation specific error"), so these parts of the runs never reach the driver.
static const struct {uint64_t base, end;} kStockReserved[] = {
    {0x8a800000ull, 0x9b000000ull},   // mpss
    {0xd4d00000ull, 0xd8000000ull},   // mpss_dsm
    {0xd8000000ull, 0xe3940000ull},   // tz reserved, xbl_sc, cpucp, qtee, ta, tz tags
    {0xe6720000ull, 0xf3800000ull},   // after the hardware fence buffer: run73 faulted on its first page (0xe6720000) in the pool's fill
    {0xf3800000ull, 0xfc800000ull},   // trust_ui_vm (dump, qrtr, vblk rings, swiotlb), oem_vm rings, swiotlb and region
    {0xfc800000ull, 0xff800000ull},   // up to llcc_lpi: the rest of the same firmware-held run (run73), never shown accessible
    {0xff800000ull, 0xffe00000ull},   // llcc_lpi
};
// KiB of the runs left out as stock reserved memory, for the boot log.
extern "C" {volatile uint32_t native_gpu_pool_reserved_kib;}

static void addRun(uint64_t base, uint64_t end, IOReturn *last)
{
    for (const auto &r : kStockReserved) {
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
