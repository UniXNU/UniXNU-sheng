// SPDX-License-Identifier: BSD-2-Clause-Patent
// The audio DSP (ADSP, LPASS) of the sheng board: boot argument sheng-adsp=1 surveys what the boot stages left (SMEM partition and items,
// the DSP's SMP2P state, the AOSS mailbox, the RSC, the command database) and writes nothing; sheng-adsp=2 starts the DSP and brings up
// GLINK so the battery manager (PMIC GLINK) and the audio services (GPR) can talk to it.
//
// Devicetree facts: remoteproc@6800000 (qcom,sm8550-adsp-pas), PAS id 1 for adsp.mbn in adspslpi 0x9ea00000 + 0x4680000 and id 36 for
// adsp_dtb.mbn in q6-adsp-dtb 0x9e980000 + 0x80000; proxy power domains lcx and lmx (RPMh ARC lcx.lvl, lmx.lvl) and the AOSS load_state
// "adsp"; SMEM 0x81d00000 + 0x200000 with TCSR mutex 3 (0x1f40000, stride 0x1000); SMP2P items 443 (in) and 429 (out), remote pid 2,
// entries "master-kernel" (bit 0 stop) and "slave-kernel" (fatal 0, ready 1, handover 2, stop ack 3); IPCC 0x408000 (SPI 229): LPASS
// client 3 with signal 0 GLINK and 2 SMP2P, AOP client 0 signal 0; GLINK items 478 (descriptor), 479 (our FIFO), 480 (the DSP's FIFO);
// AOSS message RAM 0xc300000 + 0x400; apps RSC DRV 2 at 0x17a20000 with TCS block at +0xd00 and its interrupt SPI 5; cmd-db 0x81c60000.
//
// Design: one kernel thread owns every step and all protocol state. Interrupt handlers only acknowledge their hardware, record which
// doorbell rang and wake the thread; the thread waits on that event with a deadline for each step (QMP 1 s per phase, RPMh 100 ms, DSP
// ready 5 s) and then sleeps until the next doorbell or client send. Nothing spins on a register in a loop, and no lock is held across a
// wait. Clients (ShengAdsp.h) register channels and queue sends from any thread; their callbacks run on this thread.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "QcomGlink.h"
#include "QcomMdt.h"
#include "QcomQmp.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"
#include "ShengAdsp.h"
int sheng_fw_open(const char *path, void **handle, uint64_t *size);
void sheng_qrtr_start(void);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);
}

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
// [0] stage (see Stage; negative: the step that failed), [1] last error, [2] IPCC interrupts, [3] the DSP's slave-kernel bits (-1 no
// item), [4] items (bit 0 SMP2P in 443, 1 out 429, 2 GLINK 478, 3 479, 4 480, 8 our partition, 9 global partition), [5] QMP state | AOSS
// link bits << 8, [6] RSC DRV id, [7] adsp.mbn KiB, [8] ms from the start call to "ready", [9] GLINK messages in, [10] out, [11] channels
// up (bit per client) | version agreed << 14 | link broken << 15, [12] channel opens nobody serves, [13] last SCM x0, [14] its x1,
// [15] votes: lcx index | lmx index << 8 | RSC done interrupts << 16
extern "C" {volatile int32_t native_adsp_stats[16]; char native_adsp_text[96];}

