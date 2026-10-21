// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off staged bring-up of the NT36532E touch controller. `sheng-touch=<n>`:
//   1  read-only snapshot of the GCC branches, the S4 DFS levels and the touch pins
//   2  vote the QUP clocks, wait for them, configure the pins, read the GENI engine identity
//   3  GENI SPI init and clock, then reset the touch bootloader and read the chip id
//   4  host-download the firmware, start scanning and process THP frames (needs the generated firmware object)
// Every stage reports on the diagnostic page; the first failure stops the sequence. Nothing here is validated on the
// device until a boot has shown it. Board constants and their sources are in TouchBoard.h.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "GeniSpi.h"
#include "NvtProtocol.h"
#include "NvtTouchCore.h"
#include "TouchBoard.h"
#include "SmmuBypass.h"
extern const uint8_t native_touch_firmware[] __attribute__((weak_import));
extern const uint32_t native_touch_firmware_size __attribute__((weak_import));
}

extern "C" {
char native_touch_rows[4][120];
char native_touch_trace[8][120];
volatile uint32_t native_touch_loop_step;
volatile uint32_t native_touch_stage_req, native_touch_stage_done, native_touch_step, native_touch_frames,
    native_touch_valid, native_touch_contacts, native_touch_hid_reports;
volatile int native_touch_error;
volatile int native_touch_last_x = -1, native_touch_last_y = -1;
// Latest finger set for the HID digitizer source (see NativeHidInput); sequence is odd while being written.
volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile int native_touch_xs[NVT_SLOTS], native_touch_ys[NVT_SLOTS], native_touch_ids[NVT_SLOTS];
// Finished strokes as published (int-150, for edge gestures that did nothing in int-148/149): a ring of {start ms, duration ms, first x, first y,
// last x, last y, frames} in published units (0.1 px, y already mirrored), and the extent every published contact reached {min x, max x,
// min y, max y}. Read by kits/sheng BootLog.c.
volatile int32_t native_touch_strokes[16][7];
volatile uint32_t native_touch_stroke_count;
volatile int32_t native_touch_extent[4] = {0x7fffffff, -1, 0x7fffffff, -1};
}

namespace {
struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

// The serial engine's DMA master reads and writes two 64 KiB buffers. The apps SMMU is firmware-managed; the stream gets the
// same pass-through mapping the UFS driver uses, so the engine addresses physical memory. Caches are maintained explicitly:
// the DT marks the wrapper dma-coherent, but whether bypass traffic snoops is not assumed.
constexpr size_t kDmaBytes = 64 * 1024;
void cacheRange(void *p, size_t len, int op) {   // 0 clean, 1 clean+invalidate, 2 invalidate
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4u << ((ctr >> 16) & 15);
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1), end = reinterpret_cast<uintptr_t>(p) + len;
    for (; a < end; a += line) {
        if (op == 0) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        else if (op == 1) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
void dmaClean(void *, void *p, size_t len) {cacheRange(p, len, 0);}
void dmaCleanInval(void *, void *p, size_t len) {cacheRange(p, len, 1);}
void dmaInval(void *, void *p, size_t len) {cacheRange(p, len, 2);}
uint32_t smmuRd(void *ctx, unsigned o) {return rd32(reinterpret_cast<uintptr_t>(ctx), o);}
void smmuWr(void *ctx, unsigned o, uint32_t v) {wr32(reinterpret_cast<uintptr_t>(ctx), o, v);}
int smmuErrorCode(const char *e) {
    static const char *const names[] = {"SMMU AArch64 stage 1 unsupported", "DMA exceeds SMMU address width", "unsupported SMMU geometry",
        "conflicting stream mapping", "reserved context bank is not a bypass bank", "no free stream-match group",
        "SMMU bypass bank readback", "SMMU stream context readback", "SMMU stream mapping readback"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) if (strcmp(e, names[i]) == 0) return static_cast<int>(i) + 1;
    return 99;
}

uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
// The SPI core polls its status registers with a 1 us wait between reads. IODelay blocks the thread for even that (assert_wait_deadline and
// thread_block, one timer interrupt and two context switches per read; int-182: the touch thread was the busiest non-idle kernel thread
// at 0.17 core, its samples all in geni_spi_transfer's polls). A wait this short spins on the counter instead.
void ioDelayUs(void *, unsigned us) {
    if (us > 50) {IODelay(us); return;}
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : 19200000) * us / 1000000 + 1;
    do {__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));} while (now - start < ticks);
}

