/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* sheng's VPU platform for the QcomIris driver (sheng-video=1): what mainline iris_probe and the SM8550 platform code give the iris
 * core. The VPU's translating domain on the apps SMMU (stream IDs 0x1940 and 0x1947), the HFI queues and SFR, the interrupt (SPI 174)
 * as a threaded interrupt, the deferred work (autosuspend and the system error recovery), the clocks and power domains through
 * Sm8550VideoCc, the RPMh votes, and the firmware through the peripheral authentication service (PAS 9) from the Data addon.
 *
 * RPMh: on the first power-on MMCX and MXC are voted to nominal and the MM1 bus clock enabled; the votes are never lowered (one
 * fixed operating point, MVS0 at 1332 MHz, the core at 444 MHz). Nothing else is voted. */
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include "AppsSmmuLock.h"
#include "QcomIrisDevice.h"
extern "C" {
#include "QcomMdt.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "Sm8550AppsSmmu.h"
#include "Sm8550VideoCc.h"
#include "ArmSmmu4KLive.h"
#include "QcomIrisMmu.h"
int sheng_fw_open(const char *path, void **handle, uint64_t *size);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);
int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
int armcpu_spi_mask(uint32_t, bool) __attribute__((weak_import));
extern volatile int native_gpu_addon_stage;
/* [0] stage (negative: the step that failed), [1] its error, [2] bank | asid << 8 | output bits << 24, [3] interrupts, [4] thread
 * passes, [5] core state, [6] last system error, [7] recoveries, [8] suspends, [9] resumes, [10] firmware load result, [11] videocc
 * failed step | value << 8, [12] SCM x0 of the last failed call, [13] its x1, [14] RPMh resources found (bit 0 mmcx, 1 mxc, 2 MM1),
 * [15] the last table status, [16] bytes mapped / 4 KiB, [17] live buffers. native_video_text: the firmware's image version. */
volatile int32_t native_video_stats[24];
char native_video_text[128];
/* The last buffer the VPU could not be given: [0] how many, [1] its bytes / 4 KiB, [2] the step (1 the memory, 2 wiring it, 3 its
 * translation), [3] the step's result, [4] the physical address bits the VPU's table reaches. */
volatile int32_t native_video_alloc[5];
/* The firmware's last debug messages (log-video=1 asks it for errors, fatal and high ones), newest last. */
char native_video_fw_text[4][100];
volatile uint32_t native_video_fw_lines;
/* Its error messages (VFW_E), whole: the first two kept, then the newest four. */
char native_video_fw_error[6][200];
volatile uint32_t native_video_fw_errors;
}