namespace {
enum Stage {
    StMapped = 1, StSurveyed = 2, StFirmware = 3, StSmp2p = 4, StDoorbells = 5, StLoadState = 6, StRails = 7, StDtb = 8, StLoaded = 9,
    StStarted = 10, StReady = 11, StGlink = 12, StChannel = 13,
};
constexpr uint64_t kSmem = 0x81d00000, kSmemSize = 0x200000, kMutex = 0x1f40000 + 3 * 0x1000, kIpcc = 0x408000, kAoss = 0xc300000;
constexpr uint64_t kRscDrv = 0x17a20000, kRscTcs = 0xd00, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;
constexpr uint64_t kAdspBase = 0x9ea00000, kAdspSize = 0x4680000, kDtbBase = 0x9e980000, kDtbSize = 0x80000;
constexpr uint32_t kPasAdsp = 1, kPasDtb = 36, kRemotePid = 2, kItemIn = 443, kItemOut = 429, kItemCrash = 423;
constexpr uint32_t kItemGlinkDesc = 478, kItemGlinkTx = 479, kItemGlinkRx = 480, kGlinkFifo = 0x4000;
constexpr uint32_t kSpiIpcc = 229, kSpiRsc = 5;
constexpr uint32_t kFatal = 1u << 0, kReady = 1u << 1, kHandover = 1u << 2;
constexpr uint32_t kEvSmp2p = 1, kEvGlink = 2, kEvAop = 4, kEvRsc = 8, kEvSend = 16;
constexpr unsigned kClients = 4, kQueue = 8;

struct Window {IOMemoryMap *map; volatile uint8_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
uint32_t rd(const Window &w, uint64_t at) {return *reinterpret_cast<volatile uint32_t *>(w.va + at);}
void wr(const Window &w, uint64_t at, uint32_t value) {*reinterpret_cast<volatile uint32_t *>(w.va + at) = value; __asm__ volatile("dsb sy" ::: "memory");}
void fail(int step, int error) {native_adsp_stats[1] = error; native_adsp_stats[0] = -step;}
void stage(int value) {native_adsp_stats[0] = value;}
void note(const char *prefix, const char *text) {
    size_t at = 0;
    for (; prefix[at] && at < sizeof native_adsp_text - 1; ++at) native_adsp_text[at] = prefix[at];
    for (size_t i = 0; text[i] && at < sizeof native_adsp_text - 1; ++i) native_adsp_text[at++] = text[i];
    native_adsp_text[at] = 0;
}

Window gSmemW, gMutex, gIpcc, gAoss, gRsc;
QcomSmem gSmem;
QcomSmemPartition gPart;
QcomSmp2p gP2p;
int gStopEntry = -1;
QcomQmp gQmp;
QcomGlink gGlink;
bool gGlinkStarted;
QcomRscLayout gLayout;
uint32_t gRscIrqBits;
volatile uint32_t gPending, gRscDone;

// ---- clients ----
struct Message {uint32_t length; uint8_t data[SHENG_ADSP_MAX_MESSAGE];};
}  // namespace
struct ShengAdspChannel {
    char name[QCOM_GLINK_NAME];
    uint32_t intent_size, intent_count;
    int initiator;
    ShengAdspClient client;
    void *context;
    uint8_t *storage;
    QcomGlinkChannel *glink;
    Message *queue;  // kQueue slots
    uint32_t head, count;
};
namespace {
ShengAdspChannel gClients[kClients];
uint32_t gClientCount;
IOLock *gClientLock;

void wake(uint32_t bits) {
    __atomic_fetch_or(&gPending, bits, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}

// ---- interrupts: acknowledge, record, wake ----
void ipccIrq(void *) {
    uint32_t bits = 0;
    for (unsigned n = 0; n < 32; ++n) {
        const uint32_t id = rd(gIpcc, QCOM_IPCC_RECV_ID);
        if (id == QCOM_IPCC_NONE) break;
        wr(gIpcc, QCOM_IPCC_RECV_CLEAR, id);
        if (id == qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_SMP2P)) bits |= kEvSmp2p;
        else if (id == qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_GLINK_QMP)) bits |= kEvGlink;
        else if (id == qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP)) bits |= kEvAop;
    }
    native_adsp_stats[2] = native_adsp_stats[2] + 1;
    if (bits) wake(bits);
}
void rscIrq(void *) {
    const uint32_t status = rd(gRsc, kRscTcs + gLayout.irq_status) & gRscIrqBits;
    if (!status) return;
    wr(gRsc, kRscTcs + gLayout.irq_clear, status);
    __atomic_fetch_or(&gRscDone, status, __ATOMIC_RELEASE);
    native_adsp_stats[15] = native_adsp_stats[15] + (1 << 16);
    wake(kEvRsc);
}

// Waits for an event up to ms milliseconds; returns the event bits (0: the deadline passed).
uint32_t waitEvents(uint32_t ms) {
    uint32_t bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
    if (bits) return bits;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms, kMillisecondScale);
    bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
    if (bits) {clear_wait(current_thread(), THREAD_AWAKENED); return bits;}
    thread_block(THREAD_CONTINUE_NULL);
    return __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
}

void ring(uint32_t client, uint32_t signal) {wr(gIpcc, QCOM_IPCC_SEND, qcom_ipcc_id(client, signal));}
void kickAop(void *) {ring(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP);}
void kickGlink(void *) {ring(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_GLINK_QMP);}
void kickSmp2p() {ring(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_SMP2P);}