struct Session {
    Region gccLow, gccS4, gccHalt, gccVote, tlmmIrq, tlmmSpi, wrapper, se, smmu;
    IOBufferMemoryDescriptor *dmaMem;
    uint64_t dmaPhys;
    int smmuSlot, smmuBank, smmuErr;
    // First transfers as seen on the wire (tx = bytes the driver sent, rx = bytes the engine returned), and the identification
    // result per SPI mode tried.
    unsigned traceN;
    uint32_t traceLen[8];
    uint8_t traceTx[8][8], traceRx[8][10];
    unsigned modeTried, modeUsed;
    // Last six transfers (ring) and the firmware being downloaded, for failures after identification.
    unsigned ringN;
    uint32_t ringLen[6];
    uint8_t ringTx[6][8], ringRx[6][8];
    unsigned fwParts, fwCasc, fwIlm, fwDlm, fwSize;
    // Frame loop evidence.
    uint32_t reads, bad, irqHigh, irqSeen, irqIdle, peeks, fresh, lastType;
    uint8_t head[24], thp[24];
    int mn, mx;
    int loopRc = 99;                  // engine loopback self-test (99 = not run)
    uint8_t loopRx[16];
    uint32_t loopTxIn, loopRxIn;
    uint8_t loopRxLate[8];
    uint32_t reg[16];                  // engine registers read back after the loopback transfer (what the writes left behind)
    uint8_t modeId[4][6];
    int modeRc[4];
    struct geni_spi spi;
    uint8_t *tx = nullptr, *rx = nullptr, *frame = nullptr;
    int16_t *matrix = nullptr;       // 4.8 KB touch matrix (heap: kernel thread stacks are 16 KiB)
    struct nvt_chip chip;
    struct nvt_bus bus;
    struct nvt_touch_core *core = nullptr;
    uint32_t vals[40];
    int lastRc = 0;
    uint32_t lastLen = 0, xIrq = 0, xGeni = 0, xTx = 0, xRx = 0;
    uint64_t levelHz[TB_DFS_LEVELS];
    unsigned level = 0, div = 0;
    uint64_t actualHz = 0;
};

int busXfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Session *s = static_cast<Session *>(ctx);
    int rc = geni_spi_transfer(&s->spi, tx, rx, len, 0);
    {
        unsigned i = s->ringN++ % 6;
        s->ringLen[i] = static_cast<uint32_t>(len);
        for (unsigned k = 0; k < 8; ++k) {
            s->ringTx[i][k] = (tx && k < len) ? tx[k] : 0;
            s->ringRx[i][k] = (rx && k < len && !rc) ? rx[k] : 0;
        }
    }
    if (!rc && s->traceN < 8) {
        unsigned i = s->traceN++;
        s->traceLen[i] = static_cast<uint32_t>(len);
        for (unsigned k = 0; k < 8 && k < len && tx; ++k) s->traceTx[i][k] = tx[k];
        for (unsigned k = 0; k < 10 && k < len && rx; ++k) s->traceRx[i][k] = rx[k];
    }
    if (rc) {   // keep the engine state of the first failure
        if (!s->lastRc) {
            s->lastRc = rc;
            s->lastLen = static_cast<uint32_t>(len);
            // Registers read by the driver just before it cancelled the command (reading them here would show the
            // post-recovery state).
            s->xIrq = s->spi.snap_irq;
            s->xGeni = s->spi.snap_status;
            s->xTx = s->spi.snap_txfs;
            s->xRx = s->spi.snap_rxfs;
        }
    }
    return rc;
}
void busDelay(void *, unsigned ms) {IOSleep(ms);}

void fail(unsigned step, int code) {
    if (!native_touch_error) {native_touch_step = step; native_touch_error = code ? code : -1;}
}

void rows(const Session &s);
void mark(const Session &s, unsigned step) {native_touch_step = step; rows(s);}

// snprintf returns the length it would have written, so a running offset must be clamped before the next append.
struct Out {
    char *p; size_t cap, n;
    void add(const char *fmt, unsigned a = 0, unsigned b = 0) {
        if (n + 1 >= cap) return;
        int w = snprintf(p + n, cap - n, fmt, a, b);
        if (w < 0) return;
        n = (n + static_cast<size_t>(w) >= cap) ? cap - 1 : n + static_cast<size_t>(w);
    }
    void hex(uint8_t v) {add("%02x", v);}
};