namespace {
constexpr uint64_t kVpu = QCOM_IRIS_BASE, kRsc = 0x17a20000, kCmdDb = 0x81c60000, kCarveout = 0x9bb00000;
constexpr uint64_t kCarveoutSize = 0x700000, kCmdDbSize = 0x20000;
constexpr uint32_t kIntid = 32 + QCOM_IRIS_SPI, kMvs0Hz = 1332000000u, kRscTcs = 0xd00;
constexpr unsigned kOwnedTcs = 2;                 // shared with USB and the microphone rail, under the apps SMMU lock
constexpr uint32_t kLevelNominal = 256;           // RPMH_REGULATOR_LEVEL_NOM
constexpr uint16_t kSids[2] = {0x1940, 0x1947};
constexpr unsigned kPollBudget = 1000;
const char kFirmware[] = "/private/var/ngpu/firmware/qcom/sm8550/sheng/vpu30_4v.mbn";

enum Stage {StMapped = 1, StDomain, StQueues, StInterrupt, StThreads, StReady};

struct Window {IOMemoryMap *map; volatile uint8_t *va; uint64_t size;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache)
{
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    w.size = size;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
uint32_t rd(const Window &w, uint64_t at) {return *reinterpret_cast<volatile uint32_t *>(w.va + at);}
void wr(const Window &w, uint64_t at, uint32_t value) {*reinterpret_cast<volatile uint32_t *>(w.va + at) = value; barrier();}
uint64_t nowMs() {uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
void cleanToPoc(const volatile void *start, size_t length)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    barrier();
}
void fail(int step, int error) {native_video_stats[1] = error; native_video_stats[0] = -step;}

// ---- table pages of the VPU domain: one wired page at a time, sorted by physical address ----
struct TablePages {
    struct Page {uint64_t physical; volatile uint64_t *mapping; IOBufferMemoryDescriptor *memory;};
    Page *pages = nullptr;
    size_t count = 0, capacity = 0;
    uint64_t limit = 0;
    ArmSmmu4KTablePages provider = {};
    size_t lowerBound(uint64_t physical) const
    {
        size_t low = 0, high = count;
        while (low < high) {const size_t mid = low + (high - low) / 2; if (pages[mid].physical < physical) low = mid + 1; else high = mid;}
        return low;
    }
    static int allocate(void *cookie, uint64_t *physical, volatile uint64_t **mapping)
    {
        auto *self = static_cast<TablePages *>(cookie);
        if (self->count == self->capacity) {
            const size_t size = self->capacity ? self->capacity * 2 : 64;
            Page *made = IONewData(Page, size);
            if (!made) return 1;
            if (self->count) memcpy(made, self->pages, sizeof(Page) * self->count);
            if (self->pages) IODeleteData(self->pages, Page, self->capacity);
            self->pages = made; self->capacity = size;
        }
        IOBufferMemoryDescriptor *page = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, 4096, (self->limit - 1) & ~UINT64_C(4095));
        if (!page) return 1;
        if (page->prepare() != kIOReturnSuccess) {page->release(); return 1;}
        IOByteCount length = 0;
        const addr64_t at = page->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
        void *virt = page->getBytesNoCopy();
        if (!at || (at & 4095) || at >= self->limit || length < 4096 || !virt) {page->complete(); page->release(); return 1;}
        memset(virt, 0, 4096);
        cleanToPoc(virt, 4096);
        const size_t i = self->lowerBound(at);
        memmove(self->pages + i + 1, self->pages + i, sizeof(Page) * (self->count - i));
        self->pages[i] = {at, static_cast<volatile uint64_t *>(virt), page};
        ++self->count;
        *physical = at; *mapping = static_cast<volatile uint64_t *>(virt);
        return 0;
    }
    static volatile uint64_t *find(void *cookie, uint64_t physical)
    {
        auto *self = static_cast<TablePages *>(cookie);
        const size_t i = self->lowerBound(physical);
        return i < self->count && self->pages[i].physical == physical ? self->pages[i].mapping : nullptr;
    }
    static int overlaps(void *cookie, uint64_t physical, uint64_t bytes)
    {
        auto *self = static_cast<TablePages *>(cookie);
        if (!bytes || !self->count) return 0;
        const size_t i = self->lowerBound(physical > 4095 ? physical - 4095 : 0);
        return i < self->count && self->pages[i].physical < physical + bytes;
    }
};
// The apps SMMU walks its tables without snooping: a changed page is cleaned to the point of coherency.
int publishTable(void *, volatile uint64_t *page, size_t bytes) {cleanToPoc(page, bytes); return 0;}

struct Video {
    Window vpu, gcc, videocc, smmu, rsc;
    IOLock *lock;
    struct qcom_iris_core core;
    struct qcom_iris_vpu_io vpuIo;
    struct sm8550_video_cc_io ccIo;
    struct sm8550_video_cc_state cc;
    struct sm8550_apps_smmu_domain domain;
    struct qcom_iris_mmu mmu;
    TablePages tables;
    QcomIrisBuffer uc;
    bool voted, ready, initSignalled;
    IOSimpleLock *irqLock;
    unsigned irqDepth;
    volatile uint32_t irqPending;
    uint64_t suspendAt, recoveryAt;   // 0: none
    uint32_t workPending;
} v;

// ---- register access ----
uint32_t vpuRead(void *, uint32_t offset) {return rd(v.vpu, offset);}
void vpuWrite(void *, uint32_t offset, uint32_t value) {wr(v.vpu, offset, value);}
void delayUs(void *, unsigned us) {IODelay(us);}
uint32_t ccRead(void *, enum sm8550_video_cc_window w, uint32_t offset) {return rd(w == Sm8550VideoGcc ? v.gcc : v.videocc, offset);}
void ccWrite(void *, enum sm8550_video_cc_window w, uint32_t offset, uint32_t value) {wr(w == Sm8550VideoGcc ? v.gcc : v.videocc, offset, value);}
uint32_t smmuRead(void *, uint32_t offset) {return rd(v.smmu, offset);}
void smmuWrite(void *, uint32_t offset, uint32_t value) {wr(v.smmu, offset, value);}
int kitRead32(void *, size_t at, uint32_t *value) {if (at + 4 > v.smmu.size) return 1; *value = rd(v.smmu, at); return 0;}
int kitRead64(void *, size_t at, uint64_t *value)
{
    if (at + 8 > v.smmu.size || (at & 7)) return 1;
    *value = *reinterpret_cast<volatile uint64_t *>(v.smmu.va + at);
    return 0;
}
int kitWrite32(void *, size_t at, uint32_t value) {if (at + 4 > v.smmu.size) return 1; wr(v.smmu, at, value); return 0;}
int kitWrite64(void *, size_t at, uint64_t value)
{
    if (at + 8 > v.smmu.size || (at & 7)) return 1;
    *reinterpret_cast<volatile uint64_t *>(v.smmu.va + at) = value;
    barrier();
    return 0;
}
int kitBarrier(void *) {barrier(); return 0;}

// ---- secure world calls: SMCCC with up to four value arguments, interrupted calls resumed, busy ones retried ----
struct ScmResult {int64_t x0; uint64_t x1;};
ScmResult scm(uint32_t svc, uint32_t command, unsigned count, uint64_t a, uint64_t b, uint64_t c, uint64_t d, unsigned rwSecond = 0)
{
    ScmResult result = {-1, 0};
    const uint32_t arginfo = qcom_scm_args(count, 0, rwSecond ? QCOM_SCM_RW : 0, 0);
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t function = qcom_scm_fn(svc, command), session = 0;
        for (unsigned resumes = 0;;) {
            register uint64_t x0 __asm__("x0") = function, x1 __asm__("x1") = arginfo, x2 __asm__("x2") = a, x3 __asm__("x3") = b,
                              x4 __asm__("x4") = c, x5 __asm__("x5") = d, x6 __asm__("x6") = session, x7 __asm__("x7") = 0;
            __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                             :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
            result = {(int64_t)x0, x1};
            if ((int64_t)x0 != QCOM_SCM_INTERRUPTED || ++resumes > 100000) break;
            function = QCOM_SCM_INTERRUPTED;
            session = x6;
        }
        if (result.x0 != QCOM_SCM_V2_EBUSY) break;
        IOSleep(30);
    }
    if (result.x0 || result.x1) {native_video_stats[12] = (int32_t)result.x0; native_video_stats[13] = (int32_t)result.x1;}
    return result;
}
bool scmOk(const ScmResult &r) {return r.x0 == 0 && r.x1 == 0;}
constexpr uint32_t kSvcBoot = 0x01, kBootSetRemoteState = 0x0a, kSvcMp = 0x0c, kMpVideoVar = 0x08;