// ---- SMEM helpers (allocation under the TCSR hardware mutex) ----
bool lockSmem() {
    // The mutex is held only for the few writes of one allocation by whichever processor has it; it is tried a bounded number of times.
    for (unsigned i = 0; i < 1000; ++i) {
        wr(gMutex, 0, 1);
        if (rd(gMutex, 0) == 1) return true;
        IODelay(10);
    }
    return false;
}
void unlockSmem() {wr(gMutex, 0, 0);}
int allocItem(uint16_t item, uint32_t size) {
    if (!lockSmem()) return -16;
    const int rc = qcom_smem_alloc(&gSmem, &gPart, item, size);
    unlockSmem();
    return rc == QcomSmemExists ? 0 : rc;
}
volatile uint8_t *item(uint16_t id, uint32_t *size) {
    uint32_t at = 0, bytes = 0;
    if (qcom_smem_find(&gSmem, &gPart, id, &at, &bytes)) return nullptr;
    if (size) *size = bytes;
    return gSmemW.va + at;
}

void readCrashReason() {
    QcomSmemPartition global;
    uint32_t at = 0, size = 0;
    if (qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global) ||
        qcom_smem_find(&gSmem, &global, kItemCrash, &at, &size) || !size)
        return;
    char text[80];
    size_t n = 0;
    for (; n < sizeof text - 1 && n < size; ++n) {const char c = (char)gSmemW.va[at + n]; if (!c) break; text[n] = c >= 32 && c < 127 ? c : '.';}
    text[n] = 0;
    if (n) note("crash: ", text);
}

// ---- the survey (sheng-adsp=1 and the first step of 2): reads only ----
void survey() {
    int32_t bits = 0;
    if (!qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_APPS, kRemotePid, &gPart)) {
        bits |= 1 << 8;
        const uint16_t ids[5] = {kItemIn, kItemOut, kItemGlinkDesc, kItemGlinkTx, kItemGlinkRx};
        for (unsigned i = 0; i < 5; ++i) if (item(ids[i], nullptr)) bits |= 1 << i;
    }
    QcomSmemPartition global;
    if (!qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global)) bits |= 1 << 9;
    native_adsp_stats[4] = bits;
    native_adsp_stats[3] = -1;
    if (volatile uint8_t *in = item(kItemIn, nullptr)) {
        QcomSmp2p view = {};
        view.in = reinterpret_cast<volatile uint32_t *>(in);
        view.local_pid = 0; view.remote_pid = kRemotePid;
        const int index = qcom_smp2p_check_in(view.in, 0, kRemotePid) ? -1 : qcom_smp2p_find_in(&view, "slave-kernel");
        if (index >= 0) native_adsp_stats[3] = (int32_t)qcom_smp2p_in_value(&view, index);
    }
    const uint32_t up = 0xffff;
    native_adsp_stats[5] = (rd(gAoss, 0) == 0x4d41494cu ? 1 : 0) << 12 | (rd(gAoss, 0xc) == up) << 8 | (rd(gAoss, 0x24) == up) << 9 |
                           (rd(gAoss, 0x14) == up) << 10 | (rd(gAoss, 0x2c) == up) << 11;
    native_adsp_stats[6] = (int32_t)rd(gRsc, 0);
    readCrashReason();
}

void service(uint32_t bits);

// ---- RPMh proxy votes ----
bool readCmdDb(uint8_t *copy) {
    Window db = {};
    if (!mapWindow(db, kCmdDb, kCmdDbSize)) return false;
    for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = rd(db, i); memcpy(copy + i, &w, 4);}
    unmap(db);
    return true;
}
QcomRpmhResource gLcx, gLmx;
int gTcs = -1;

