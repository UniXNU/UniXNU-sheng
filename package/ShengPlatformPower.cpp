// SPDX-License-Identifier: MIT
// Board-side original PMGR ABI qualification. Default off. Modes: 1 releases
// unpublished objects, 2 publishes the original nub, 3 tests power refusal.
// No clocks, sleep, reset or secondary-CPU success is advertised.
#include "OriginalPMGRABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *);
void sheng_pmgr_static_init(void) __asm__("__GLOBAL__sub_I_ShengPlatformPower.cpp");
IOService *sheng_original_pmgr_nub_create(IORegistryEntry *, ApplePMGR *);
uint32_t sheng_original_pmgr_nub_read(IOService *);
void sheng_original_pmgr_nub_write(IOService *, uint64_t);
// stage,status,parent-size,own-size,provider,nub,bound-provider,read-before,
// read-after,own-before,own-after,nub-before,nub-after,attached,registered,mode.
volatile uint64_t sheng_pmgr_observation[16];
__attribute__((noinline)) void sheng_pmgr_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
}

__attribute__((noreturn)) static void unavailable(unsigned slot)
{
  sheng_pmgr_observation[0] = 0x100 + slot;
  sheng_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnUnsupported);
  sheng_pmgr_checkpoint();
  panic("ShengPlatformPower: unimplemented operation %u", slot);
}

class ShengPlatformPower : public ApplePMGR {
  OSDeclareDefaultStructors(ShengPlatformPower)
public:
  void enableCPUCore(unsigned, uint64_t) override { unavailable(168); }
  void enableCPUCore(unsigned) override { unavailable(169); }
  void disableCPUCore(unsigned) override { unavailable(170); }
  void enableCPUCluster(unsigned) override { unavailable(171); }
  void disableCPUCluster(unsigned) override { unavailable(172); }
  void initCPUIdle(ml_processor_info_t *) override { unavailable(173); }
  void enterCPUIdle(UInt64 *) override { unavailable(174); }
  void exitCPUIdle(UInt64 *) override { unavailable(175); }
  void updateCPUIdle(UInt64 *) override { unavailable(176); }
#include "GuardedPMGRSlots.inc"
};
OSDefineMetaClassWithInit(ShengPlatformPower, ApplePMGR, )
OSMetaClassConstructorInit(ShengPlatformPower, ApplePMGR, )
OSDefineBasicStructors(ShengPlatformPower, ApplePMGR)
// The original object's storage exceeds kalloc_type's 16 KiB size limit.
// Use XNU's existing zeroed, size-aware OSObject heap allocation pair.
void *ShengPlatformPower::operator new(size_t size) { return OSObject::operator new(size); }
void ShengPlatformPower::operator delete(void *memory, size_t size)
{
  OSObject::operator delete(memory, size);
}
static_assert(sizeof(ShengPlatformPower) == 0x4b8a8, "Exact original provider storage");

static const OSMetaClass *lookup(const char *text)
{
  const OSSymbol *name = OSSymbol::withCString(text);
  const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
  OSSafeReleaseNULL(name);
  return meta;
}