// ---- RPMh: MMCX and MXC at nominal, MM1 enabled; raised once, never lowered ----
bool vote()
{
    if (v.voted) return true;
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    Window dbw = {};
    bool ok = db && mapWindow(dbw, kCmdDb, kCmdDbSize) && (v.rsc.va || mapWindow(v.rsc, kRsc, 0x10000));
    if (ok) {
        for (uint32_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = rd(dbw, i); memcpy(db + i, &w, 4);}
        const QcomRscLayout layout = qcom_rsc_layout(rd(v.rsc, 0));
        QcomRpmhResource mmcx, mxc, mm1;
        uint32_t mmcxLevel = 0, mxcLevel = 0;
        if (!qcom_cmd_db_find(db, kCmdDbSize, "mmcx.lvl", &mmcx) && !qcom_rpmh_level_index(&mmcx, kLevelNominal, &mmcxLevel))
            native_video_stats[14] |= 1;
        if (!qcom_cmd_db_find(db, kCmdDbSize, "mxc.lvl", &mxc) && !qcom_rpmh_level_index(&mxc, kLevelNominal, &mxcLevel))
            native_video_stats[14] |= 2;
        if (!qcom_cmd_db_find(db, kCmdDbSize, "MM1", &mm1)) native_video_stats[14] |= 4;
        ok = native_video_stats[14] == 7;
        AppsSmmuLock guard;
        auto send = [&](uint32_t address, uint32_t value) {
            QcomRpmhWrite writes[16];
            const unsigned n = qcom_rpmh_active_request(&layout, kOwnedTcs, &address, &value, 1, writes, 16);
            for (unsigned i = 0; i < n; ++i) wr(v.rsc, kRscTcs + writes[i].offset, writes[i].value);
            IOSleep(2);
            const unsigned m = qcom_rpmh_finish(&layout, kOwnedTcs, writes, 16);
            for (unsigned i = 0; i < m; ++i) wr(v.rsc, kRscTcs + writes[i].offset, writes[i].value);
        };
        if (ok) {
            send(mxc.address, mxcLevel);
            send(mmcx.address, mmcxLevel);
            // bcm_mm1 has an enable mask: BCM_TCS_CMD(commit, valid, vote_x 0, vote_y enable_mask 1)
            send(mm1.address, 1u << 30 | 1u << 29 | 1u);
        }
    }
    unmap(dbw);
    if (db) IOFreeData(db, kCmdDbSize);
    v.voted = ok;
    return ok;
}