// Votes both rails to the given ARC index pair through active TCS 1 and waits up to 100 ms for its done interrupt. The active TCSs are 0
// to 2 (tcs-config lists the active type first); the WiFi bring-up sends through TCS 0, so the two never share one. A request whose done
// interrupt is not seen still counts (bit 15 of [15]): the WiFi rails show such requests take effect.
int vote(uint32_t lcx, uint32_t lmx) {
    if (gTcs < 0) {
        gTcs = 1;
        gRscIrqBits = 1u << gTcs;
        wr(gRsc, kRscTcs + 0, rd(gRsc, kRscTcs + 0) | gRscIrqBits);  // IRQ_ENABLE for our TCS
    }
    const uint32_t addresses[2] = {gLcx.address, gLmx.address}, values[2] = {lcx, lmx};
    QcomRpmhWrite writes[16];
    const unsigned n = qcom_rpmh_active_request(&gLayout, (unsigned)gTcs, addresses, values, 2, writes, 16);
    if (!n) return -22;
    __atomic_store_n(&gRscDone, 0, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < n; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
    uint64_t deadline;
    clock_interval_to_deadline(100, kMillisecondScale, &deadline);
    while (!(__atomic_load_n(&gRscDone, __ATOMIC_ACQUIRE) & gRscIrqBits) && mach_absolute_time() < deadline) service(waitEvents(20));
    bool done = (__atomic_load_n(&gRscDone, __ATOMIC_ACQUIRE) & gRscIrqBits) != 0;
    if (!done && (rd(gRsc, kRscTcs + gLayout.irq_status) & gRscIrqBits)) done = true;  // one look in case the line is not ours
    const unsigned m = qcom_rpmh_finish(&gLayout, (unsigned)gTcs, writes, 16);
    for (unsigned i = 0; i < m; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
    native_adsp_stats[15] = (native_adsp_stats[15] & ~0xffff) | (int32_t)(lcx | lmx << 8 | (done ? 0 : 1u << 15));
    return 0;
}

// ---- secure world calls ----
struct ScmResult {int64_t x0; uint64_t x1;};
ScmResult scm(uint32_t command, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c) {
    ScmResult result = {-1, 0};
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t function = qcom_scm_fn(QCOM_SCM_SVC_PIL, command), session = 0;
        unsigned resumes = 0;
        for (;;) {
            register uint64_t x0 __asm__("x0") = function, x1 __asm__("x1") = arginfo, x2 __asm__("x2") = a, x3 __asm__("x3") = b,
                              x4 __asm__("x4") = c, x5 __asm__("x5") = 0, x6 __asm__("x6") = session, x7 __asm__("x7") = 0;
            __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                             :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
            result = {(int64_t)x0, x1};
            if ((int64_t)x0 != QCOM_SCM_INTERRUPTED || ++resumes > 100000) break;
            function = QCOM_SCM_INTERRUPTED;  // resume the interrupted call in its session
            session = x6;
        }
        if (result.x0 != QCOM_SCM_V2_EBUSY) break;
        IOSleep(30);
    }
    native_adsp_stats[13] = (int32_t)result.x0;
    native_adsp_stats[14] = (int32_t)result.x1;
    return result;
}
bool scmOk(const ScmResult &r) {return r.x0 == 0 && r.x1 == 0;}

void cleanToPoc(const void *start, size_t length) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- a firmware image: parsed from its file, metadata handed over, segments written into the carveout ----
struct Image {  // plain data, zero at start: the two live in static storage (a few KiB of header table each, kept off the stack)
    void *file;
    uint64_t size;
    QcomMdtImage mdt;
    IOBufferMemoryDescriptor *meta;
};
Image gDtbImage, gAdspImage;
int fileRead(void *context, uint64_t offset, void *to, size_t length) {return sheng_fw_read(context, offset, to, length);}
void release(Image &image) {
    if (image.file) sheng_fw_close(image.file);
    if (image.meta) image.meta->release();
    image.file = nullptr; image.meta = nullptr;
}
int openImage(Image &image, const char *path) {
    int rc = sheng_fw_open(path, &image.file, &image.size);
    if (rc) {image.file = nullptr; return rc;}
    rc = qcom_mdt_parse(&image.mdt, image.size, fileRead, image.file);
    return rc ? 1000 - rc : 0;
}
// init_image with the metadata in physically contiguous memory, then the memory setup of a relocatable image.
int authenticateHeader(Image &image, uint32_t pas, uint64_t base) {
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes,
                                                                 0xfffffffffffff000ull);
    if (!image.meta || image.meta->prepare() != kIOReturnSuccess) return -12;
    uint8_t *va = static_cast<uint8_t *>(image.meta->getBytesNoCopy());
    if (qcom_mdt_metadata(&image.mdt, fileRead, image.file, va, bytes)) return -5;
    cleanToPoc(va, bytes);
    const uint64_t phys = image.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    if (!scmOk(scm(QCOM_SCM_PAS_INIT_IMAGE, qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0), pas, phys, 0))) return -1;
    if (image.mdt.relocatable &&
        !scmOk(scm(QCOM_SCM_PAS_MEM_SETUP, qcom_scm_args(3, 0, 0, 0), pas, base, qcom_mdt_span(&image.mdt))))
        return -2;
    return 0;
}
int loadSegments(Image &image, uint64_t base, uint64_t size) {
    Window region = {};
    if (!mapWindow(region, base, size, kIOMapWriteCombineCache)) return -12;
    constexpr size_t kChunk = 256 * 1024;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kChunk));
    int rc = bounce ? 0 : -12;
    for (unsigned i = 0; !rc && i < image.mdt.count; ++i) {
        const QcomMdtSegment &s = image.mdt.segment[i];
        uint64_t at = 0;
        if (!s.load) continue;
        if (qcom_mdt_place(&image.mdt, i, base, size, &at)) {rc = -7; break;}
        uint8_t *to = const_cast<uint8_t *>(region.va) + at;
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (sheng_fw_read(image.file, s.file_offset + done, bounce, n)) {rc = -5; break;}
            memcpy(to + done, bounce, n);
            done += n;
        }
        if (!rc && s.memory_size > s.file_size) bzero(to + s.file_size, (size_t)(s.memory_size - s.file_size));
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (bounce) IOFreeData(bounce, kChunk);
    unmap(region);
    return rc;
}