void traceRows(const Session &s) {
    for (unsigned r = 0; r < 2; ++r) {
        Out o = {native_touch_trace[r], sizeof native_touch_trace[r], 0};
        o.p[0] = 0;
        o.add("TCH xfer");
        for (unsigned j = 0; j < 2; ++j) {
            unsigned i = r * 2 + j;
            if (i >= s.traceN) break;
            o.add(" #%u len%u tx", i, s.traceLen[i]);
            for (unsigned k = 0; k < 8 && k < s.traceLen[i]; ++k) o.hex(s.traceTx[i][k]);
            o.add(" rx");
            for (unsigned k = 0; k < 10 && k < s.traceLen[i]; ++k) o.hex(s.traceRx[i][k]);
            o.add(" |");
        }
    }
    const struct nvt_chip &c = s.chip;
    Out v = {native_touch_trace[2], sizeof native_touch_trace[2], 0};
    v.p[0] = 0;
    v.add("TCH sram-readback ilm bad%u", c.sram_bad[0]);
    v.add(" first%d", static_cast<unsigned>(c.sram_first_bad[0]));
    v.add(" crc%08x", c.sram_crc[0]);
    v.add(" | dlm bad%u", c.sram_bad[1]);
    v.add(" first%d", static_cast<unsigned>(c.sram_first_bad[1]));
    v.add(" crc%08x", c.sram_crc[1]);
    Out f = {native_touch_trace[3], sizeof native_touch_trace[3], 0};
    f.p[0] = 0;
    f.add("TCH frames lp%u", native_touch_loop_step);
    f.add(" peek%u", s.peeks);
    f.add(" new%u", s.fresh);
    f.add(" type%u", s.lastType);
    f.add(" reads%u", s.reads);
    f.add(" bad%u", s.bad);
    f.add(" irq%u", s.irqHigh);
    f.add(" seen%u", s.irqSeen);
    f.add(" min%d", static_cast<unsigned>(s.mn));
    f.add(" max%d head", static_cast<unsigned>(s.mx));
    for (unsigned k = 0; k < 24; ++k) f.hex(s.head[k]);
    Out d = {native_touch_trace[4], sizeof native_touch_trace[4], 0};
    d.p[0] = 0;
    d.add("TCH dl try%u", c.dl_attempt);
    d.add(" stage%u", c.dl_stage);
    d.add(" rc%d", static_cast<unsigned>(c.dl_rc));
    d.add(" wait%02x", c.dl_wait_value);
    d.add(" polls%u rst", c.dl_polls);
    for (unsigned k = 0; k < 5; ++k) d.hex(c.dl_reset_state[k]);
    d.add(" fw parts%u", s.fwParts);
    d.add(" casc%u", s.fwCasc);
    d.add(" ilm%u", s.fwIlm);
    d.add(" dlm%u", s.fwDlm);
    for (unsigned r = 0; r < 2; ++r) {
        Out o = {native_touch_trace[5 + r], sizeof native_touch_trace[5 + r], 0};
        o.p[0] = 0;
        o.add("TCH last");
        for (unsigned j = 0; j < 2; ++j) {
            unsigned k = r * 2 + j;                       // last four, oldest first
            if (s.ringN < 4 && k >= s.ringN) break;
            unsigned seq = s.ringN >= 4 ? s.ringN - 4 + k : k;
            unsigned i = seq % 6;
            o.add(" #%u len%u tx", seq, s.ringLen[i]);
            for (unsigned q = 0; q < 8 && q < s.ringLen[i]; ++q) o.hex(s.ringTx[i][q]);
            o.add(" rx");
            for (unsigned q = 0; q < 8 && q < s.ringLen[i]; ++q) o.hex(s.ringRx[i][q]);
            o.add(" |");
        }
    }
    Out t = {native_touch_trace[7], sizeof native_touch_trace[7], 0};
    t.p[0] = 0;
    t.add("TCH thp-header");
    for (unsigned q = 0; q < 24; ++q) t.hex(s.thp[q]);
}

void rows(const Session &s) {
    traceRows(s);
    const uint32_t *v = s.vals;
    snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
        "TCH s%u/%u st%u e%d vote%08x h%08x %08x %08x %08x %08x gp%08x dfs%08x",
        native_touch_stage_done, native_touch_stage_req, native_touch_step, native_touch_error,
        v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    snprintf(native_touch_rows[1], sizeof native_touch_rows[1],
        "TCH p%08x %08x %08x %08x irq%08x wr%08x fw%08x if%08x hw%08x L%u/%u %uHz",
        v[9], v[10], v[11], v[12], v[13], v[14], v[15], v[16], v[17], s.level, s.div, (unsigned)s.actualHz);
    snprintf(native_touch_rows[2], sizeof native_touch_rows[2],
        "TCH id%02x%02x%02x%02x%02x%02x %.8s v%02x p%04x %ux%u fr%u ok%u c%u %d,%d hid%u",
        s.chip.id[0], s.chip.id[1], s.chip.id[2], s.chip.id[3], s.chip.id[4], s.chip.id[5],
        s.chip.map ? s.chip.map->name : "-", v[18], v[19], v[20], v[21], native_touch_frames, native_touch_valid,
        native_touch_contacts, native_touch_last_x, native_touch_last_y, native_touch_hid_reports);
    snprintf(native_touch_rows[3], sizeof native_touch_rows[3],
        "TCH xf rc%d n%u ev%08x gs%08x tx%08x rx%08x dma t%x r%x ti%x ri%x sm%d/%d/e%d pa%llx",
        s.lastRc, s.lastLen, s.xIrq, s.xGeni, s.xTx, s.xRx, s.spi.snap_dma_tx, s.spi.snap_dma_rx, s.spi.snap_dma_txin,
        s.spi.snap_dma_rxin, s.smmuSlot, s.smmuBank, s.smmuErr, (unsigned long long)s.dmaPhys);
}