// ---- the core's platform ops ----
int powerOnController(void *)
{
    if (!vote()) return -EIO;
    if (!sm8550_video_cc_configure(&v.ccIo, &v.cc, kMvs0Hz) || !sm8550_video_cc_controller_on(&v.ccIo, &v.cc)) {
        native_video_stats[11] = (int32_t)(v.cc.failed_step | v.cc.failed_value << 8);
        return -EIO;
    }
    return 0;
}
int powerOnHardware(void *)
{
    if (sm8550_video_cc_hardware_on(&v.ccIo, &v.cc)) return 0;
    native_video_stats[11] = (int32_t)(v.cc.failed_step | v.cc.failed_value << 8);
    return -EIO;
}
int setHwmode(void *) {return sm8550_video_cc_hardware_mode(&v.ccIo, &v.cc, true) ? 0 : -EIO;}
void powerOffHardware(void *)
{
    (void)sm8550_video_cc_hardware_mode(&v.ccIo, &v.cc, false);
    sm8550_video_cc_hardware_off(&v.ccIo, &v.cc);
}
void powerOffController(void *) {sm8550_video_cc_controller_off(&v.ccIo, &v.cc);}

int fileRead(void *context, uint64_t offset, void *to, size_t length) {return sheng_fw_read(context, offset, to, length);}
// iris_load_fw_to_memory (qcom_mdt_load: metadata, memory setup, segments) and iris_fw_load (authentication, content protection)
int loadFirmwareImage()
{
    if (native_gpu_addon_stage < 1) return -EAGAIN;
    void *file = nullptr;
    uint64_t size = 0;
    int rc = sheng_fw_open(kFirmware, &file, &size);
    if (rc) return -rc;
    QcomMdtImage *image = IOMallocType(QcomMdtImage);
    IOBufferMemoryDescriptor *meta = nullptr;
    Window region = {};
    rc = qcom_mdt_parse(image, size, fileRead, file) ? -EINVAL : 0;
    if (!rc && qcom_mdt_span(image) > kCarveoutSize) rc = -EINVAL;
    uint64_t bytes = rc ? 0 : qcom_mdt_metadata_size(image);
    if (!rc) {
        meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut |
            (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes, 0xfffffffffffff000ull);
        if (!meta || meta->prepare() != kIOReturnSuccess) rc = -ENOMEM;
    }
    if (!rc) {
        uint8_t *va = static_cast<uint8_t *>(meta->getBytesNoCopy());
        if (qcom_mdt_metadata(image, fileRead, file, va, bytes)) rc = -EIO;
        else cleanToPoc(va, bytes);
    }
    if (!rc && !scmOk(scm(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_INIT_IMAGE, 2, QCOM_IRIS_PAS_ID,
                          meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone), 0, 0, 1)))
        rc = -EIO;
    if (!rc && image->relocatable &&
        !scmOk(scm(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_MEM_SETUP, 3, QCOM_IRIS_PAS_ID, kCarveout, qcom_mdt_span(image), 0)))
        rc = -EIO;
    bool initialized = !rc;
    if (!rc && !mapWindow(region, kCarveout, kCarveoutSize, kIOMapWriteCombineCache)) rc = -ENOMEM;
    constexpr size_t kChunk = 256 * 1024;
    uint8_t *bounce = rc ? nullptr : static_cast<uint8_t *>(IOMallocData(kChunk));
    if (!rc && !bounce) rc = -ENOMEM;
    for (unsigned i = 0; !rc && i < image->count; ++i) {
        const QcomMdtSegment &s = image->segment[i];
        uint64_t at = 0;
        if (!s.load) continue;
        if (qcom_mdt_place(image, i, kCarveout, kCarveoutSize, &at)) {rc = -EINVAL; break;}
        uint8_t *to = const_cast<uint8_t *>(region.va) + at;
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (sheng_fw_read(file, s.file_offset + done, bounce, n)) {rc = -EIO; break;}
            memcpy(to + done, bounce, n);
            done += n;
        }
        if (!rc && s.memory_size > s.file_size) bzero(to + s.file_size, (size_t)(s.memory_size - s.file_size));
    }
    barrier();
    if (bounce) IOFreeData(bounce, kChunk);
    unmap(region);
    if (!rc && !scmOk(scm(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_AUTH_AND_RESET, 1, QCOM_IRIS_PAS_ID, 0, 0, 0))) rc = -EIO;
    // tz_cp_config_vpu3
    if (!rc && !scmOk(scm(kSvcMp, kMpVideoVar, 4, 0, 0x25800000, 0x01000000, 0x24800000))) {
        scm(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_SHUTDOWN, 1, QCOM_IRIS_PAS_ID, 0, 0, 0);
        rc = -EIO;
    } else if (rc && initialized) {
        scm(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_SHUTDOWN, 1, QCOM_IRIS_PAS_ID, 0, 0, 0);
    }
    if (meta) {meta->complete(); meta->release();}
    IOFreeType(image, QcomMdtImage);
    sheng_fw_close(file);
    return rc;
}
// With the lock held throughout, as iris_core_init holds core->lock: no session sees an initializing core half loaded.
int loadFirmware(void *)
{
    const int rc = loadFirmwareImage();
    native_video_stats[10] = rc;
    return rc;
}
void unloadFirmware(void *) {scm(QCOM_SCM_SVC_PIL, QCOM_SCM_PAS_SHUTDOWN, 1, QCOM_IRIS_PAS_ID, 0, 0, 0);}
int setRemoteState(void *, bool resume) {return scmOk(scm(kSvcBoot, kBootSetRemoteState, 2, resume ? 1 : 0, 0, 0, 0)) ? 0 : -EIO;}