extern "C" void sheng_pmgr_run(void)
{
  unsigned mode = 0;
  if (!PE_parse_boot_argn("pmgr", &mode, sizeof(mode)) || !mode) return;
  sheng_pmgr_observation[15] = mode;
  sheng_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnError);
  if (mode > 3 || sheng_pmgr_observation[0]) { sheng_pmgr_checkpoint(); return; }
  sheng_pmgr_observation[0] = 1;
  kern_return_t result = OSKextLoadKextWithIdentifier("com.apple.driver.ApplePMGR");
  if (result) { sheng_pmgr_observation[1] = static_cast<uint32_t>(result); sheng_pmgr_checkpoint(); return; }
  const OSMetaClass *parent = lookup("ApplePMGR"), *nubMeta = lookup("ApplePMGRNub");
  if (!parent || parent != &ApplePMGR::gMetaClass || parent->getClassSize() != 0x4b8a8 ||
      !nubMeta || nubMeta->getClassSize() != 0x740) { sheng_pmgr_checkpoint(); return; }
  sheng_pmgr_observation[2] = parent->getClassSize();
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) { sheng_pmgr_checkpoint(); return; }
  sheng_pmgr_static_init();
  result = OSMetaClass::postModLoad(handle);
  if (result) { sheng_pmgr_observation[1] = static_cast<uint32_t>(result); sheng_pmgr_checkpoint(); return; }
  sheng_pmgr_observation[9] = ShengPlatformPower::gMetaClass.getInstanceCount();
  sheng_pmgr_observation[11] = nubMeta->getInstanceCount();
  ShengPlatformPower *provider = new ShengPlatformPower;
  IORegistryEntry *description = new IORegistryEntry;
  IOService *nub = nullptr;
  do {
    if (!provider || !description || !provider->init() || !description->init()) break;
    sheng_pmgr_observation[0] = 2;
    sheng_pmgr_observation[3] = provider->getMetaClass()->getClassSize();
    sheng_pmgr_observation[4] = reinterpret_cast<uintptr_t>(provider);
    // Empty resource description: no Apple register map or device capabilities.
    description->setName("sheng-platform-power");
    nub = sheng_original_pmgr_nub_create(description, provider);
    sheng_pmgr_observation[5] = reinterpret_cast<uintptr_t>(nub);
    if (!nub || nub->getMetaClass() != nubMeta || !provider->metaCast("ApplePMGR")) break;
    uintptr_t bound = 0;
    memcpy(&bound, reinterpret_cast<const char *>(nub) + 0x88, sizeof(bound));
    sheng_pmgr_observation[6] = bound;
    if (bound != reinterpret_cast<uintptr_t>(provider)) break;
    sheng_pmgr_observation[0] = 3;
    uint32_t value = sheng_original_pmgr_nub_read(nub);
    sheng_pmgr_observation[7] = value;
    sheng_original_pmgr_nub_write(nub, uint64_t(value | 4));
    sheng_pmgr_observation[8] = sheng_original_pmgr_nub_read(nub);
    sheng_pmgr_observation[13] = nub->getProvider() != nullptr || provider->getProvider() != nullptr;
    sheng_pmgr_observation[14] = (nub->getState() | provider->getState()) & kIOServiceRegisteredState;
    if (value != 0xffffffff || sheng_pmgr_observation[8] != 0xffffffff ||
        sheng_pmgr_observation[13] || sheng_pmgr_observation[14]) break;
    if (mode == 3) provider->disableCPUCore(1);
    if (mode == 2) {
      IOService *platform = IOService::getPlatform();
      if (!platform || !provider->attach(platform)) break;
      if (!provider->start(platform)) { provider->detach(platform); break; }
      if (!nub->attach(provider)) {
        provider->stop(platform); provider->detach(platform); break;
      }
      if (!nub->start(provider)) {
        nub->detach(provider); provider->stop(platform); provider->detach(platform); break;
      }
      provider->registerService(kIOServiceSynchronous);
      nub->registerService(kIOServiceSynchronous);
      sheng_pmgr_observation[0] = 5;
      sheng_pmgr_observation[13] = nub->getProvider() == provider && provider->getProvider() == platform;
      sheng_pmgr_observation[14] = (nub->getState() & provider->getState()) & kIOServiceRegisteredState;
      auto matching = IOService::serviceMatching("ApplePMGRNub");
      IOService *matched = matching ? IOService::waitForMatchingService(matching, 1000000) : nullptr;
      OSSafeReleaseNULL(matching);
      // Check actual service discovery and genuine type identity, then retain
      // both live objects. The registry and original nub refer to this provider.
      if (matched != nub || matched->getMetaClass() != nubMeta ||
          !sheng_pmgr_observation[13] || !sheng_pmgr_observation[14]) {
        sheng_pmgr_checkpoint();
        panic("ShengPlatformPower: original nub publication failed");
      }
      sheng_pmgr_observation[10] = ShengPlatformPower::gMetaClass.getInstanceCount();
      sheng_pmgr_observation[12] = nubMeta->getInstanceCount();
      sheng_pmgr_observation[1] = kIOReturnSuccess;
      OSSafeReleaseNULL(matched);
      OSSafeReleaseNULL(description);
      sheng_pmgr_checkpoint();
      return;
    }
    sheng_pmgr_observation[1] = kIOReturnSuccess;
  } while (false);
  OSSafeReleaseNULL(nub);
  OSSafeReleaseNULL(description);
  OSSafeReleaseNULL(provider);
  sheng_pmgr_observation[10] = ShengPlatformPower::gMetaClass.getInstanceCount();
  sheng_pmgr_observation[12] = nubMeta->getInstanceCount();
  if (sheng_pmgr_observation[9] != sheng_pmgr_observation[10] ||
      sheng_pmgr_observation[11] != sheng_pmgr_observation[12]) sheng_pmgr_observation[1] = static_cast<uint32_t>(kIOReturnError);
  sheng_pmgr_observation[0] = 4;
  sheng_pmgr_checkpoint();
}
