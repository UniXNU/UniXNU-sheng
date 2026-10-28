// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bind a mounted BSD device to an owner-verified UFS root window.
//
// The owner-root kernel patches ask these queries after mounting: the device
// must be the BSD node of exactly one IOMedia whose provider chain reaches a
// NativeUfs window that verified the media's volume UUID.
#include <IOKit/IOService.h>
#include <IOKit/IOBSD.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <sys/types.h>
#include <sys/param.h>

extern "C" {
int native_ufs_block_authenticates(IOService *provider, const char *uuid, uint8_t *digest);
int native_verified_block_root_authorized(dev_t device);
int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48]);
int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size);
}

static constexpr size_t kDigestBytes = 48;

// 1 when `device` is the only IOMedia with that BSD number and a UFS window
// below it authenticates the media UUID (and `expectedUUID`, when given).
static int verifiedVolumeQuery(dev_t device, const char *expectedUUID, uint8_t *manifestDigest)
{
    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(2);
    OSNumber *mjr = OSNumber::withNumber(static_cast<unsigned>(major(static_cast<uint32_t>(device))), 32);
    OSNumber *mnr = OSNumber::withNumber(static_cast<unsigned>(minor(static_cast<uint32_t>(device))), 32);
    bool ready = matching && properties && mjr && mnr &&
                 properties->setObject(kIOBSDMajorKey, mjr) && properties->setObject(kIOBSDMinorKey, mnr) &&
                 matching->setObject("IOPropertyMatch", properties);
    OSSafeReleaseNULL(mnr);
    OSSafeReleaseNULL(mjr);
    OSSafeReleaseNULL(properties);
    OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!iterator) return 0;
    bool authenticated = false;
    unsigned matches = 0;
    while (OSObject *object = iterator->getNextObject()) {
        IOService *media = OSDynamicCast(IOService, object);
        if (!media) continue;
        if (++matches != 1) { authenticated = false; break; }
        OSString *uuid = OSDynamicCast(OSString, media->getProperty("UUID"));
        if (!uuid || uuid->getLength() != 36 ||
            (expectedUUID && strcmp(expectedUUID, uuid->getCStringNoCopy()))) continue;
        unsigned depth = 0;
        for (IOService *provider = media; provider && depth < 64; provider = provider->getProvider(), ++depth) {
            const int ufs = native_ufs_block_authenticates(provider, uuid->getCStringNoCopy(), manifestDigest);
            if (ufs >= 0) {
                authenticated = ufs == 1;
                break;
            }
        }
    }
    iterator->release();
    if (!authenticated || matches != 1) {
        if (manifestDigest) bzero(manifestDigest, kDigestBytes);
        return 0;
    }
    return 1;
}

extern "C" int native_verified_block_root_authorized(dev_t device)
{
    return verifiedVolumeQuery(device, nullptr, nullptr);
}

extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
    if (!digest) return 0;
    bzero(digest, kDigestBytes);
    if (!uuid || strlen(uuid) != 36) return 0;
    return verifiedVolumeQuery(device, uuid, digest);
}

extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
    if (!uuid || !device || !path || size < 6 || strlen(uuid) != 36) return 0;
    *device = NODEV;
    path[0] = 0;
    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    OSString *wanted = OSString::withCString(uuid);
    bool ready = matching && properties && wanted && properties->setObject("UUID", wanted) &&
                 matching->setObject("IOPropertyMatch", properties);
    OSSafeReleaseNULL(wanted);
    OSSafeReleaseNULL(properties);
    OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!iterator) return 0;
    unsigned matches = 0;
    bool authenticated = false;
    dev_t candidate = NODEV;
    char candidatePath[64] = {};
    while (OSObject *object = iterator->getNextObject()) {
        IOService *media = OSDynamicCast(IOService, object);
        if (!media) continue;
        if (++matches != 1) { authenticated = false; break; }
        OSString *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
        OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
        OSNumber *mjr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
        OSNumber *mnr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
        if (!identity || strcmp(identity->getCStringNoCopy(), uuid) || !name || !mjr || !mnr ||
            !name->getLength() || name->getLength() >= sizeof(candidatePath) - 6 || name->getLength() >= size - 6 ||
            mjr->unsigned64BitValue() > 255 || mnr->unsigned64BitValue() > 0xffffff) continue;
        candidate = static_cast<dev_t>(makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
        if (!native_verified_block_root_authorized(candidate)) continue;
        snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", name->getCStringNoCopy());
        authenticated = true;
    }
    iterator->release();
    if (!authenticated || matches != 1) return 0;
    *device = candidate;
    strlcpy(path, candidatePath, size);
    return 1;
}