// The line's disable depth, as enable_irq / disable_irq count it: masked while it is above zero.
void irqDepth(int change)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(v.irqLock);
    const unsigned before = v.irqDepth;
    v.irqDepth = change > 0 ? before + 1 : before ? before - 1 : 0;
    if (!before && v.irqDepth) armcpu_spi_mask(kIntid, true);
    else if (before && !v.irqDepth) armcpu_spi_mask(kIntid, false);
    IOSimpleLockUnlockEnableInterrupt(v.irqLock, state);
}
void enableIrq(void *) {irqDepth(-1);}
void disableIrq(void *) {irqDepth(1);}
// iris_hfi_isr: disable_irq_nosync, then the thread
void primary(void *)
{
    irqDepth(1);
    native_video_stats[3] = native_video_stats[3] + 1;
    __atomic_store_n(&v.irqPending, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&v.irqPending);
}
bool waitInit(void *, unsigned ms)
{
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    while (!v.initSignalled && mach_absolute_time() < deadline) IOLockSleepDeadline(v.lock, &v.initSignalled, deadline, THREAD_UNINT);
    const bool signalled = v.initSignalled;
    v.initSignalled = false;
    return signalled;
}
void signalInit(void *) {v.initSignalled = true; IOLockWakeup(v.lock, &v.initSignalled, false);}
void scheduleWork(uint64_t *slot, unsigned ms)
{
    *slot = nowMs() + ms;
    if (!*slot) *slot = 1;
    __atomic_store_n(&v.workPending, 1u, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&v.workPending);
}
void scheduleRecovery(void *, unsigned ms) {scheduleWork(&v.recoveryAt, ms);}
void scheduleSuspend(void *, unsigned ms) {scheduleWork(&v.suspendAt, ms);}
uint64_t nowOp(void *) {return nowMs();}
void logText(void *, const char *text)
{
    if (!strncmp(text, "video-firmware", 14) || !strncmp(text, "vfw", 3)) strlcpy(native_video_text, text, sizeof native_video_text);
    else {
        const uint32_t n = native_video_fw_lines;
        strlcpy(native_video_fw_text[n & 3], text, sizeof native_video_fw_text[0]);
        native_video_fw_lines = n + 1;
        if (!strncmp(text, "<VFW_E", 6) && !strnstr(text, "ParseC2SysPacket", 120)) {   /* not its version and licence lines */
            const uint32_t e = native_video_fw_errors;
            strlcpy(native_video_fw_error[e < 2 ? e : 2 + (e & 3)], text, sizeof native_video_fw_error[0]);
            native_video_fw_errors = e + 1;
        }
        IOLog("[video] %s\n", text);
    }
}
const struct qcom_iris_core_ops kOps = {nullptr, powerOnController, powerOnHardware, setHwmode, powerOffHardware, powerOffController,
    loadFirmware, unloadFirmware, setRemoteState, enableIrq, disableIrq, waitInit, signalInit, scheduleRecovery, scheduleSuspend, nowOp,
    logText};

void note()
{
    native_video_stats[5] = (int32_t)v.core.state;
    native_video_stats[6] = (int32_t)v.core.last_error;
    native_video_stats[7] = (int32_t)v.core.recoveries;
    native_video_stats[15] = (int32_t)v.mmu.last_status;
    native_video_stats[16] = (int32_t)(v.mmu.iova.allocated / 4096);
}

