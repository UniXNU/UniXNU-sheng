// SPDX-License-Identifier: BSD-2-Clause-Patent
// Start the source-built UFS adapter after the original storage family loads.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <string.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void source_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void source_storage_ufs_init(void) __asm__("__GLOBAL__sub_I_NativeUfs.cpp");
extern "C" void source_storage_ram_init(void) __asm__("__GLOBAL__sub_I_NativeRamBlock.cpp");
extern "C" IOService *native_ufs_source_create(void);
extern "C" bool native_source_root_policies_load(void);
// NativeUfs records how far root start got and the last soft-failure reason.
extern "C" volatile uint32_t native_ufs_root_step;
extern "C" const char *volatile native_ufs_root_failure;

// NativeUfs polls the optional board diagnostic during window reads. The
// minimal kernel does not include it; a weak definition keeps the archive
// linkable and is replaced when the diagnostic component is built in.
extern "C" __attribute__((weak)) void boot_progress_poll(void) {}
// Starts the sheng-log writer (BootLog.c); the board page calls it when that page is built in.
extern "C" void boot_log_tick(unsigned now);
extern "C" volatile uint32_t boot_log_state, boot_log_rounds, boot_log_errors;
extern "C" const char *volatile boot_log_open_refusal;

// Without the board page nothing shows the writer's state, so a log that cannot be written stops the boot
// with the reason on the panic screen instead of leaving an unattended run without a log.
// Once it writes, a writer whose rounds stop for 20 s (a hang elsewhere that blocks it) also panics: the panic screen's log flush then
// keeps the message buffer up to the hang.
static void boot_log_check(void *, wait_result_t)
{
    unsigned waited = 0;
    for (; waited < 30 && !boot_log_rounds; ++waited) {
        if (boot_log_state == 3) break;
        IOSleep(1000);
    }
    if (!boot_log_rounds)
        panic("SourceStorage: sheng-log has no log (state %u, rounds %u, errors %u, open: %s)", boot_log_state,
              boot_log_rounds, boot_log_errors, boot_log_open_refusal ? boot_log_open_refusal : "no refusal");
    uint32_t last = boot_log_rounds;
    for (unsigned still = 0; boot_log_state == 2; IOSleep(1000)) {
        const uint32_t now = boot_log_rounds;
        still = now == last ? still + 1 : 0;
        last = now;
        if (still >= 20)
            panic("SourceStorage: sheng-log writer stalled for 20 s after %u rounds (errors %u)", now, boot_log_errors);
    }
}