struct Stroke {int id, x0, y0, x1, y1, frames; uint64_t t0;};
Stroke strokes[NVT_SLOTS];
unsigned strokeCount;
uint32_t touchMs() {
    uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return (uint32_t)(mach_absolute_time() / ((f ? f : 19200000) / 1000));
}
void trackStrokes(unsigned count, const int *ids, const int *xs, const int *ys) {
    const uint32_t now = touchMs();
    for (unsigned i = 0; i < count; ++i) {
        if (xs[i] < native_touch_extent[0]) native_touch_extent[0] = xs[i];
        if (xs[i] > native_touch_extent[1]) native_touch_extent[1] = xs[i];
        if (ys[i] < native_touch_extent[2]) native_touch_extent[2] = ys[i];
        if (ys[i] > native_touch_extent[3]) native_touch_extent[3] = ys[i];
        unsigned k = 0;
        while (k < strokeCount && strokes[k].id != ids[i]) ++k;
        if (k == strokeCount) {
            if (strokeCount == NVT_SLOTS) continue;
            strokes[strokeCount++] = {ids[i], xs[i], ys[i], xs[i], ys[i], 0, now};
        }
        strokes[k].x1 = xs[i]; strokes[k].y1 = ys[i]; ++strokes[k].frames;
    }
    for (unsigned k = 0; k < strokeCount;) {
        bool live = false;
        for (unsigned i = 0; i < count && !live; ++i) live = ids[i] == strokes[k].id;
        if (live) {++k; continue;}
        volatile int32_t *r = native_touch_strokes[native_touch_stroke_count & 15];
        r[0] = (int32_t)(uint32_t)strokes[k].t0; r[1] = (int32_t)(now - (uint32_t)strokes[k].t0);
        r[2] = strokes[k].x0; r[3] = strokes[k].y0; r[4] = strokes[k].x1; r[5] = strokes[k].y1; r[6] = strokes[k].frames;
        native_touch_stroke_count = native_touch_stroke_count + 1;
        strokes[k] = strokes[--strokeCount];
    }
}

// Whoever consumes the finger set (NativeHidInput) registers a function here; it is called from the thread that published, after the seqlock closes.
static void (*gTouchSink)(void *);
static void *gTouchSinkContext;
extern "C" void native_touch_set_sink(void (*sink)(void *), void *context) {gTouchSinkContext = context; __atomic_store_n(&gTouchSink, sink, __ATOMIC_RELEASE);}
void publish(unsigned count, const nvt_contact *c) {
    int ids[NVT_SLOTS], xs[NVT_SLOTS], ys[NVT_SLOTS];
    for (unsigned i = 0; i < count && i < NVT_SLOTS; ++i) {
        ids[i] = c[i].tracking_id; xs[i] = c[i].x; ys[i] = c[i].y < 0 ? c[i].y : (20319 - (c[i].y > 20319 ? 20319 : c[i].y));
    }
    trackStrokes(count < NVT_SLOTS ? count : NVT_SLOTS, ids, xs, ys);
    native_touch_seq = native_touch_seq + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    for (unsigned i = 0; i < count && i < NVT_SLOTS; ++i) {
        native_touch_xs[i] = c[i].x;
        // Panel photograph (v94): X is correct and Y is mirrored, so publish Y from the other edge (axis maximum 20320).
        native_touch_ys[i] = c[i].y < 0 ? c[i].y : (20319 - (c[i].y > 20319 ? 20319 : c[i].y));
        native_touch_ids[i] = c[i].tracking_id;
    }
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1;
    if (void (*sink)(void *) = __atomic_load_n(&gTouchSink, __ATOMIC_ACQUIRE)) sink(gTouchSinkContext);
}

bool snapshot(Session &s) {
    mark(s, 1);
    if (!mapRegion(TB_GCC_BASE + 0x0000, 0x1000, s.gccLow) || !mapRegion(TB_GCC_BASE + 0x18000, 0x1000, s.gccS4) ||
        !mapRegion(TB_GCC_BASE + 0x23000, 0x1000, s.gccHalt) || !mapRegion(TB_GCC_BASE + 0x52000, 0x1000, s.gccVote) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_IRQ), 0x1000, s.tlmmIrq) ||
        !mapRegion(TB_TLMM_BASE + tb_tlmm_ctl(TB_GPIO_SPI_MISO), 4 * TB_GPIO_STRIDE, s.tlmmSpi)) {
        fail(1, -12);
        return false;
    }
    uint32_t *v = s.vals;
    v[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    v[1] = rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff);
    v[2] = rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff);
    v[3] = rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff);
    v[4] = rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff);
    v[5] = rd32(s.gccS4.va, TB_HALT_S4 & 0xfff);
    v[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE);
    v[7] = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_DFSR_CMD) & 0xfff);
    v[8] = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_CFG) & 0xfff);
    for (unsigned l = 0; l < TB_DFS_LEVELS; ++l) {
        uint32_t cfg = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_CFG(l)) & 0xfff);
        uint32_t m = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_M(l)) & 0xfff);
        uint32_t n = rd32(s.gccS4.va, (TB_S4_RCG + TB_RCG_PERF_N(l)) & 0xfff);
        s.levelHz[l] = tb_dfs_level_hz(cfg, m, n);
        if (l < 4) s.vals[24 + l] = cfg;
    }
    for (unsigned i = 0; i < 4; ++i) v[9 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    v[13] = rd32(s.tlmmIrq.va, 4);
    return true;
}