// iris_hfi_isr_handler
void irqThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "sheng-video-irq");
    for (;;) {
        assert_wait((event_t)&v.irqPending, THREAD_UNINT);
        if (__atomic_exchange_n(&v.irqPending, 0u, __ATOMIC_ACQ_REL)) clear_wait(current_thread(), THREAD_AWAKENED);
        else {thread_block(THREAD_CONTINUE_NULL); __atomic_store_n(&v.irqPending, 0u, __ATOMIC_RELEASE);}
        IOLockLock(v.lock);
        const bool enable = qcom_iris_core_thread(&v.core);
        native_video_stats[4] = native_video_stats[4] + 1;
        note();
        IOLockUnlock(v.lock);
        if (enable) irqDepth(-1);
    }
}
// the deferred work: autosuspend (pm_runtime) and the system error handler
void workThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "sheng-video-work");
    for (;;) {
        IOLockLock(v.lock);
        const uint64_t now = nowMs();
        if (v.recoveryAt && now >= v.recoveryAt) {
            v.recoveryAt = 0;
            qcom_iris_core_recover(&v.core);
        }
        if (v.suspendAt && nowMs() >= v.suspendAt) {
            v.suspendAt = 0;
            const bool was = v.core.suspended;
            qcom_iris_core_autosuspend(&v.core);
            if (!was && v.core.suspended) native_video_stats[8] = native_video_stats[8] + 1;
        }
        note();
        uint64_t next = UINT64_MAX;
        if (v.recoveryAt && v.recoveryAt < next) next = v.recoveryAt;
        if (v.suspendAt && v.suspendAt < next) next = v.suspendAt;
        IOLockUnlock(v.lock);
        if (next == UINT64_MAX) assert_wait((event_t)&v.workPending, THREAD_UNINT);
        else {
            const uint64_t t = nowMs();
            uint64_t deadline;
            clock_interval_to_deadline(next > t ? (uint32_t)(next - t) : 0, kMillisecondScale, &deadline);
            assert_wait_deadline((event_t)&v.workPending, THREAD_UNINT, deadline);
        }
        if (__atomic_exchange_n(&v.workPending, 0u, __ATOMIC_ACQ_REL)) clear_wait(current_thread(), THREAD_AWAKENED);
        else {thread_block(THREAD_CONTINUE_NULL); __atomic_store_n(&v.workPending, 0u, __ATOMIC_RELEASE);}
    }
}

// ---- memory for the VPU ----
void *hostAllocate(void *, size_t bytes) {return IOMallocData(bytes);}
void hostRelease(void *, void *memory, size_t bytes) {IOFreeData(memory, bytes);}

int mapPrepared(IOMemoryDescriptor *memory, uint64_t first, uint64_t end, uint32_t align, uint32_t *iova)
{
    size_t count = 0;
    for (uint64_t at = first; at < end;) {
        IOByteCount length = 0;
        const addr64_t pa = memory->getPhysicalSegment(at, &length, kIOMemoryMapperNone);
        if (!pa || (pa & 4095) || !length) return -EFAULT;
        if (pa + length > (UINT64_C(1) << v.mmu.arena.config.output_bits)) return -EFAULT;   /* past what the VPU's table reaches */
        at += length > end - at ? end - at : length;
        ++count;
    }
    struct qcom_iris_segment *segments = IONewData(struct qcom_iris_segment, count);
    if (!segments) return -ENOMEM;
    size_t n = 0;
    for (uint64_t at = first; at < end; ++n) {
        IOByteCount length = 0;
        const addr64_t pa = memory->getPhysicalSegment(at, &length, kIOMemoryMapperNone);
        const uint64_t take = length > end - at ? end - at : length;
        segments[n] = {pa, (take + 4095) & ~UINT64_C(4095)};
        at += take;
    }
    const int rc = qcom_iris_mmu_map(&v.mmu, segments, count, align, iova);
    IODeleteData(segments, struct qcom_iris_segment, count);
    return rc;
}
}  // namespace

bool QcomIrisDeviceReady(void) {return __atomic_load_n(&v.ready, __ATOMIC_ACQUIRE);}
IOLock *QcomIrisDeviceLock(void) {return v.lock;}
struct qcom_iris_core *QcomIrisDeviceCore(void) {return QcomIrisDeviceReady() ? &v.core : nullptr;}

int QcomIrisDeviceAllocate(uint64_t bytes, QcomIrisBuffer *b)
{
    *b = {};
    bytes = (bytes + 4095) & ~UINT64_C(4095);
    const uint64_t limit = UINT64_C(1) << v.mmu.arena.config.output_bits;
    /* A physical mask makes the kernel allocate the buffer as one contiguous run, which a 4K stream's buffers (50 to 130 MB each)
     * did not get (run123: the capture start failed with ENOMEM). The VPU's table reaches at least 64 GiB of physical addresses on
     * this SoC, above all of its memory, so the buffer needs no mask and may lie in scattered pages; a smaller reach keeps it. */
    /* Pageable memory that prepare() wires: wiring waits for pages and reclaims the file cache for them, where a wired kernel
     * allocation is refused at once when the free list is short. The system runs with tens of MiB free, and a 16.8 MiB reference
     * picture was refused that way (run125: "[video alloc] refused 3 pages 4106 step 1"). The GPU's buffers are made the same way. */
    IOBufferMemoryDescriptor *memory = limit >= (UINT64_C(1) << 36)
        ? IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPageable, bytes, 4096)
        : IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut, bytes, (limit - 1) & ~UINT64_C(4095));
    const auto refused = [&](int step, int result) {
        native_video_alloc[0] = native_video_alloc[0] + 1; native_video_alloc[1] = (int32_t)(bytes >> 12);
        native_video_alloc[2] = step; native_video_alloc[3] = result; native_video_alloc[4] = (int32_t)v.mmu.arena.config.output_bits;
    };
    if (!memory) {refused(1, 0); return -ENOMEM;}
    if (const IOReturn wired = memory->prepare(); wired != kIOReturnSuccess) {refused(2, (int)wired); memory->release(); return -ENOMEM;}
    bzero(memory->getBytesNoCopy(), (size_t)bytes);
    uint32_t iova = 0;
    const int rc = mapPrepared(memory, 0, bytes, 4096, &iova);
    if (rc) {refused(3, rc); memory->complete(); memory->release(); return rc;}
    *b = {memory, iova, 0, bytes, memory->getBytesNoCopy(), nullptr};
    native_video_stats[17] = native_video_stats[17] + 1;
    return 0;
}

