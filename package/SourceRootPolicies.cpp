// SPDX-License-Identifier: BSD-2-Clause-Patent
// Authenticate the firmware's root-policy sidecar with a compiled-in owner key.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_kern_xnu.h>
#include <mach/mach_vm.h>
#include "verified_root.h"
#include "VerifiedBlockPolicy.h"

extern "C" {
extern const uint8_t verified_root_key[];
extern const struct vr_crypto vr_xnu_crypto;
NativeVerifiedBlockPolicy verified_root_policies[16];
uint64_t verified_root_policy_count;
}

static uint64_t read64(const uint8_t *p)
{
    uint64_t n = 0;
    for (unsigned i = 0; i != 8; ++i) n |= uint64_t(p[i]) << (i * 8);
    return n;
}

extern "C" bool native_source_root_policies_load(void)
{
    if (verified_root_policy_count) return true;
    IORegistryEntry *entry = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (!entry) return false;
    OSData *property = OSDynamicCast(OSData, entry->getProperty("RootPolicies"));
    uint64_t physical = 0, length = 0;
    if (property && property->getLength() == 16) {
        const auto *data = static_cast<const uint8_t *>(property->getBytesNoCopy());
        physical = read64(data);
        length = read64(data + 8);
    }
    entry->release();
    if (!physical || (physical & 4095) || length < 32 || length > 128ULL * 1024 * 1024 ||
        physical > UINT64_MAX - length) return false;
    IOMemoryDescriptor *descriptor = IOMemoryDescriptor::withPhysicalAddress(physical, length, kIODirectionIn);
    if (!descriptor) return false;
    IOMemoryMap *mapping = descriptor->map();
    if (!mapping) { descriptor->release(); return false; }
    vm_offset_t allocation = 0;
    const vm_size_t allocationBytes = round_page(static_cast<vm_size_t>(length));
    const kern_return_t allocated = kmem_alloc(kernel_map, &allocation, allocationBytes,
        static_cast<kma_flags_t>(KMA_KOBJECT | KMA_DATA), VM_KERN_MEMORY_SECURITY);
    if (allocated == KERN_SUCCESS)
        memcpy(reinterpret_cast<void *>(allocation), reinterpret_cast<const void *>(mapping->getVirtualAddress()), length);
    mapping->release();
    descriptor->release();
    if (allocated != KERN_SUCCESS) return false;
    // Protect the copied bytes before authenticating; later firmware or source
    // mappings cannot alter the data from which we publish policy pointers.
    if (mach_vm_protect(kernel_map, allocation, allocationBytes, TRUE, VM_PROT_READ) != KERN_SUCCESS)
        return false;
    const auto *bytes = reinterpret_cast<const uint8_t *>(allocation);
    if (memcmp(bytes, "VROOTB01", 8) || read64(bytes + 8) != 1 || read64(bytes + 24) != length)
        return false;
    const uint64_t count = read64(bytes + 16);
    if (!count || count > 16 || 32 + count * 16 > length) return false;
    uint64_t cursor = 32 + count * 16;
    for (uint64_t i = 0; i != count; ++i) {
        const uint64_t offset = read64(bytes + 32 + i * 16);
        const uint64_t size = read64(bytes + 40 + i * 16);
        if (offset != cursor || size < VR_HEADER_BYTES + VR_SIGNATURE_BYTES || size > length - cursor)
            return false;
        // A policy is either the whole envelope or only its signed header, whose hash table
        // is a userdata object that NativeUfs reads and checks against the signed digest.
        struct vr_view view = {};
        const bool headerOnly = size == VR_HEADER_BYTES + VR_SIGNATURE_BYTES;
        if ((headerOnly ? vr_open_header(bytes + offset, size, verified_root_key, 1, &vr_xnu_crypto, &view) :
                          vr_open(bytes + offset, size, verified_root_key, 1, &vr_xnu_crypto, &view)) != VR_OK)
            return false;
        verified_root_policies[i] = {bytes + offset, size};
        cursor += size;
    }
    if (cursor != length) return false;
    verified_root_policy_count = count;
    return true;
}