bool enable(Session &s) {
    mark(s, 2);
    // GPLL0 feeds the DFS levels; vote for it (a shared vote, not an exclusive enable) and wait for lock.
    wr32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff) | 1u);
    for (unsigned i = 0; i < 10000 && !(rd32(s.gccLow.va, TB_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);
    s.vals[6] = rd32(s.gccLow.va, TB_GCC_GPLL0_MODE);
    wr32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff) | TB_VOTE_ALL);
    // Branches with hardware clock gating (bit 1) show CLK_OFF while idle and wake on access, so only a branch
    // without that bit must have CLK_OFF clear.
    auto running = [](uint32_t cbcr) {return !(cbcr & TB_CBCR_CLK_OFF) || (cbcr & 2u);};
    bool ok = false;
    for (unsigned i = 0; i < 2000 && !ok; ++i) {
        ok = running(rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff)) && running(rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff)) &&
             !(rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff) & TB_CBCR_CLK_OFF) &&
             !(rd32(s.gccS4.va, TB_HALT_S4 & 0xfff) & TB_CBCR_CLK_OFF);
        if (!ok) IODelay(10);
    }
    s.vals[0] = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff);
    s.vals[1] = rd32(s.gccHalt.va, TB_HALT_M_AHB & 0xfff);
    s.vals[2] = rd32(s.gccHalt.va, TB_HALT_S_AHB & 0xfff);
    s.vals[3] = rd32(s.gccHalt.va, TB_HALT_CORE & 0xfff);
    s.vals[4] = rd32(s.gccHalt.va, TB_HALT_CORE_2X & 0xfff);
    s.vals[5] = rd32(s.gccS4.va, TB_HALT_S4 & 0xfff);
    if (!ok) {fail(2, -110); return false;}   // never touch the engine without running clocks
    const uint32_t cfg = tb_pin_cfg(TB_FUNC_QUP1_SE4, TB_DRIVE_6MA);
    for (unsigned i = 0; i < 4; ++i) wr32(s.tlmmSpi.va, i * TB_GPIO_STRIDE, cfg);
    for (unsigned i = 0; i < 4; ++i) s.vals[9 + i] = rd32(s.tlmmSpi.va, i * TB_GPIO_STRIDE);
    mark(s, 3);   // the next accesses touch the engine: if the page freezes here, the engine access hung
    if (!mapRegion(TB_QUP_WRAPPER_BASE, 0x2000, s.wrapper) || !mapRegion(TB_SE_BASE, TB_SE_SIZE, s.se)) {fail(3, -12); return false;}
    s.spi.se = (struct geni_io){reinterpret_cast<void *>(s.se.va), ioRd, ioWr, ioDelayUs};
    s.spi.wrapper = (struct geni_io){reinterpret_cast<void *>(s.wrapper.va), ioRd, ioWr, ioDelayUs};
    mark(s, 31);   // first engine register read
    int r = geni_spi_read_info(&s.spi);
    s.vals[14] = s.spi.info.qup_hw_version;
    s.vals[15] = s.spi.info.fw_revision;
    s.vals[16] = s.spi.info.if_disable;
    s.vals[17] = s.spi.info.hw_param0;
    if (r) {fail(3, r); return false;}
    return true;
}

bool prepareDma(Session &s) {
    mark(s, 41);
    s.dmaMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
                                                               2 * kDmaBytes, 0xfffffff000ULL);
    if (!s.dmaMem || s.dmaMem->prepare() != kIOReturnSuccess) {fail(41, -12); return false;}
    mark(s, 42);
    addr64_t seg_len = 0;
    s.dmaPhys = s.dmaMem->getPhysicalSegment(0, &seg_len);
    uint8_t *virt = static_cast<uint8_t *>(s.dmaMem->getBytesNoCopy());
    if (!virt || !s.dmaPhys || seg_len < 2 * kDmaBytes) {fail(42, -14); return false;}
    // Cached allocation (kIOMapInhibitCache on RAM above 4 GiB panics in phystokv_range: the cache-attribute path truncates the
    // page number to 32 bits), so the engine's view is made coherent with explicit cache maintenance.
    memset(virt, 0, 2 * kDmaBytes);
    cacheRange(virt, 2 * kDmaBytes, 1);
    if (!mapRegion(TB_APPS_SMMU_BASE, TB_APPS_SMMU_SIZE, s.smmu)) {fail(43, -12); return false;}
    mark(s, 43);
    struct smmu_io io = {reinterpret_cast<void *>(s.smmu.va), smmuRd, smmuWr};
    struct smmu_plan plan = smmu_plan_bypass(&io, TB_QUP_SMMU_SID, s.dmaPhys, 2 * kDmaBytes);
    s.smmuSlot = static_cast<int>(plan.slot); s.smmuBank = static_cast<int>(plan.bank);
    if (plan.error) {s.smmuErr = smmuErrorCode(plan.error); fail(43, -100 - s.smmuErr); return false;}
    const char *e = smmu_configure_bypass(&io, &plan, TB_QUP_SMMU_SID);
    if (e) {s.smmuErr = smmuErrorCode(e); fail(44, -100 - s.smmuErr); return false;}
    s.spi.use_dma = 1;
    s.spi.dma.ctx = nullptr;
    s.spi.dma.clean = dmaClean;
    s.spi.dma.clean_inval = dmaCleanInval;
    s.spi.dma.inval = dmaInval;
    s.spi.dma.tx_virt = virt;
    s.spi.dma.rx_virt = virt + kDmaBytes;
    s.spi.dma.tx_phys = s.dmaPhys;
    s.spi.dma.rx_phys = s.dmaPhys + kDmaBytes;
    s.spi.dma.size = kDmaBytes;
    return true;
}