int QcomIrisDeviceMap(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, QcomIrisBuffer *b)
{
    *b = {};
    if (!memory || !length || offset + length > memory->getLength() || offset + length < offset) return -EINVAL;
    const uint64_t first = offset & ~UINT64_C(4095), end = (offset + length + 4095) & ~UINT64_C(4095);
    if (memory->prepare(kIODirectionInOut) != kIOReturnSuccess) return -ENOMEM;
    memory->retain();
    uint32_t iova = 0;
    const int rc = mapPrepared(memory, first, end > memory->getLength() ? memory->getLength() : end, 4096, &iova);
    if (rc) {memory->complete(kIODirectionInOut); memory->release(); return rc;}
    *b = {memory, iova, first, end - first, nullptr, nullptr};
    native_video_stats[17] = native_video_stats[17] + 1;
    return 0;
}

void QcomIrisDeviceRelease(QcomIrisBuffer *b)
{
    if (!b->memory) return;
    // A buffer whose translation could not be removed stays wired and referenced: the VPU may still reach it.
    if (qcom_iris_mmu_unmap(&v.mmu, b->iova, (b->bytes + 4095) & ~UINT64_C(4095))) {*b = {}; return;}
    b->memory->complete(kIODirectionInOut);
    b->memory->release();
    native_video_stats[17] = native_video_stats[17] - 1;
    *b = {};
}

int QcomIrisDeviceSend(const void *message, uint32_t size) {return qcom_iris_core_send(&v.core, message, size);}
bool QcomIrisDeviceWait(struct qcom_iris_session *s, unsigned ms)
{
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    while (!s->responded && s->state != QcomIrisError && mach_absolute_time() < deadline)
        IOLockSleepDeadline(v.lock, s, deadline, THREAD_UNINT);
    return s->responded;
}
void QcomIrisDeviceSignal(struct qcom_iris_session *s) {IOLockWakeup(v.lock, s, false);}
int QcomIrisDeviceAllocateInternal(uint32_t bytes, uint64_t *device_addr, void **handle)
{
    QcomIrisBuffer *b = IOMallocType(QcomIrisBuffer);
    const int rc = QcomIrisDeviceAllocate(bytes, b);
    if (rc) {IOFreeType(b, QcomIrisBuffer); return rc;}
    *device_addr = b->iova;
    *handle = b;
    return 0;
}
void QcomIrisDeviceReleaseInternal(void *handle)
{
    auto *b = static_cast<QcomIrisBuffer *>(handle);
    if (!b) return;
    QcomIrisDeviceRelease(b);
    IOFreeType(b, QcomIrisBuffer);
}