// ---- UFS benchmark (sheng-ufs-bench=1) ---------------------------------------------------------------------------------------
// The same tests as the Linux reader's ufsbench (fixed-seed xorshift per test and worker, so both read the same LBAs): random reads of
// 4 KiB to 1 MiB at queue depth 1, 8 and 32, then sequential reads, inside the flashed root image of userdata (from 1 MiB, 14304 MiB),
// through the driver's partition read path (native_ufs_part_read: 128 KiB commands from the slot pool). Queue depth N = N kernel threads
// with one read each. It runs before the boot goes on, prints one line per test into the message buffer, reads the UniPro link
// attributes, waits for two log rounds to put it on UFS and then restarts into fastboot. Nothing is written.
extern "C" int native_ufs_part_find(const char *, uint32_t *, uint64_t *, uint64_t *) __attribute__((weak_import));
extern "C" int native_ufs_part_read(uint32_t, uint64_t, uint32_t, void *) __attribute__((weak_import));
extern "C" int native_ufs_dme_get(uint32_t, uint32_t, uint32_t *) __attribute__((weak_import));
extern "C" int boot_reset_probe(char *, unsigned) __attribute__((weak_import));
extern "C" volatile uint64_t native_ufs_irqs __attribute__((weak_import)), native_ufs_irq_sleeps __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_irq_mode __attribute__((weak_import)), native_ufs_list_mode __attribute__((weak_import));
extern "C" void native_ufs_slot_stats(uint32_t *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" void native_ufs_set_zero_copy(int) __attribute__((weak_import));
extern "C" void native_ufs_set_spin(uint32_t) __attribute__((weak_import));
extern "C" int boot_reset_bootloader(char *, unsigned) __attribute__((weak_import));
namespace {
enum { kBenchBuckets = 4000, kBenchBucketNs = 5000 };
struct BenchWorker {
  uint64_t first, blocks, bytes, seed, stream, streams, deadline, start;
  uint32_t lun; int random;
  uint8_t *buffer;
  uint64_t ops, sumNs, maxNs, errors;
  uint32_t hist[kBenchBuckets];
  volatile uint32_t *done;
};
uint64_t benchFreq;
uint64_t benchNs(uint64_t ticks) { return ticks * 1000000000ULL / benchFreq; }
void benchWork(void *arg, wait_result_t)
{
  BenchWorker *w = static_cast<BenchWorker *>(arg);
  uint64_t x = w->seed * 0x9e3779b97f4a7c15ULL + 1, count = w->blocks / (w->bytes / 4096), pos = count / w->streams * w->stream;
  while (mach_absolute_time() < w->start) {}
  while (mach_absolute_time() < w->deadline) {
    uint64_t index;
    if (w->random) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; index = x % count; }
    else { index = pos++; if (pos >= count) pos = 0; }
    const uint64_t t0 = mach_absolute_time();
    const int ok = native_ufs_part_read(w->lun, w->first + index * (w->bytes / 4096), static_cast<uint32_t>(w->bytes / 4096), w->buffer);
    const uint64_t dt = benchNs(mach_absolute_time() - t0);
    if (!ok) { ++w->errors; continue; }
    ++w->ops; w->sumNs += dt; if (dt > w->maxNs) w->maxNs = dt;
    const uint64_t b = dt / kBenchBucketNs; w->hist[b < kBenchBuckets ? b : kBenchBuckets - 1]++;
  }
  __atomic_add_fetch(w->done, 1, __ATOMIC_RELEASE);
}
uint64_t benchPercentile(const uint32_t *hist, uint64_t ops, uint64_t permille)
{
  uint64_t want = (ops * permille + 999) / 1000, seen = 0;
  for (unsigned i = 0; i < kBenchBuckets; ++i) { seen += hist[i]; if (seen >= want && want) return uint64_t(i + 1) * kBenchBucketNs; }
  return 0;
}
void benchTest(BenchWorker *workers, uint32_t lun, uint64_t first, uint64_t blocks, uint64_t bytes, uint32_t qd, int random, uint32_t id,
               const char *tag = "")
{
  volatile uint32_t done = 0;
  const uint64_t start = mach_absolute_time() + benchFreq / 20, deadline = start + 3 * benchFreq;
  for (uint32_t i = 0; i < qd; ++i) {
    BenchWorker &w = workers[i];
    uint8_t *buffer = w.buffer;
    bzero(&w, sizeof w);
    w.buffer = buffer; w.first = first; w.blocks = blocks; w.bytes = bytes; w.seed = (uint64_t(id) << 8) | i; w.stream = i;
    w.streams = qd; w.deadline = deadline; w.start = start; w.lun = lun; w.random = random; w.done = &done;
    thread_t thread;
    if (kernel_thread_start(benchWork, &w, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else __atomic_add_fetch(&done, 1, __ATOMIC_RELEASE);
  }
  while (__atomic_load_n(&done, __ATOMIC_ACQUIRE) < qd) IOSleep(10);
  uint32_t *hist = static_cast<uint32_t *>(IOMallocZero(sizeof(uint32_t) * kBenchBuckets));   // allocated: the kernel's __DATA is fixed
  if (!hist) return;
  uint64_t ops = 0, sum = 0, max = 0, errors = 0;
  for (uint32_t i = 0; i < qd; ++i) {
    ops += workers[i].ops; sum += workers[i].sumNs; errors += workers[i].errors;
    if (workers[i].maxNs > max) max = workers[i].maxNs;
    for (unsigned b = 0; b < kBenchBuckets; ++b) hist[b] += workers[i].hist[b];
  }
  const uint64_t avg = ops ? sum / ops : 0, mbps = ops * bytes / 3 * 1000 / 1000000;
  printf("ufsbench %s%s bs=%llu qd=%u ops=%llu iops=%llu MBps=%llu.%03llu avg_us=%llu.%03llu p50_us=%llu p99_us=%llu max_us=%llu err=%llu\n",
         random ? "rand" : "seq ", tag, (unsigned long long)bytes, qd, (unsigned long long)ops, (unsigned long long)(ops / 3),
         (unsigned long long)(mbps / 1000), (unsigned long long)(mbps % 1000), (unsigned long long)(avg / 1000),
         (unsigned long long)(avg % 1000), (unsigned long long)(benchPercentile(hist, ops, 500) / 1000),
         (unsigned long long)(benchPercentile(hist, ops, 990) / 1000), (unsigned long long)(max / 1000), (unsigned long long)errors);
  IOFree(hist, sizeof(uint32_t) * kBenchBuckets);
}
void ufsBench(void)
{
  uint32_t on = 0;
  if (!PE_parse_boot_argn("sheng-ufs-bench", &on, sizeof on) || on != 1) return;
  if (!&native_ufs_part_find || !&native_ufs_part_read) { printf("ufsbench: driver exports missing\n"); return; }
  for (unsigned waited = 0; waited < 300 && !boot_log_rounds; ++waited) IOSleep(100);   // the log writer is running
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(benchFreq));
  if (!benchFreq) benchFreq = 19200000;
  uint32_t lun = 0; uint64_t part = 0, partBlocks = 0;
  if (!native_ufs_part_find("userdata", &lun, &part, &partBlocks)) { printf("ufsbench: no userdata\n"); return; }
  uint8_t *probe = static_cast<uint8_t *>(IOMallocAligned(4096, 4096));
  if (!probe) return;
  if (&native_ufs_dme_get) {
    // PA_ActiveTxDataLanes, PA_ActiveRxDataLanes, PA_TxGear, PA_RxGear, PA_HSSeries, PA_PWRMode, PA_TxTermination, PA_RxTermination,
    // PA_MaxRxHSGear, PA_ConnectedTxDataLanes, PA_ConnectedRxDataLanes
    static const uint32_t attributes[] = {0x1560, 0x1580, 0x1568, 0x1583, 0x156a, 0x1571, 0x1569, 0x1584, 0x1587, 0x1561, 0x1581};
    for (uint32_t attribute : attributes) {
      uint32_t value = 0;
      native_ufs_part_read(lun, part, 1, probe);   // a transfer first: the link may sit in auto-hibernate
      const int ok = native_ufs_dme_get(attribute, 0, &value);
      printf("ufsbench dme %#x=%#x%s\n", attribute, value, ok ? "" : " (failed)");
    }
  }
  const uint64_t first = part + (1ULL << 20) / 4096, blocks = (14304ULL << 20) / 4096;
  printf("ufsbench userdata lun=%u first=%llu blocks=%llu region=%llu+%llu\n", lun, (unsigned long long)part,
         (unsigned long long)partBlocks, (unsigned long long)first, (unsigned long long)blocks);
  BenchWorker *workers = static_cast<BenchWorker *>(IOMallocZero(sizeof(BenchWorker) * 32));
  if (!workers) { printf("ufsbench: no memory\n"); return; }
  for (unsigned k = 0; k < 32; ++k) {
    workers[k].buffer = static_cast<uint8_t *>(IOMallocAligned(1 << 20, 4096));
    if (!workers[k].buffer) { printf("ufsbench: no memory\n"); return; }
  }
  // Zero copy must read what the data-area path reads: the same random LBAs at every size, compared byte for byte.
  if (&native_ufs_set_zero_copy) {
    uint8_t *a = workers[0].buffer, *b = workers[1].buffer;
    uint64_t x = 0x5eed, mismatches = 0, reads = 0;
    static const uint32_t counts[] = {1, 4, 32, 64, 256};
    for (uint32_t n : counts) for (unsigned k = 0; k < 200; ++k) {
      x ^= x << 13; x ^= x >> 7; x ^= x << 17;
      const uint64_t lba = first + x % (blocks - n);
      native_ufs_set_zero_copy(1); memset(a, 0x11, n * 4096ULL);
      const int ok1 = native_ufs_part_read(lun, lba, n, a);
      native_ufs_set_zero_copy(0); memset(b, 0x22, n * 4096ULL);
      const int ok2 = native_ufs_part_read(lun, lba, n, b);
      ++reads;
      if (!ok1 || !ok2 || memcmp(a, b, n * 4096ULL)) ++mismatches;
    }
    native_ufs_set_zero_copy(1);
    printf("ufsbench verify zero-copy reads=%llu mismatches=%llu\n", (unsigned long long)reads, (unsigned long long)mismatches);
    if (mismatches) {
      native_ufs_set_zero_copy(0);
      printf("ufsbench verify FAIL: zero copy off for the rest of the bench\n");
    }
  }
  static const uint64_t sizes[] = {4096, 16384, 131072, 1048576};
  static const uint32_t depths[] = {1, 8, 32};
  uint32_t id = 1;
  for (uint64_t size : sizes) for (uint32_t depth : depths) benchTest(workers, lun, first, blocks, size, depth, 1, id++);
  benchTest(workers, lun, first, blocks, 131072, 1, 0, id++);
  benchTest(workers, lun, first, blocks, 1048576, 1, 0, id++);
  benchTest(workers, lun, first, blocks, 1048576, 8, 0, id++);
  // Spin budget sweep (a completion wait polls this long, then sleeps on the interrupt): small blocks at queue depth 8 and 32.
  if (&native_ufs_set_spin) {
    static const uint32_t spins[] = {50, 100, 400, 100000};
    static const char *const names[] = {" spin=50", " spin=100", " spin=400", " spin=all"};
    for (unsigned p = 0; p < 4; ++p) {
      native_ufs_set_spin(spins[p]);
      static const uint64_t sweepSizes[] = {4096, 16384};
      static const uint32_t sweepDepths[] = {8, 32};
      for (uint64_t size : sweepSizes) for (uint32_t depth : sweepDepths) {
        const uint32_t testId = (size == 4096 ? 1 : 4) + (depth == 8 ? 1 : 2);   // the same LBAs as the main test
        benchTest(workers, lun, first, blocks, size, depth, 1, testId, names[p]);
      }
    }
    native_ufs_set_spin(200);
  }
  {
    uint32_t slots = 0, peak = 0; uint64_t waits = 0;
    if (&native_ufs_slot_stats) native_ufs_slot_stats(&slots, &peak, &waits);
    printf("ufsbench host slots=%u peak=%u waits=%llu list=%u irq_mode=%u irqs=%llu sleeps=%llu\n", slots, peak, (unsigned long long)waits,
           &native_ufs_list_mode ? native_ufs_list_mode : 9,
           &native_ufs_irq_mode ? native_ufs_irq_mode : 0, &native_ufs_irqs ? (unsigned long long)native_ufs_irqs : 0ULL,
           &native_ufs_irq_sleeps ? (unsigned long long)native_ufs_irq_sleeps : 0ULL);
  }
  printf("ufsbench done\n");
  for (unsigned k = 0; k < 32; ++k) IOFreeAligned(workers[k].buffer, 1 << 20);
  const uint32_t rounds = boot_log_rounds;
  while (boot_log_rounds < rounds + 2) IOSleep(500);
  char text[200] = "no probe";
  if (&boot_reset_probe && &boot_reset_bootloader && boot_reset_probe(text, sizeof text) == 0) boot_reset_bootloader(text, sizeof text);
  printf("ufsbench: no restart into fastboot (%s)\n", text);
  for (;;) IOSleep(1000);   // a bench boot never goes on
}
}  // namespace

static const OSMetaClass *storageParent;
static IOService *storageDriver;
static bool attempted;

// Used only by the deferred source IOBlockStorageDevice metaclass constructor.
// The C++ class name stays unchanged for compiler-authored arm64e discriminators.
extern "C" const OSMetaClass *native_source_storage_parent(void)
{
    if (!storageParent) panic("SourceStorage: metaclass initialized before original family");
    return storageParent;
}

// The Data seed the loader placed in /chosen/memory-map. Its disk starts after UFS, whose store window
// keeps the changed pages of the seed when the board enables persistence.
static void start_data_disk(IOService *provider)
{
    IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    const bool seed = map && (map->getProperty("PackedRAMDisk") || map->getProperty("RAMDisk"));
    OSSafeReleaseNULL(map);
    if (!seed) return;
    IOService *disk = OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NativeRamBlock"));
    if (!disk) panic("SourceStorage: cannot allocate the Data disk");
    const char *step = !disk->init() ? "init" : !disk->attach(provider) ? "attach" : !disk->start(provider) ? "start" : nullptr;
    if (step) panic("SourceStorage: Data disk %s failed", step);
    printf("SourceStorage: Data disk started\n");
}

extern "C" void native_source_storage_start(void)
{
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("native-source-storage", &enabled, sizeof(enabled)) || enabled != 1 || attempted)
        return;
    attempted = true;
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess)
        panic("SourceStorage: IOStorageFamily load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("SourceStorage: original base layout %u differs from source %lu",
              storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));

    // Authenticate policies before registering hardware-facing classes. The
    // firmware hands off the envelope; the trusted anchor is compiled in.
    if (!native_source_root_policies_load())
        panic("SourceStorage: root policy handoff or authentication failed");
    status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
    if (status != kOSReturnSuccess)
        panic("SourceStorage: APFS load failed %x", status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("SourceStorage: cannot begin class registration");
    source_storage_base_init();
    source_storage_ufs_init();
    source_storage_ram_init();
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess)
        panic("SourceStorage: class registration failed %x", status);

    OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (!provider) panic("SourceStorage: platform root unavailable");
    storageDriver = native_ufs_source_create();
    if (!storageDriver) panic("SourceStorage: cannot allocate UFS service");
    const char *step = !storageDriver->init() ? "init" : !storageDriver->attach(provider) ? "attach" :
                       !storageDriver->start(provider) ? "start" : nullptr;
    if (step)
        panic("SourceStorage: UFS %s failed (root step %u: %s)", step, native_ufs_root_step,
              native_ufs_root_failure ? native_ufs_root_failure : "no reason recorded");
    printf("SourceStorage: UFS started through public IOStorageFamily interface\n");
    start_data_disk(provider);
    // The UFS log area exists from here on; the writer only runs with sheng-log=1.
    boot_log_tick(0);
    unsigned log = 0;
    thread_t check;
    if (PE_parse_boot_argn("sheng-log", &log, sizeof(log)) && log == 1 &&
        kernel_thread_start(boot_log_check, nullptr, &check) == KERN_SUCCESS)
        thread_deallocate(check);
    ufsBench();
}