bool openBus(Session &s, unsigned hz) {
    mark(s, 4);
    s.spi.poll_limit = 400000;
    s.spi.poll_only = 2;             // keep the top-level IRQ enable: the status registers must stay visible to the poll
    // v53-v55 hardware: FIFO_IF_DISABLE=1 and the register that controls it (0x2008) aborts on access, so FIFO transfers
    // cannot be used; the SE DMA engine is the host-visible path.
    if (s.spi.info.fifo_disabled && !prepareDma(s)) return false;
    int r = geni_spi_init(&s.spi);
    if (r) {fail(4, r); return false;}
    (void)hz;
    return true;
}

bool configureClock(Session &s, unsigned hz) {
    struct tb_clock_choice choice = {};
    if (tb_choose_clock(s.levelHz, TB_DFS_LEVELS, s.spi.oversampling, hz, &choice)) {fail(5, -22); return false;}
    s.level = choice.level;
    s.div = choice.div;
    s.actualHz = choice.actual_hz;
    // The divider arithmetic lives in the GENI library; the DFS level is selected through GENI_CLK_SEL.
    int r = geni_spi_set_clock(&s.spi, hz, static_cast<unsigned>(choice.sclk_hz), choice.level);
    if (r) {fail(5, r); return false;}
    r = geni_spi_setup(&s.spi, TB_SPI_MODE, 0);
    if (r) {fail(5, r); return false;}
    return true;
}
}  // namespace

// The touch interrupt (GPIO 25): registers per pin at TLMM + 0x1000 * pin, as pinctrl-msm.c / pinctrl-sm8550.c define them: +0x4 IN_OUT (bit 0 the
// level), +0x8 INTR_CFG (bit 0 enable, bit 1 polarity, bits 3:2 detection 0 level / 1 rising / 2 falling / 3 both, bit 4 raw status enable, bits 7:5
// target, 3 = the application processor), +0xc INTR_STATUS (bit 0; acknowledged by writing 0, the sm8550 groups set no intr_ack_high). All TLMM
// pin interrupts share the summary SPI 208 (sm8550.dtsi tlmm interrupts, level high), INTID 240.
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" {volatile uint32_t native_touch_irq_stats[4];}   // [0] 0 not armed, 1 armed, 2 attach failed, [1] interrupts, [2] spurious, [3] attach status
static volatile uint32_t gTouchIrqEvents;
static volatile uint32_t *gTouchPin;
static void touchIrq(void *) {
    if (!(gTouchPin[0xc / 4] & 1)) {native_touch_irq_stats[2] = native_touch_irq_stats[2] + 1; return;}
    gTouchPin[0xc / 4] = 0;
    native_touch_irq_stats[1] = native_touch_irq_stats[1] + 1;
    __atomic_fetch_add(&gTouchIrqEvents, 1, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gTouchIrqEvents);
}
static bool armTouchIrq(Session &s) {
    gTouchPin = reinterpret_cast<volatile uint32_t *>(s.tlmmIrq.va);
    uint32_t cfg = gTouchPin[0x8 / 4];
    cfg &= ~((7u << 5) | (3u << 2) | (1u << 1) | 1u);
    cfg |= (3u << 5) | (1u << 2) | (1u << 1) | (1u << 4);   // application processor, rising edge, polarity high, raw status on
    gTouchPin[0x8 / 4] = cfg;
    gTouchPin[0xc / 4] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    if (!armcpu_spi_attach) {native_touch_irq_stats[0] = 2; native_touch_irq_stats[3] = 0xffff; return false;}
    const int rc = armcpu_spi_attach(32 + 208, false, touchIrq, nullptr, nullptr, nullptr);
    native_touch_irq_stats[3] = (uint32_t)rc;
    if (rc) {native_touch_irq_stats[0] = 2; return false;}
    gTouchPin[0x8 / 4] = cfg | 1u;   // enable last
    native_touch_irq_stats[0] = 1;
    return true;
}