namespace {
// iris_probe without the video devices: the registers, the domain, the queues, the interrupt and the threads; the core powers on
// at the first session (iris_core_init).
bool start()
{
    if (!mapWindow(v.vpu, kVpu, QCOM_IRIS_SIZE) || !mapWindow(v.gcc, SM8550_GCC_BASE, SM8550_GCC_SIZE) ||
        !mapWindow(v.videocc, SM8550_VIDEO_CC_BASE, SM8550_VIDEO_CC_SIZE) || !mapWindow(v.smmu, SM8550_APPS_SMMU_BASE, SM8550_APPS_SMMU_SIZE))
        {fail(StMapped, -ENOMEM); return false;}
    v.lock = IOLockAlloc();
    v.irqLock = IOSimpleLockAlloc();
    if (!v.lock || !v.irqLock) {fail(StMapped, -ENOMEM); return false;}
    v.vpuIo = {nullptr, vpuRead, vpuWrite, delayUs};
    v.ccIo = {nullptr, ccRead, ccWrite, delayUs};
    native_video_stats[0] = StMapped;

    // The domain: bank, ASID and slots picked, the empty table prepared, the bank programmed and enabled, then the streams routed.
    {
        AppsSmmuLock guard;
        if (!guard.ready()) {fail(StDomain, -EBUSY); return false;}
        const struct sm8550_apps_smmu_io io = {nullptr, smmuRead, smmuWrite};
        int rc = sm8550_apps_smmu_pick(&io, kSids, 2, &v.domain);
        if (rc) {fail(StDomain, rc); return false;}
        static const uint8_t oas[6] = {32, 36, 40, 42, 44, 48};
        const unsigned code = (v.domain.id2 >> 4) & 15, output = code < 6 ? oas[code] : 0;
        if (!output) {fail(StDomain, -ENOTSUP); return false;}
        v.tables.limit = UINT64_C(1) << output;
        v.tables.provider = {&v.tables, TablePages::allocate, TablePages::find, TablePages::overlaps};
        const struct qcom_iris_mmu_memory memory = {nullptr, hostAllocate, hostRelease};
        const ArmSmmu4KLiveVisibility visibility = {nullptr, publishTable};
        rc = qcom_iris_mmu_prepare(&v.mmu, &memory, &v.tables.provider, &visibility, output);
        const ArmSmmu4KIo kit = {nullptr, (size_t)SM8550_APPS_SMMU_SIZE, kitRead32, kitRead64, kitWrite32, kitWrite64, kitBarrier};
        if (!rc) rc = qcom_iris_mmu_program(&v.mmu, &kit, v.domain.bank, v.domain.asid, kPollBudget);
        if (!rc) rc = sm8550_apps_smmu_route(&io, &v.domain);
        native_video_stats[2] = (int32_t)(v.domain.bank | v.domain.asid << 8 | output << 24);
        native_video_stats[15] = (int32_t)v.mmu.last_status;
        if (rc) {fail(StDomain, rc); return false;}
    }
    native_video_stats[0] = StDomain;

    // The UC region: the queue table and queues, then the SFR, in one region of the size the firmware is told (1 MiB aligned).
    const uint32_t region = qcom_iris_uc_region_size();
    {
        IOBufferMemoryDescriptor *memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut,
            region, ((UINT64_C(1) << v.mmu.arena.config.output_bits) - 1) & ~UINT64_C(4095));
        if (!memory || memory->prepare() != kIOReturnSuccess) {if (memory) memory->release(); fail(StQueues, -ENOMEM); return false;}
        bzero(memory->getBytesNoCopy(), region);
        uint32_t iova = 0;
        const int rc = mapPrepared(memory, 0, region, 1u << 20, &iova);
        if (rc) {memory->complete(); memory->release(); fail(StQueues, rc); return false;}
        v.uc = {memory, iova, 0, region, memory->getBytesNoCopy(), nullptr};
    }
    uint8_t *base = static_cast<uint8_t *>(v.uc.cpu);
    const struct qcom_iris_queues queues = {base, v.uc.iova};
    qcom_iris_core_setup(&v.core, &kOps, &v.vpuIo, &queues, reinterpret_cast<volatile uint32_t *>(base + QCOM_IRIS_QUEUE_BYTES),
                         v.uc.iova + (uint32_t)QCOM_IRIS_QUEUE_BYTES, 4);
    {   /* log-video=1: the firmware writes its error, fatal and high messages to the debug queue ([video fw] rows). */
        uint32_t logging = 0;
        if (PE_parse_boot_argn("log-video", &logging, sizeof logging) && logging) v.core.debug_level = 1 | 2 | 8;
    }
    native_video_stats[0] = StQueues;

    // Requested disabled, as the line stays until the first power-on enables it.
    v.irqDepth = 1;
    if (!armcpu_spi_attach || !armcpu_spi_mask || armcpu_spi_attach(kIntid, false, primary, nullptr, nullptr, nullptr))
        {fail(StInterrupt, -EIO); return false;}
    armcpu_spi_mask(kIntid, true);
    native_video_stats[0] = StInterrupt;

    thread_t thread = nullptr;
    if (kernel_thread_start(irqThread, nullptr, &thread) != KERN_SUCCESS) {fail(StThreads, -ENOMEM); return false;}
    thread_deallocate(thread);
    if (kernel_thread_start(workThread, nullptr, &thread) != KERN_SUCCESS) {fail(StThreads, -ENOMEM); return false;}
    thread_deallocate(thread);
    __atomic_store_n(&v.ready, true, __ATOMIC_RELEASE);
    native_video_stats[0] = StReady;
    return true;
}
}  // namespace

extern "C" void qcom_iris_video_publish(void) __attribute__((weak_import));
extern "C" void sheng_video_probe(void)
{
    unsigned enabled = 1;   /* sheng-video=0 leaves the VPU off and VideoToolbox its own codecs */
    PE_parse_boot_argn("sheng-video", &enabled, sizeof enabled);
    if (!enabled) return;
    if (start() && qcom_iris_video_publish) qcom_iris_video_publish();
}