// ---- events ----
uint32_t gSlave;
bool gHandedOver;
void serviceSmp2p() {
    if (!gP2p.in) {
        if (volatile uint8_t *in = item(kItemIn, nullptr)) gP2p.in = reinterpret_cast<volatile uint32_t *>(in);
        else return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gP2p, "slave-kernel", &value, &changed)) kickSmp2p();
    gSlave = value;
    native_adsp_stats[3] = (int32_t)value;
    if ((changed & value & kFatal)) readCrashReason();
    if ((value & kHandover) && !gHandedOver) {
        gHandedOver = true;
        vote(0, 0);  // the DSP holds its own votes now
    }
}

void applyClients();
void pumpSends();
void serviceGlink() {
    if (!gGlinkStarted) return;
    if (!gGlink.rx.data) {
        uint32_t size = 0;
        if (volatile uint8_t *rx = item(kItemGlinkRx, &size)) qcom_glink_set_rx(&gGlink, rx, size);
    }
    qcom_glink_poll(&gGlink);
    pumpSends();
    uint32_t up = 0;
    for (unsigned i = 0; i < gClientCount; ++i) if (gClients[i].glink && gClients[i].glink->up) up |= 1u << i;
    native_adsp_stats[9] = (int32_t)gGlink.rx_messages;
    native_adsp_stats[10] = (int32_t)gGlink.tx_messages;
    native_adsp_stats[11] = (int32_t)(up | (uint32_t)gGlink.version_agreed << 14 | (uint32_t)gGlink.broken << 15);
    native_adsp_stats[12] = (int32_t)gGlink.unknown_opens;
    if (gGlink.unknown_opens && strncmp(native_adsp_text, "crash: ", 7)) note("open: ", gGlink.unknown_names);
    if (gGlink.version_agreed && native_adsp_stats[0] >= StReady && native_adsp_stats[0] < StGlink) stage(StGlink);
    if (up && native_adsp_stats[0] >= StGlink) stage(StChannel);
}
void service(uint32_t bits) {
    if (bits & kEvAop) qcom_qmp_step(&gQmp);
    if (bits & (kEvSmp2p | kEvGlink)) serviceSmp2p();
    if (bits & (kEvGlink | kEvSend)) {applyClients(); serviceGlink();}
}
// Runs events until done() holds or ms pass; returns done().
template <typename F> bool waitUntil(uint32_t ms, F done) {
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    while (!done()) {
        const uint64_t now = mach_absolute_time();
        if (now >= deadline) return done();
        uint64_t left = 0;
        absolutetime_to_nanoseconds(deadline - now, &left);
        service(waitEvents((uint32_t)(left / 1000000) + 1));
    }
    return true;
}

// ---- client glue (DSP thread side) ----
void clientState(void *context, QcomGlinkChannel *, int up) {
    ShengAdspChannel *c = static_cast<ShengAdspChannel *>(context);
    if (c->client.state) c->client.state(c->context, up);
}
void clientReceive(void *context, QcomGlinkChannel *, const uint8_t *data, uint32_t length) {
    ShengAdspChannel *c = static_cast<ShengAdspChannel *>(context);
    if (c->client.receive) c->client.receive(c->context, data, length);
}
void applyClients() {
    if (!gGlinkStarted) return;
    const QcomGlinkClient glue = {clientState, clientReceive};
    IOLockLock(gClientLock);
    for (unsigned i = 0; i < gClientCount; ++i) {
        ShengAdspChannel &c = gClients[i];
        if (!c.glink) c.glink = qcom_glink_channel(&gGlink, c.name, c.intent_size, c.intent_count, c.storage, &glue, &c, c.initiator);
    }
    IOLockUnlock(gClientLock);
}
void pumpSends() {
    for (unsigned i = 0; i < gClientCount; ++i) {
        ShengAdspChannel &c = gClients[i];
        if (!c.glink || !c.glink->up) continue;
        for (;;) {
            IOLockLock(gClientLock);
            if (!c.count) {IOLockUnlock(gClientLock); break;}
            Message *m = &c.queue[c.head];
            IOLockUnlock(gClientLock);
            // Only this thread takes messages off the queue, so m stays valid while the lock is dropped for the send.
            if (qcom_glink_send(&gGlink, c.glink, m->data, m->length) != QcomGlinkOk) break;
            IOLockLock(gClientLock);
            c.head = (c.head + 1) % kQueue;
            c.count--;
            IOLockUnlock(gClientLock);
        }
    }
}