static void touchThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(),"sheng-touch");
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) {fail(0, -12); return;}
    bzero(s, sizeof *s);
    unsigned stage = native_touch_stage_req, hz = TB_SPI_DEFAULT_HZ;
    PE_parse_boot_argn("sheng-touch-hz", &hz, sizeof hz);
    if (hz < 100000 || hz > TB_SPI_MAX_HZ) hz = TB_SPI_DEFAULT_HZ;
    do {
        if (!snapshot(*s)) break;
        native_touch_stage_done = 1;
        rows(*s);
        if (stage < 2) break;
        if (!enable(*s)) break;
        native_touch_stage_done = 2;
        rows(*s);
        if (stage < 3) break;
        if (!openBus(*s, hz) || !configureClock(*s, hz)) break;
        s->tx = static_cast<uint8_t *>(IOMalloc(NVT_TRANSFER_LEN + 8));
        s->rx = static_cast<uint8_t *>(IOMalloc(NVT_READ_MAX + 2));
        s->frame = static_cast<uint8_t *>(IOMalloc(NVT_THP_FRAME_LEN));
        s->matrix = static_cast<int16_t *>(IOMalloc(NVT_NODES * sizeof(int16_t)));
        if (!s->tx || !s->rx || !s->frame || !s->matrix) {fail(6, -12); break;}
        s->bus = (struct nvt_bus){s, busXfer, busDelay};
        s->chip = {};
        s->chip.bus = &s->bus;
        s->chip.scratch = s->tx;
        s->chip.rxbuf = s->rx;
        mark(*s, 7);
        // SPI mode: sheng-touch-mode=0..3 forces one; otherwise sweep until the chip id matches (the board DT gives no
        // spi-cpha/spi-cpol, i.e. mode 0, so this is a hardware cross-check rather than a guess).
        // Engine loopback: MOSI is fed back to MISO inside the controller, so this isolates the DMA, packing and clock path
        // from the touch chip.
        {
            static const uint8_t pattern[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00};
            {volatile uint8_t *pre = s->spi.dma.rx_virt; for (unsigned k = 0; k < 64; ++k) pre[k] = 0x5a;}
            geni_spi_setup(&s->spi, GENI_SPI_LOOP, 0);
            s->loopRc = geni_spi_transfer(&s->spi, pattern, s->rx, sizeof pattern, 0);
            for (unsigned k = 0; k < 16; ++k) s->loopRx[k] = s->rx[k];
            s->loopTxIn = s->spi.last_tx_in;
            s->loopRxIn = s->spi.last_rx_in;
            IODelay(2000);   // is the data merely late?
            {volatile uint8_t *late = s->spi.dma.rx_virt; for (unsigned k = 0; k < 8; ++k) s->loopRxLate[k] = late[k];}
            static const uint32_t regs[16] = {SE_DMA_TX_PTR_L, SE_DMA_TX_PTR_H, SE_DMA_TX_ATTR, SE_DMA_TX_LEN, SE_DMA_RX_PTR_L,
                SE_DMA_RX_PTR_H, SE_DMA_RX_ATTR, SE_DMA_RX_LEN, SE_GENI_DMA_MODE_EN, SE_SPI_LOOPBACK, SE_GSI_EVENT_EN, SE_IRQ_EN,
                SE_DMA_TX_IRQ_STAT, SE_DMA_RX_IRQ_STAT, SE_GENI_STATUS, SE_GENI_M_IRQ_STATUS};
            for (unsigned k = 0; k < 16; ++k) s->reg[k] = ioRd(s->spi.se.ctx, regs[k]);
            geni_spi_setup(&s->spi, 0, 0);
            rows(*s);
        }
        unsigned forced = 4;
        PE_parse_boot_argn("sheng-touch-mode", &forced, sizeof forced);
        static const unsigned order[4] = {0, 3, 1, 2};
        int r = NVT_ERR_CHIP;
        for (unsigned i = 0; i < 4 && r; ++i) {
            unsigned m = forced < 4 ? forced : order[i];
            geni_spi_setup(&s->spi, ((m & 1) ? GENI_SPI_CPHA : 0) | ((m & 2) ? GENI_SPI_CPOL : 0), 0);
            s->chip = {};
            s->chip.bus = &s->bus;
            s->chip.scratch = s->tx;
            s->chip.rxbuf = s->rx;
            r = nvt_identify(&s->chip);
            s->modeId[i][0] = s->chip.id[0]; s->modeId[i][1] = s->chip.id[1]; s->modeId[i][2] = s->chip.id[2];
            s->modeId[i][3] = s->chip.id[3]; s->modeId[i][4] = s->chip.id[4]; s->modeId[i][5] = s->chip.id[5];
            s->modeRc[i] = r;
            s->modeTried = i + 1;
            s->modeUsed = m;
            rows(*s);
            if (forced < 4) break;
        }
        if (r) {fail(7, r); rows(*s); break;}
        native_touch_stage_done = 3;
        rows(*s);
        if (stage < 4) break;
        if (!&native_touch_firmware || !&native_touch_firmware_size || !native_touch_firmware_size) {fail(8, -2); break;}
        mark(*s, 8);
        struct nvt_fw fw;
        r = nvt_fw_parse(&fw, native_touch_firmware, native_touch_firmware_size);
        if (r) {fail(8, r); break;}
        s->fwParts = fw.count; s->fwCasc = static_cast<unsigned>(fw.cascade_2nd_header);
        s->fwIlm = fw.part[0].size; s->fwDlm = fw.part[1].size; s->fwSize = static_cast<unsigned>(native_touch_firmware_size);
        mark(*s, 9);
        s->chip.reset_retries = 60;   // vendor: 10 polls of 10 ms; longer here to tell "slow" from "never"
        r = nvt_download_firmware(&s->chip, &fw);
        if (r) {fail(9, r); rows(*s); break;}
        mark(*s, 10);
        struct nvt_fw_info info;
        r = nvt_start_scanning(&s->chip, &info);
        s->vals[18] = info.fw_ver;
        s->vals[19] = info.pid;
        s->vals[20] = info.x_num;
        s->vals[21] = info.y_num;
        if (r && r != NVT_ERR_FW) {fail(10, r); rows(*s); break;}
        native_touch_stage_done = 4;
        rows(*s);
        s->core = static_cast<struct nvt_touch_core *>(IOMalloc(sizeof *s->core));
        if (!s->core) {fail(11, -12); break;}
        nvt_touch_reset(s->core);
        mark(*s, 11);
        uint32_t lastCounter = 0xffffffffu;
        unsigned tick = 0, rowTick = 0;
        s->mn = 0; s->mx = 0;
        // The firmware may not pulse GPIO25 in its idle (doze) mode, so the trigger is the frame counter: a 32-byte peek of the
        // THP header every few milliseconds (cheap), and a full 5 KB read only for a new, valid touch frame. The interrupt line
        // is still sampled and counted.
        // Event driven (int-189): the controller pulses its interrupt line (GPIO 25, rising edge, devicetree novatek,irq-gpio flags 0x2001) for every
        // frame, the TLMM raises its summary interrupt (SPI 208), the handler acknowledges it and wakes this thread, which then reads the frame. Nothing
        // wakes while nobody touches the panel. The earlier loop polled the frame counter every 2 ms (int-185: 93 of 300 s of CPU); sheng-touch-poll=1
        // brings that back for diagnosis only, and it is also the fallback if the interrupt cannot be armed (then native_touch_irq_stats[0] says 2).
        unsigned pollOnly = 0;
        PE_parse_boot_argn("sheng-touch-poll", &pollOnly, sizeof pollOnly);
        const bool irqArmed = !pollOnly && armTouchIrq(*s);
        uint32_t seen = gTouchIrqEvents;
        for (;;) {
            if (irqArmed) {
                assert_wait((event_t)&gTouchIrqEvents, THREAD_UNINT);
                if (gTouchIrqEvents != seen) clear_wait(current_thread(), THREAD_AWAKENED);
                else thread_block(THREAD_CONTINUE_NULL);
                seen = gTouchIrqEvents;
            }
            for (;;) {   // every frame that is ready now
                native_touch_loop_step = 1;
                const bool high = (rd32(s->tlmmIrq.va, 4) & 1) != 0;
                s->irqHigh = high;
                if (high) ++s->irqSeen;
                uint8_t pk[NVT_THP_PEEK_LEN];
                native_touch_loop_step = 2;
                if (nvt_peek_frame_header(&s->chip, pk)) {fail(11, NVT_ERR_BUS); break;}
                native_touch_loop_step = 3;
                ++s->peeks;
                for (unsigned q = 0; q < 24; ++q) s->thp[q] = pk[q];
                if ((++rowTick & 255) == 0) rows(*s);
                if (!nvt_peek_valid(pk)) {++s->bad; break;}
                const unsigned counter = nvt_peek_counter(pk), type = nvt_peek_type(pk);
                if (counter == lastCounter) break;
                lastCounter = counter;
                ++s->fresh;
                s->lastType = type;
                if (type != 2 && type != 4) continue;
                native_touch_loop_step = 4;
                if (nvt_read_frame(&s->chip, s->frame)) {fail(11, NVT_ERR_BUS); break;}
                native_touch_loop_step = 5;
                ++s->reads;
                for (unsigned q = 0; q < 24; ++q) s->head[q] = s->frame[q];
                if (!nvt_frame_header_valid(s->frame)) {++s->bad; continue;}   // torn: the chip rewrote it while it was read
                native_touch_valid = native_touch_valid + 1;
                int16_t *matrix = s->matrix;
                nvt_frame_matrix(s->frame, matrix);
                int mn = matrix[0], mx = matrix[0];
                for (unsigned q = 1; q < NVT_NODES; ++q) {if (matrix[q] < mn) mn = matrix[q]; if (matrix[q] > mx) mx = matrix[q];}
                s->mn = mn; s->mx = mx;
                struct nvt_contact out[NVT_SLOTS];
                native_touch_loop_step = 6;
                const int n = nvt_touch_process(s->core, matrix, type, out, NVT_SLOTS);
                native_touch_loop_step = 7;
                native_touch_frames = native_touch_frames + 1;
                if (n >= 0) {
                    native_touch_contacts = static_cast<uint32_t>(n);
                    if (n > 0) {native_touch_last_x = out[0].x; native_touch_last_y = out[0].y;}
                    publish(static_cast<unsigned>(n), out);
                }
                if ((++tick & 15) == 0) rows(*s);
                if (irqArmed && gTouchIrqEvents == seen) break;   // no further pulse: the frame just read was the only one
            }
            if (!irqArmed) IOSleep(pollOnly ? 2 : 10);   // fallback / diagnosis only
        }
    } while (0);
    rows(*s);
}

extern "C" void native_touch_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("sheng-touch", &stage, sizeof stage) || stage < 1 || native_touch_stage_req) return;
    if (stage > 4) stage = 4;
    native_touch_stage_req = stage;
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_touch_error = -12;
}