// ---- the bring-up ----
bool gIrq;
bool attachIrqs() {
    if (!armcpu_spi_attach) return false;
    int rc = 0;
    for (unsigned tries = 0;; ++tries) {  // the GIC driver may not have started yet (as ButtonProbe found)
        rc = armcpu_spi_attach(32 + kSpiIpcc, false, ipccIrq, nullptr, nullptr, nullptr);
        if (rc != kIOReturnNotReady || tries == 240) break;
        IOSleep(250);
    }
    if (rc) {fail(StDoorbells, rc); return false;}
    if (armcpu_spi_attach(32 + kSpiRsc, false, rscIrq, nullptr, nullptr, nullptr)) native_adsp_stats[15] |= 0x80 << 16;
    const uint32_t enable[3] = {qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_SMP2P),
                                qcom_ipcc_id(QCOM_IPCC_CLIENT_LPASS, QCOM_IPCC_SIGNAL_GLINK_QMP),
                                qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP)};
    for (uint32_t id : enable) wr(gIpcc, QCOM_IPCC_RECV_ENABLE, id);
    gIrq = true;
    return true;
}

// The firmware comes from the device's own firmware partition ("modem:", read in place) or, with sheng-adsp-fw=DIR, from DIR/adsp.mbn
// and DIR/adsp_dtb.mbn on a mounted volume. Without the argument the partition is tried first, then the two directories files may be
// installed in. The UFS driver and the Data volume may not be up when this thread starts: a few tries with growing pauses (about two
// minutes in all), not a poll. source gets the source that opened ("modem:" or a directory).
int openPair(Image &dtb, Image &adsp, const char *source, char *path, size_t size) {
    const bool modem = !strcmp(source, "modem:");
    snprintf(path, size, modem ? "%sadsp_dtb" : "%s/adsp_dtb.mbn", source);
    int rc = openImage(dtb, path);
    if (rc) return rc;
    snprintf(path, size, modem ? "%sadsp" : "%s/adsp.mbn", source);
    if ((rc = openImage(adsp, path))) release(dtb);
    return rc;
}
int openFirmware(Image &dtb, Image &adsp, char *path, size_t size, const char *chosen) {
    static const uint32_t pauses[] = {1, 2, 4, 8, 16, 30, 30, 30};
    const char *sources[3] = {"modem:", "/private/var/firmware/qcom/sm8550/sheng", "/private/var/ngpu/fw"};
    const unsigned count = chosen ? 1 : 3;
    if (chosen) sources[0] = chosen;
    int rc = 0;
    for (unsigned i = 0;; ++i) {
        int first = 0;
        for (unsigned s = 0; s < count; ++s) {
            rc = openPair(dtb, adsp, sources[s], path, size);
            if (!rc) return 0;
            if (!s) first = rc;
            if (rc != ENOENT && rc != ENXIO && rc != EIO) return rc;  // found but unusable: no other source is tried
        }
        rc = first;  // the reason the first source failed is the one worth reporting
        if (i == sizeof pauses / sizeof pauses[0]) return rc;
        IOSleep(pauses[i] * 1000);
    }
}

void shutdownDsp(bool dtb, bool adsp) {
    if (adsp) scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPasAdsp, 0, 0);
    if (dtb) scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPasDtb, 0, 0);
}

bool start() {
    char dir[80] = "";
    const bool chosen = PE_parse_boot_argn("sheng-adsp-fw", dir, sizeof dir) && dir[0];
    char path[112];
    Image &dtb = gDtbImage, &adsp = gAdspImage;
    int rc = openFirmware(dtb, adsp, path, sizeof path, chosen ? dir : nullptr);
    if (rc) {note("fw: ", path); fail(StFirmware, rc); return false;}
    native_adsp_stats[7] = (int32_t)(adsp.size >> 10);
    stage(StFirmware);
    if (native_adsp_stats[3] > 0) {fail(StFirmware, -114); release(dtb); release(adsp); return false;}  // running already: left alone

    // Our SMP2P item, before the DSP starts (it looks for it at boot).
    if ((rc = allocItem(kItemOut, QCOM_SMP2P_ITEM_BYTES))) {fail(StSmp2p, rc); release(dtb); release(adsp); return false;}
    qcom_smp2p_init_out(&gP2p, reinterpret_cast<volatile uint32_t *>(item(kItemOut, nullptr)), 0, kRemotePid);
    gStopEntry = qcom_smp2p_add_entry(&gP2p, "master-kernel");
    stage(StSmp2p);
    if (!attachIrqs()) {release(dtb); release(adsp); return false;}
    kickSmp2p();
    stage(StDoorbells);

    qcom_qmp_init(&gQmp, reinterpret_cast<volatile uint32_t *>(gAoss.va), 0x400, kickAop, nullptr);
    for (int phase = 0; phase < 4 && qcom_qmp_step(&gQmp) > 0 && gQmp.state != QcomQmpOpen; ++phase)
        waitUntil(1000, [] {return qcom_qmp_step(&gQmp) == QcomQmpOpen || gQmp.state < 0;});
    native_adsp_stats[5] = (native_adsp_stats[5] & ~0xff) | (gQmp.state & 0xff);
    if (gQmp.state != QcomQmpOpen) {fail(StLoadState, gQmp.state); release(dtb); release(adsp); return false;}
    qcom_qmp_send(&gQmp, "{class: image, res: load_state, name: adsp, val: on}");
    if (!waitUntil(1000, [] {return qcom_qmp_step(&gQmp) == QcomQmpOpen;})) {
        qcom_qmp_abandon(&gQmp);
        fail(StLoadState, -60); release(dtb); release(adsp); return false;
    }
    stage(StLoadState);

    static uint8_t db[kCmdDbSize];
    uint32_t lcx = 0, lmx = 0;
    if (!readCmdDb(db) || qcom_cmd_db_find(db, sizeof db, "lcx.lvl", &gLcx) || qcom_cmd_db_find(db, sizeof db, "lmx.lvl", &gLmx) ||
        qcom_rpmh_level_index(&gLcx, 0xffffffffu, &lcx) || qcom_rpmh_level_index(&gLmx, 0xffffffffu, &lmx)) {
        fail(StRails, -2); release(dtb); release(adsp); return false;
    }
    if ((rc = vote(lcx, lmx))) {fail(StRails, rc); release(dtb); release(adsp); return false;}
    stage(StRails);

    // The DSP's devicetree image first, as its own authenticated image, then the DSP image.
    if ((rc = authenticateHeader(dtb, kPasDtb, kDtbBase)) || (rc = loadSegments(dtb, kDtbBase, kDtbSize)) ||
        (rc = scmOk(scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPasDtb, 0, 0)) ? 0 : -3)) {
        fail(StDtb, rc); vote(0, 0); release(dtb); release(adsp); return false;
    }
    stage(StDtb);
    if ((rc = authenticateHeader(adsp, kPasAdsp, kAdspBase)) || (rc = loadSegments(adsp, kAdspBase, kAdspSize))) {
        fail(StLoaded, rc); shutdownDsp(true, false); vote(0, 0); release(dtb); release(adsp); return false;
    }
    stage(StLoaded);
    const uint64_t began = mach_absolute_time();
    if (!scmOk(scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPasAdsp, 0, 0))) {
        fail(StStarted, -3); shutdownDsp(true, false); vote(0, 0); release(dtb); release(adsp); return false;
    }
    stage(StStarted);
    const bool ready = waitUntil(5000, [] {return (gSlave & (kReady | kFatal)) != 0;});
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    native_adsp_stats[8] = (int32_t)(ns / 1000000);
    release(dtb);
    release(adsp);  // the metadata may go once the images are authenticated
    if (!ready || (gSlave & kFatal)) {
        readCrashReason();
        fail(StReady, ready ? -5 : -60);
        shutdownDsp(true, true);
        if (!gHandedOver) vote(0, 0);
        return false;
    }
    stage(StReady);
    return true;
}

bool startGlink() {
    int rc = allocItem(kItemGlinkDesc, 32);
    if (!rc) rc = allocItem(kItemGlinkTx, kGlinkFifo);
    uint32_t size = 0;
    volatile uint8_t *desc = item(kItemGlinkDesc, nullptr), *txf = item(kItemGlinkTx, &size);
    if (rc || !desc || !txf) {fail(StGlink, rc ? rc : -2); return false;}
    volatile uint32_t *d = reinterpret_cast<volatile uint32_t *>(desc);
    // Descriptor words: our FIFO's tail and head, then the DSP's FIFO's tail and head.
    const QcomGlinkFifo tx = {&d[1], &d[0], txf, size}, rx = {&d[3], &d[2], nullptr, 0};
    qcom_glink_init(&gGlink, tx, rx, kickGlink, nullptr);
    gGlinkStarted = true;
    applyClients();
    serviceGlink();
    return true;
}

void adspThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng-adsp");
    unsigned mode = 0;
    PE_parse_boot_argn("sheng-adsp", &mode, sizeof mode);
    if (!mapWindow(gSmemW, kSmem, kSmemSize) || !mapWindow(gMutex, kMutex, 0x1000) || !mapWindow(gIpcc, kIpcc, 0x1000) ||
        !mapWindow(gAoss, kAoss, 0x1000) || !mapWindow(gRsc, kRscDrv, 0x10000)) {
        fail(StMapped, -12);
        return;
    }
    gSmem = {gSmemW.va, (uint32_t)kSmemSize};
    gLayout = qcom_rsc_layout(rd(gRsc, 0));
    stage(StMapped);
    survey();
    stage(StSurveyed);
    if (mode < 2 || !start() || !startGlink()) return;
    // From here on the thread only answers doorbells and client sends; with nothing pending it sleeps without a deadline, except while
    // a send waits for an intent or FIFO room (then it looks again every second).
    for (;;) {
        bool pending = false;
        for (unsigned i = 0; i < gClientCount; ++i) pending |= gClients[i].count != 0;
        service(waitEvents(pending ? 1000 : 3600u * 1000u) | (pending ? kEvSend : 0));
    }
}
}  // namespace

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *name, uint32_t intent_size, uint32_t intent_count, int initiator,
                                                const ShengAdspClient *client, void *context) {
    if (!gClientLock || !name || !client || !intent_size || !intent_count || intent_count > QCOM_GLINK_MAX_LOCAL_INTENTS) return nullptr;
    uint8_t *storage = static_cast<uint8_t *>(IOMallocData((vm_size_t)intent_size * intent_count));
    Message *queue = static_cast<Message *>(IOMallocData(sizeof(Message) * kQueue));
    IOLockLock(gClientLock);
    ShengAdspChannel *c = gClientCount < kClients && storage && queue ? &gClients[gClientCount] : nullptr;
    if (c) {
        bzero(c, sizeof *c);
        strlcpy(c->name, name, sizeof c->name);
        c->intent_size = intent_size; c->intent_count = intent_count; c->initiator = initiator;
        c->client = *client; c->context = context; c->storage = storage; c->queue = queue;
        __atomic_store_n(&gClientCount, gClientCount + 1, __ATOMIC_RELEASE);
    }
    IOLockUnlock(gClientLock);
    if (!c) {
        if (storage) IOFreeData(storage, (vm_size_t)intent_size * intent_count);
        if (queue) IOFreeData(queue, sizeof(Message) * kQueue);
        return nullptr;
    }
    wake(kEvSend);
    return c;
}

extern "C" int sheng_adsp_send(ShengAdspChannel *c, const void *data, uint32_t length) {
    if (!c || !data || !length) return -2;
    if (length > SHENG_ADSP_MAX_MESSAGE) return -1;
    IOLockLock(gClientLock);
    if (c->count == kQueue) {IOLockUnlock(gClientLock); return -1;}
    Message &m = c->queue[(c->head + c->count) % kQueue];
    memcpy(m.data, data, length);
    m.length = length;
    c->count++;
    IOLockUnlock(gClientLock);
    wake(kEvSend);
    return 0;
}

extern "C" void sheng_adsp_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("sheng-adsp", &mode, sizeof mode) || !mode || native_adsp_stats[0]) return;
    gClientLock = IOLockAlloc();
    if (gClientLock && mode >= 2) sheng_qrtr_start();  // the router link registers before the DSP can open it
    thread_t thread = nullptr;
    if (!gClientLock || kernel_thread_start(adspThread, nullptr, &thread) != KERN_SUCCESS) {fail(StMapped, -7); return;}
    thread_deallocate(thread);
}
