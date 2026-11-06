// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8550 USB device controller behind Apple's own stack. The DWC3 programming model, endpoints, control transfers and everything
// above them (IOUSBDeviceFamily, AppleUSBDeviceMux, configd's USBDeviceConfiguration, lockdownd) stay the unchanged system parts:
// this class only supplies what an Apple SoC subclass of AppleUSBXDCI supplies, namely the register window, the interrupt, power
// and the PHY. Hardware facts come from mainline Linux: sm8550.dtsi, gcc-sm8550.c, dwc3-qcom.c, phy-snps-eusb2.c.
#include "OriginalUsbXdciABI.h"
#include "OriginalUsbXhciABI.h"
#include "QcomEusb2Phy.h"
#include "QcomQmpUsb3.h"
#include "GeniI2c.h"
#include "QcomUcsi.h"
#include "QcomRpmh.h"
#include "AppsSmmuLock.h"
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
extern "C" {
int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);
int armcpu_spi_mask(uint32_t, bool);
bool sm8550_gcc_update32(volatile uint32_t *, uint32_t, uint32_t);
// [0] stage (negative: the step that failed), [1] GSNPSID, [2] GUID-free hardware parameter 0, [3] parameter 3, [4] parameter 7,
// [5] GDSC USB30, [6] GDSC PHY, [7] SMMU slot | bank << 16, [8] repeater status, [9] RPMh rails found, [10] VBUS override state,
// [11] interrupts, [12] powerOn calls, [13] powerOff calls, [14] controllerReset result, [15] DSTS at power on, [16] DCFG after
// reset, [17] GCTL after reset, [18] GUSB2PHYCFG after reset, [19] start result, [20] power state request result, [21] kext load result,
// with sheng-usb=2, every 2 s: [22] GSBUSCFG0, [23] GSBUSCFG1, [24] GUCTL1, [25] SMMU global fault, [26] DSTS, [27] DALEPENA,
// [28] GEVNTCOUNT, [29] DEPCMD of EP0 OUT, [30] bypass bank fault status, [31] GEVNTSIZ.
// [32] retimer: chip id | stage << 16, [33] QMP PCS status, [34] orientation GPIO, [35] VBUS changes | last value << 16,
// [44] role applied | wanted << 8 | core << 16, [45] role changes, [46] xHCI load << 16 | start, [47] USBCMD, [48] USBSTS, [49] [50] PORTSC 1 and 2,
// [51] HCSPARAMS1,
// [40] UCSI state | version << 16, [41] UCSI connector status, [42] CCI, [43] changes | errors << 16,
// [36] bit mask of the SuperSpeed link states seen (GDBGLTSSM bits 25:22), [37] GDBGLTSSM, [38] GUSB3PIPECTL, [39] cable test steps.
volatile uint32_t native_usb_stats[54];
}
namespace {
// The DWC3 global, device and OTG registers in the window Apple's class addresses: base + 0xc000.
constexpr uint64_t kCore = 0x0a600000, kAppleWindow = kCore + 0xc000, kQscratch = kCore + 0xf8800, kGcc = 0x100000, kTcsr = 0x1fd5000,
    kPhy = 0x088e3000, kSmmu = 0x15000000, kRsc = 0x17a20000, kCmdDb = 0x81c60000;
constexpr uint32_t kCmdDbSize = 0x20000, kIrq = 133 + 32, kSid = 0x40;
constexpr unsigned kOwnedTcs = 2;   // shared with the microphone rail under the same lock
struct Region {IOMemoryMap *map; volatile uint32_t *va;};
Region gGcc, gScratch, gPhy, gTcsrMap;
bool gSuperSpeed;
void stage(int value) {native_usb_stats[0] = (uint32_t)value;}
bool mapRegion(Region &r, uint64_t pa, uint64_t bytes) {
    IODeviceMemory *d = IODeviceMemory::withRange(pa, bytes);
    r.map = d ? d->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(d);
    r.va = r.map ? reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()) : nullptr;
    return r.va != nullptr;
}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
bool waitBits(volatile uint32_t *reg, uint32_t mask, uint32_t value, unsigned ms) {
    for (unsigned i = 0; i < ms * 10; ++i) {
        if ((*reg & mask) == value) return true;
        IODelay(100);
    }
    return (*reg & mask) == value;
}
// Rails of the eUSB2 PHY, its repeater and the QMP PHY, the reference clock and the path to memory (dwc3-qcom usb-ddr).
bool powerRails() {
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    if (!db) return false;
    Region dbw = {}, rsc = {};
    bool ok = mapRegion(dbw, kCmdDb, kCmdDbSize) && mapRegion(rsc, kRsc, 0x10000);
    if (ok) {
        for (uint32_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = dbw.va[i / 4]; memcpy(db + i, &w, 4);}
        const QcomRscLayout layout = qcom_rsc_layout(rsc.va[0]);
        AppsSmmuLock guard;
        auto send = [&](uint32_t address, uint32_t value) {
            QcomRpmhWrite writes[16];
            const unsigned n = qcom_rpmh_active_request(&layout, kOwnedTcs, &address, &value, 1, writes, 16);
            for (unsigned i = 0; i < n; ++i) rsc.va[(0xd00 + writes[i].offset) / 4] = writes[i].value;
            IOSleep(2);
            const unsigned m = qcom_rpmh_finish(&layout, kOwnedTcs, writes, 16);
            for (unsigned i = 0; i < m; ++i) rsc.va[(0xd00 + writes[i].offset) / 4] = writes[i].value;
        };
        static const struct {const char *name; uint32_t mv;} rails[] = {
            {"ldoe1", 880}, {"ldoe3", 1200}, {"ldob15", 1800}, {"ldob5", 3104}, {"ldof3", 912}};
        for (unsigned i = 0; i < sizeof rails / sizeof rails[0] && ok; ++i) {
            QcomRpmhResource res;
            if (qcom_cmd_db_find(db, kCmdDbSize, rails[i].name, &res)) {ok = false; break;}
            send(res.address + 8, 7);
            send(res.address, rails[i].mv);
            send(res.address + 4, 1);
            native_usb_stats[9] = native_usb_stats[9] | (1u << i);
        }
        QcomRpmhResource res;
        if (ok && !qcom_cmd_db_find(db, kCmdDbSize, "xo.lvl", &res)) send(res.address, 3); else ok = false;
        // SN2 carries qnm_aggre1_noc: vote = ceil(ceil(kBps * width / buswidth) * 1000 / unit), buswidth 16, 2500 MB/s peak.
        if (ok && !qcom_cmd_db_find(db, kCmdDbSize, "SN2", &res)) {
            uint32_t vote = 0x400;
            if (res.level_count >= 3 && (res.levels[0] | (uint32_t)res.levels[1] << 16) && res.levels[2]) {
                const uint64_t unit = res.levels[0] | (uint32_t)res.levels[1] << 16;
                const uint64_t x = (((2500000ull * res.levels[2] + 15) / 16) * 1000 + unit - 1) / unit;
                vote = x < 1 ? 1 : x > 0x3fff ? 0x3fff : (uint32_t)x;
            }
            send(res.address, 1u << 30 | 1u << 29 | vote << 14 | vote);
            native_usb_stats[9] = native_usb_stats[9] | 0x80000000u;
        } else ok = false;
    }
    if (dbw.map) dbw.map->release();
    if (rsc.map) rsc.map->release();
    IOFreeData(db, kCmdDbSize);
    return ok;
}
bool gdsc(uint32_t offset, unsigned statIndex) {
    volatile uint32_t *reg = gGcc.va + offset / 4;
    if (!sm8550_gcc_update32(reg, 1u, 1u << 11)) return false;   // SW_COLLAPSE off, RETAIN_FF_ENABLE on
    const bool ok = waitBits(reg, 0x80000000u, 0x80000000u, 100);
    native_usb_stats[statIndex] = *reg;
    return ok;
}
bool branch(uint32_t offset) {
    volatile uint32_t *reg = gGcc.va + offset / 4;
    if (!sm8550_gcc_update32(reg, 0, 1u)) return false;
    return waitBits(reg, 0x80000000u, 0, 100);
}
bool rcg(uint32_t cmd, uint32_t cfg) {
    volatile uint32_t *c = gGcc.va + cmd / 4;
    if ((c[1] & 0x71fu) == cfg) return true;
    c[1] = (c[1] & ~0x71fu) | cfg;
    barrier();
    c[0] = c[0] | 1u;
    barrier();
    return waitBits(c, 1u, 0, 10);
}
bool clocksAndResets() {
    // gcc-sm8550.c: USB30_PRIM_BCR 0x39000, QUSB2PHY_PRIM_BCR 0x12000; the core reset of dwc3_qcom_probe, the PHY reset of its init.
    volatile uint32_t *core = gGcc.va + 0x39000 / 4, *phy = gGcc.va + 0x12000 / 4;
    *core = *core | 1u; barrier(); IODelay(20);
    *core = *core & ~1u; barrier();
    // Master 200 MHz from GPLL0 (source 1, half divider 5), mock UTMI 19.2 MHz from XO, as the dtsi assigns them.
    if (!rcg(0x3902c, 1u << 8 | 5u) || !rcg(0x39044, 0)) return false;
    if (!branch(0x39088) || !branch(0x39018) || !branch(0x3908c) || !branch(0x39024) || !branch(0x39028)) return false;
    gTcsrMap.va[0x118 / 4] = gTcsrMap.va[0x118 / 4] | 1u;
    gTcsrMap.va[0x108 / 4] = gTcsrMap.va[0x108 / 4] | 1u;
    barrier();
    *phy = *phy | 1u; barrier(); IODelay(150);
    *phy = *phy & ~1u; barrier();
    return true;
}
// SPMI arbiter v7 of this board (spmi@c400000, EE 0): one byte through this EE's channels.
struct Spmi {
    volatile uint32_t *core, *cnfg, *obs, *chn;
    uint64_t obsBytes, chnBytes;
    int apid;
};
uint32_t spmiCmd(uint8_t op, uint8_t addr) {return (uint32_t)op << 27 | (uint32_t)addr << 4;}
bool spmiWait(volatile uint32_t *ch) {
    for (unsigned i = 0; i < 100; ++i) {
        const uint32_t status = ch[2];
        if (status) return status == 1;
        IODelay(10);
    }
    return false;
}
bool spmiRead(const Spmi &s, uint8_t addr, uint8_t *out) {
    volatile uint32_t *ch = s.obs + (0x20u * (uint32_t)s.apid) / 4;
    ch[0] = spmiCmd(1, addr);
    if (!spmiWait(ch)) return false;
    *out = (uint8_t)ch[6];
    return true;
}
bool spmiWrite(const Spmi &s, uint8_t addr, uint8_t value) {
    volatile uint32_t *ch = s.chn + (0x1000u * (uint32_t)s.apid) / 4;
    ch[4] = value;
    barrier();
    ch[0] = spmiCmd(0, addr);
    return spmiWait(ch);
}
// init powers it and tunes it; every call sets the mode: eusb2_repeater_set_mode, host forces the 19.2 MHz clock enable on.
bool repeater(bool init, bool host) {
    Region core = {}, cnfg = {}, obs = {}, chn = {};
    if (!mapRegion(core, 0xc400000, 0x3000) || !mapRegion(cnfg, 0xc42d000, 0x1000 * 2)) return false;
    Spmi s = {core.va, cnfg.va, nullptr, nullptr, 0, 0, -1};
    const uint32_t count = core.va[0x4 / 4] & 0x7ffu;
    for (uint32_t i = 0; i < count && i < 1024; ++i) {
        const uint32_t map = core.va[(0x2000 + 4 * i) / 4];
        if (((map >> 8) & 0xfffu) != 0x7fdu) continue;   // SID 7, peripheral 0xfd
        const int owner = (int)(cnfg.va[i] & 7u);
        if (s.apid < 0 || owner == 0) s.apid = (int)i;
    }
    bool ok = s.apid >= 0 && mapRegion(obs, 0xc440000, 0x20ull * (uint32_t)(s.apid + 1)) && mapRegion(chn, 0xc500000, 0x1000ull * (uint32_t)(s.apid + 1));
    if (ok) {
        s.obs = obs.va; s.chn = chn.va;
        if (init) {
            ok = spmiWrite(s, kQcomRepeaterEnable, kQcomRepeaterEnableBit);
            for (const auto &t : kQcomRepeaterTune) ok = ok && spmiWrite(s, t.offset, t.value);
        }
        const uint8_t force = host ? 1u << 6 : 0;   // F_CLK_19P2M_EN, V_CLK_19P2M_EN
        ok = ok && spmiWrite(s, kQcomRepeaterForceEn5, force) && spmiWrite(s, kQcomRepeaterForceVal5, force);
        if (init) {
            IODelay(50);
            uint8_t status = 0;
            ok = ok && spmiRead(s, kQcomRepeaterStatus, &status);
            native_usb_stats[8] = status | (uint32_t)s.apid << 16;
            ok = ok && (status & kQcomRepeaterOk);
        }
    }
    Region *regions[] = {&core, &cnfg, &obs, &chn};
    for (Region *r : regions) if (r->map) r->map->release();
    return ok;
}
void phyInit() {
    for (const auto &w : kQcomEusb2Init) {
        volatile uint32_t *reg = gPhy.va + w.offset / 4;
        *reg = (*reg & ~(uint32_t)w.mask) | w.value;
        barrier();
    }
}
// dwc3_qcom_select_utmi_clk: no PIPE clock until the QMP PHY runs.
void selectUtmiClock() {
    volatile uint32_t *cfg = gScratch.va + 0x8 / 4;
    *cfg = *cfg | 1u << 8; barrier(); IOSleep(1);
    *cfg = *cfg | 1u | 1u << 3; barrier(); IOSleep(1);
    *cfg = *cfg & ~(1u << 8); barrier();
}
void vbusOverride(bool on) {
    volatile uint32_t *ss = gScratch.va + 0x30 / 4, *hs = gScratch.va + 0x10 / 4;
    const uint32_t ssBits = 1u << 24, hsBits = 1u << 20 | 1u << 28;
    if (on) {*ss = *ss | ssBits; *hs = *hs | hsBits;} else {*ss = *ss & ~ssBits; *hs = *hs & ~hsBits;}
    barrier();
    native_usb_stats[10] = *hs;
}
// PS5169 retimer on i2c@a80000 (wrap 1 engine 0), address 0x28, reset GPIO 7 active low: the sequence of Linux ps5169.c for USB only.
uint32_t i2cRead(void *ctx, uint32_t reg) {return static_cast<volatile uint32_t *>(ctx)[reg / 4];}
void i2cWrite(void *ctx, uint32_t reg, uint32_t value) {static_cast<volatile uint32_t *>(ctx)[reg / 4] = value; barrier();}
bool i2cFinish(GeniI2c *bus, volatile uint32_t *se) {
    for (unsigned step = 0; step < 3; ++step) {
        for (unsigned i = 0; i < 2500; ++i) {
            const uint32_t status = se[GENI_M_IRQ_STATUS / 4];
            if (status) {
                if (status & GENI_M_TX_WATERMARK) se[GENI_TX_WATERMARK / 4] = 0;
                se[GENI_M_IRQ_CLEAR / 4] = status;
                const int result = geni_i2c_event(bus, status);
                if (result == GeniDone) return true;
                if (result == GeniFailed) return false;
            }
            IODelay(10);
        }
        const int result = geni_i2c_timeout(bus);
        if (result != GeniPending) return false;
    }
    return false;
}
bool retimerWrite(GeniI2c *bus, volatile uint32_t *se, uint8_t reg, uint8_t value) {
    return geni_i2c_start_write(bus, reg, value) == 0 && i2cFinish(bus, se);
}
bool retimerInit(bool reverse) {
    Region tlmm = {}, engine = {};
    bool ok = mapRegion(tlmm, 0xf100000, 0x30000) && mapRegion(engine, 0xa80000, 0x4000);
    native_usb_stats[32] = 1u << 16;
    if (!ok) return false;
    // Reset released, then wait out the part's start (Linux: 20 ms).
    tlmm.va[(7 * 0x1000 + 4) / 4] = 1u << 1; barrier();
    tlmm.va[(7 * 0x1000) / 4] = 1u << 9; barrier();
    // qup1_se0 on GPIO 28, 29 with pull up, the serial engine clock and wrap 1's bus clocks on (BRANCH_HALT_VOTED).
    tlmm.va[(28 * 0x1000) / 4] = 3u | 1u << 2; tlmm.va[(29 * 0x1000) / 4] = 3u | 1u << 2; barrier();
    sm8550_gcc_update32(gGcc.va + 0x52008 / 4, 0, 1u << 22 | 1u << 18 | 1u << 19 | 1u << 20 | 1u << 21);
    IOSleep(20);
    native_usb_stats[32] = 2u << 16;
    // Nothing brought this engine up before: geni_se_init, the clock from the DFS table's XO level, SCL at 100 kHz from a 19.2 MHz clock
    // (geni_i2c_clk_map_19p2mhz; the retimer takes up to 400 kHz, so this stays in spec if the level runs faster).
    volatile uint32_t *se = engine.va;
    unsigned level = 8;
    for (unsigned i = 0; i < 8 && level == 8; ++i) {
        const uint32_t cfg = gGcc.va[(0x18010 + 0x1c + 4 * i) / 4];
        if (((cfg >> 8) & 7u) == 0 && (cfg & 31u) <= 1) level = i;
    }
    if (level == 8) {native_usb_stats[32] = 9u << 16; return false;}
    se[0x28 / 4] = se[0x28 / 4] | 0x7fu;                                        // CGC
    se[0xe30 / 4] = se[0xe30 / 4] | 0xfu;                                       // DMA clock gating
    se[0x24 / 4] = 0x7fu;                                                       // output control
    se[0x20 / 4] = 1u;                                                          // force default
    se[0xe18 / 4] = 0;                                                          // no GSI events
    se[0xe1c / 4] = se[0xe1c / 4] | 0xcu;                                       // M and S IRQ out of the FIFO block
    se[0x7c / 4] = level;
    se[GENI_CLK_CFG / 4] = 7u << 4 | 1u;
    se[GENI_SCL_COUNTERS / 4] = 10u << 20 | 12u << 10 | 26u;
    barrier();
    static GeniI2c bus;
    const GeniIo io = {i2cRead, i2cWrite, const_cast<uint32_t *>(engine.va)};
    uint32_t config = 0;
    if (geni_i2c_prepare(&bus, &io, 0x28, &config)) {native_usb_stats[32] = 3u << 16 | (config >> 24); return false;}
    uint32_t id = 0;
    for (unsigned i = 0; i < 2; ++i) {
        if (geni_i2c_start_read(&bus, (uint8_t)(0xac + i)) || !i2cFinish(&bus, engine.va)) {native_usb_stats[32] = (4u + i) << 16; return false;}
        id |= (uint32_t)bus.data << (8 * i);
    }
    native_usb_stats[32] = 6u << 16 | id;
    static const uint8_t table[][2] = {{0x8d, 1}, {0x90, 1}, {0x51, 0x87}, {0x50, 0x20}, {0x54, 0x11}, {0x5d, 0x66}, {0x52, 0x50}, {0x55, 0}, {0x56, 0},
        {0x57, 0}, {0x58, 0}, {0x59, 0}, {0x5a, 0}, {0x5b, 0}, {0x5e, 6}, {0x5f, 0}, {0x60, 0}, {0x61, 3}, {0x65, 0x40}, {0x66, 0}, {0x67, 3}, {0x75, 0x0c},
        {0x77, 0}, {0x78, 0x7c}};
    ok = retimerWrite(&bus, engine.va, 0x9d, 0x80);
    IOSleep(10);
    ok = ok && retimerWrite(&bus, engine.va, 0x9d, 0) && retimerWrite(&bus, engine.va, 0x40, 0x80) && retimerWrite(&bus, engine.va, 0xa0, 2);
    for (const auto &row : table) ok = ok && retimerWrite(&bus, engine.va, row[0], row[1]);
    ok = ok && retimerWrite(&bus, engine.va, 0x40, reverse ? 0xd0 : 0xc0);
    native_usb_stats[32] = (ok ? 7u : 8u) << 16 | id;
    return ok;
}
// QMP USB3/DP combo PHY at 0x88e8000 in USB 3 only mode: qmp_combo_com_init and qmp_combo_usb_power_on.
bool qmpInit(bool reverse) {
    Region phy = {};
    if (!mapRegion(phy, 0x088e8000, 0x3000)) return false;
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(phy.va);
    auto rd = [&](uint32_t o) {return *reinterpret_cast<volatile uint32_t *>(base + o);};
    auto wr = [&](uint32_t o, uint32_t v) {*reinterpret_cast<volatile uint32_t *>(base + o) = v; barrier();};
    volatile uint32_t *reset = gGcc.va + 0x50000 / 4, *common = gGcc.va + 0x50008 / 4;
    *reset = *reset | 1u; *common = *common | 1u; barrier(); IODelay(20);
    *reset = *reset & ~1u; *common = *common & ~1u; barrier();
    if (!rcg(0x39070, 0) || !branch(0x39060) || !branch(0x39064)) return false;
    sm8550_gcc_update32(gGcc.va + 0x3906c / 4, 3u, 0);                          // the pipe clock comes from the PHY
    sm8550_gcc_update32(gGcc.va + 0x39068 / 4, 0, 1u);
    wr(kQmpComPower, rd(kQmpComPower) | 1u);
    wr(kQmpComResetOvrd, rd(kQmpComResetOvrd) | 0xfu);
    wr(kQmpComTypec, 1u << 1 | (reverse ? 1u : 0));                             // software port select
    wr(kQmpComMode, 1u);                                                        // USB3_MODE
    wr(kQmpComResetOvrd, rd(kQmpComResetOvrd) & ~0xcu);
    wr(kQmpComSwi, rd(kQmpComSwi) & ~3u);
    wr(kQmpComReset, rd(kQmpComReset) & ~1u);
    wr(kQmpPowerDown, rd(kQmpPowerDown) | 1u);
    for (const auto &w : kQmpUsb3Init) wr(w.offset, w.value);
    wr(kQmpReset, rd(kQmpReset) & ~1u);
    wr(kQmpStart, rd(kQmpStart) | 3u);
    bool ok = false;
    for (unsigned i = 0; i < 1000 && !ok; ++i) {ok = !(rd(kQmpStatus) & 1u << 6); if (!ok) IODelay(200);}
    native_usb_stats[33] = rd(kQmpStatus);
    return ok;
}
// The USB 3 path, or false for USB 2 only with the UTMI clock in place of the PIPE clock (a USB 2 cable still works on the former).
bool superSpeedInit() {
    Region tlmm = {};
    if (!mapRegion(tlmm, 0xf100000, 0x10000)) return false;
    const bool reverse = tlmm.va[(11 * 0x1000 + 4) / 4] & 1u;                   // orientation-gpios, tlmm 11, active high
    native_usb_stats[34] = reverse;
    tlmm.map->release();
    return retimerInit(reverse) && qmpInit(reverse);
}
Region gSmmu;
// Apple's controller driver does no cache maintenance: Apple's fabric is coherent. Here coherence comes from the SMMU's output
// attributes, as in Linux (dma-coherent, IOMMU_CACHE page tables): a stage 1 context with an identity map of DRAM in 1 GiB blocks,
// Normal write-back inner shareable. The bypass bank keeps device attributes and leaves this master non-coherent. No IOMapper:
// IODMACommand hands the physical address to the core, which the identity map turns back into the same address.
IOBufferMemoryDescriptor *gSmmuTable;
unsigned gSmmuBank, gSmmuPages;
constexpr uint32_t kCbSctlr = 0x0, kCbTcr2 = 0x10, kCbTtbr0 = 0x20, kCbTtbr1 = 0x28, kCbTcr = 0x30, kCbMair0 = 0x38, kCbMair1 = 0x3c, kCbFsr = 0x58,
    kCbTlbiAsid = 0x610, kCbTlbSync = 0x7f0, kCbTlbStatus = 0x7f4;
bool smmuIdentity() {
    if (!mapRegion(gSmmu, kSmmu, 0xd0000)) return false;
    AppsSmmuLock guard;
    if (!guard.ready()) return false;
    const uint32_t idr0 = gSmmu.va[0x20 / 4], idr1 = gSmmu.va[0x24 / 4];
    unsigned groups = idr0 & 255;
    if (groups > 128) groups = 128;
    const unsigned banks = idr1 & 255, pages = 1u << (((idr1 >> 28) & 7) + 1);
    if ((idr1 >> 31) || !groups || banks < 2 || banks > 128 || pages > 128 || (pages + banks) * 4096u > 0xd0000u) return false;
    uint64_t used[2] = {};
    unsigned slot = 0xffffffffu;
    for (unsigned i = 0; i < groups; ++i) {
        const uint32_t smr = gSmmu.va[(0x800 + 4 * i) / 4];
        if (!(smr & 0x80000000u)) {if (slot == 0xffffffffu) slot = i; continue;}
        const uint32_t s2cr = gSmmu.va[(0xc00 + 4 * i) / 4];
        if (((smr ^ kSid) & 0xffff & ~((smr >> 16) & 0x7fff)) == 0) {IOLog("qcom-dwc3: stream already mapped %x %x\n", smr, s2cr); return false;}
        if (((s2cr >> 16) & 3) == 0) used[(s2cr & 127) / 64] |= 1ull << ((s2cr & 127) % 64);
    }
    if (slot == 0xffffffffu) return false;
    // The last bank is the shared bypass bank. Take the highest bank no stream uses and no firmware left enabled.
    unsigned bank = 0xffffffffu;
    for (unsigned b = banks - 1; b-- > 0;) {
        if ((used[b / 64] >> (b % 64)) & 1) continue;
        if (gSmmu.va[(pages + b) * 1024] & 1u) continue;
        bank = b;
        break;
    }
    if (bank == 0xffffffffu) return false;
    gSmmuTable = IOBufferMemoryDescriptor::withOptions((IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, 4096, 4096);
    if (!gSmmuTable || gSmmuTable->prepare() != kIOReturnSuccess) return false;
    uint64_t *table = static_cast<uint64_t *>(gSmmuTable->getBytesNoCopy());
    bzero(table, 4096);
    // T0SZ 28: 64 GiB, the walk starts at level 1. Blocks: AF, inner shareable, AP unprivileged read/write, attribute 0. Below 2 GiB
    // there is no DRAM, and a stray access should fault.
    for (uint64_t i = 2; i < 64; ++i) table[i] = i << 30 | 1ull << 10 | 3ull << 8 | 1ull << 6 | 1ull;
    for (uintptr_t at = reinterpret_cast<uintptr_t>(table); at < reinterpret_cast<uintptr_t>(table) + 4096; at += 64) __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
    barrier();
    const uint64_t tablePa = gSmmuTable->getPhysicalSegment(0, nullptr, 0);
    volatile uint32_t *cb = gSmmu.va + (pages + bank) * 1024;
    gSmmu.va[(0x1800 + 4 * bank) / 4] = 1;                                      // CBA2R: AArch64
    gSmmu.va[(0x1000 + 4 * bank) / 4] = 1u << 16 | 0xfu << 12 | 3u << 8;        // CBAR: stage 1 with stage 2 bypass
    cb[kCbTcr2 / 4] = 2u | 7u << 15;                                            // 40-bit output, upstream sign extension
    cb[kCbTcr / 4] = 28u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23;            // walks cacheable inner shareable, 4 KiB, no TTBR1
    cb[kCbTtbr0 / 4] = (uint32_t)tablePa; cb[kCbTtbr0 / 4 + 1] = (uint32_t)(tablePa >> 32);
    cb[kCbTtbr1 / 4] = 0; cb[kCbTtbr1 / 4 + 1] = 0;
    cb[kCbMair0 / 4] = 0xff; cb[kCbMair1 / 4] = 0;                              // attribute 0: Normal write-back
    barrier();
    cb[kCbTlbiAsid / 4] = 0;
    cb[kCbTlbSync / 4] = 0;
    barrier();
    for (unsigned i = 0; i < 1000 && (cb[kCbTlbStatus / 4] & 1u); ++i) IODelay(10);
    cb[kCbFsr / 4] = cb[kCbFsr / 4];
    cb[kCbSctlr / 4] = 1u | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12;             // M, TRE, AFE, CFRE, ASIDPNE
    barrier();
    gSmmu.va[(0xc00 + 4 * slot) / 4] = bank;                                    // S2CR translate to the bank, then the match
    barrier();
    gSmmu.va[(0x800 + 4 * slot) / 4] = 0x80000000u | kSid;
    barrier();
    native_usb_stats[7] = slot | bank << 16;
    gSmmuBank = bank; gSmmuPages = pages;
    const bool ok = (cb[kCbSctlr / 4] & 1u) && (gSmmu.va[(0xc00 + 4 * slot) / 4] & 255) == bank &&
                    gSmmu.va[(0x800 + 4 * slot) / 4] == (0x80000000u | kSid);
    if (!ok) IOLog("qcom-dwc3: SMMU context readback %x %x %x\n", cb[kCbSctlr / 4], gSmmu.va[(0xc00 + 4 * slot) / 4], gSmmu.va[(0x800 + 4 * slot) / 4]);
    return ok;
}
}  // namespace

// A controller's provider: a register window and the core interrupt, in the IOService terms IOFilterInterruptEventSource uses. Device and
// host have a nub each and share the one line: it goes to the nub of the role the core is in.
class QcomDwc3Nub;
static QcomDwc3Nub *volatile gActiveNub;
static bool gLineAttached;
class QcomDwc3Nub : public IOService {
    OSDeclareDefaultStructors(QcomDwc3Nub)
    IOInterruptAction action = nullptr;
    OSObject *target = nullptr;
    void *refcon = nullptr;
    bool enabled = false;
    static void primary(void *) {
        QcomDwc3Nub *self = gActiveNub;
        native_usb_stats[11] = native_usb_stats[11] + 1;
        if (self && self->action) self->action(self->target, self->refcon, self, 0);
    }
    void unmask() {if (gActiveNub == this) armcpu_spi_mask(kIrq, !enabled);}
public:
    static void route(QcomDwc3Nub *nub) {
        armcpu_spi_mask(kIrq, true);
        gActiveNub = nub;
        __asm__ volatile("dmb ish" ::: "memory");
        if (nub) nub->unmask();
    }
    IOReturn getInterruptType(int source, int *type) override {
        if (source != 0 || !type) return kIOReturnNoInterrupt;
        *type = kIOInterruptTypeLevel;
        return kIOReturnSuccess;
    }
    IOReturn registerInterrupt(int source, OSObject *owner, IOInterruptAction handler, void *context) override {
        if (source != 0 || !handler) return kIOReturnNoInterrupt;
        if (action) return kIOReturnNoResources;
        target = owner; refcon = context; action = handler; enabled = false;
        __asm__ volatile("dmb ish" ::: "memory");
        if (!gLineAttached) {
            if (armcpu_spi_attach(kIrq, false, primary, nullptr, nullptr, nullptr) != kIOReturnSuccess) {action = nullptr; return kIOReturnNoResources;}
            gLineAttached = true;
        }
        unmask();
        return kIOReturnSuccess;
    }
    IOReturn unregisterInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = false;
        unmask();
        action = nullptr; target = nullptr; refcon = nullptr;
        return kIOReturnSuccess;
    }
    IOReturn enableInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = true;
        unmask();
        return kIOReturnSuccess;
    }
    IOReturn disableInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = false;
        unmask();
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(QcomDwc3Nub, IOService)

// Whether the device stack may have the core: false while the core is in the host role or going there.
static volatile bool gDeviceStackAllowed = true;
class QcomDwc3Xdci : public AppleUSBXDCI {
    OSDeclareDefaultStructors(QcomDwc3Xdci)
    uint32_t read(uint32_t offset) {return (*reinterpret_cast<volatile uint32_t **>(reinterpret_cast<uint8_t *>(this) + 0x450))[offset / 4];}
    void write(uint32_t offset, uint32_t value) {(*reinterpret_cast<volatile uint32_t **>(reinterpret_cast<uint8_t *>(this) + 0x450))[offset / 4] = value; barrier();}
public:
    bool start(IOService *provider) override {
        uint8_t *self = reinterpret_cast<uint8_t *>(this);
        IOMemoryMap *map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
        if (!map) return false;
        *reinterpret_cast<IOMemoryMap **>(self + 0x448) = map;
        *reinterpret_cast<uint64_t *>(self + 0x450) = map->getVirtualAddress();
        return AppleUSBXDCI::start(provider);
    }
    // True keeps the family's already selected configuration list (IOUSBDeviceController builds it when this is false): Apple's SoC
    // classes answer from their IOPort transport object, and this class has none. The USB 3 descriptors follow the speed instead.
    bool XDCI_KEEP_CONFIGURATIONS() override {return false;}
    bool XDCI_TRANSPORT_ACTIVE() override {return true;}
    // The power the family asks for at each change, including when configd's description starts the stack: off while the host role has the core.
    uint32_t XDCI_DESIRED_POWER() override {return gDeviceStackAllowed ? AppleUSBXDCI::XDCI_DESIRED_POWER() : 0;}
    IOReturn XDCI_POWER_ON() override {
        native_usb_stats[12] = native_usb_stats[12] + 1;
        vbusOverride(true);
        native_usb_stats[15] = read(0x70c);
        return kIOReturnSuccess;
    }
    IOReturn XDCI_POWER_OFF() override {
        native_usb_stats[13] = native_usb_stats[13] + 1;
        vbusOverride(false);
        return kIOReturnSuccess;
    }
    // One event buffer and interrupter; the Linux driver has one as well. The scale and the periodic count are read in the reset.
    void XDCI_PARAMETERS() override {
        uint32_t *fields = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x2628);
        fields[0] = 1; fields[1] = 0; fields[2] = 4; fields[3] = 4;
    }
    IOReturn XDCI_CONTROLLER_RESET() override {
        const IOReturn result = AppleUSBXDCI::XDCI_CONTROLLER_RESET();
        native_usb_stats[14] = (uint32_t)result;
        if (result != kIOReturnSuccess) return result;
        uint32_t *fields = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x2628);
        // dwc3_core_setup_global_control keeps the power down scale the hardware resets to when there is no suspend clock.
        const uint32_t gctl = read(0x110);
        fields[1] = gctl >> 19;
        write(0x110, (gctl & ~(3u << 12)) | 2u << 12);
        // dwc3_gadget_setup_nump: RAM2 bytes less 24 for the event overhead and 16 per setup, in 1 KiB packets, at most 16.
        const uint32_t hw0 = read(0x140), hw6 = read(0x158), hw7 = read(0x15c);
        const uint32_t mdwidth = ((hw0 >> 8) & 0xffu) | ((hw6 >> 8) & 3u) << 8;
        const uint32_t ram2 = hw7 >> 16;
        uint32_t nump = ram2 * mdwidth / 8 > 40 ? (ram2 * mdwidth / 8 - 40) / 1024 : 1;
        fields[2] = nump > 16 ? 16 : nump ? nump : 1;
        native_usb_stats[2] = hw0; native_usb_stats[3] = read(0x14c); native_usb_stats[4] = hw7;
        // snps,dis_u2_susphy_quirk, snps,dis_enblslpm_quirk: this board never suspends the eUSB2 PHY clock from the core.
        write(0x200, read(0x200) & ~(1u << 6 | 1u << 8));
        write(0x2c0, read(0x2c0) & ~(1u << 17));
        if (!gSuperSpeed) write(0x700, read(0x700) & ~7u);   // High speed
        native_usb_stats[16] = read(0x700); native_usb_stats[17] = read(0x110); native_usb_stats[18] = read(0x200);
        return result;
    }
    // Bandwidth accounting of Apple's fabric: nothing to vote for on this SoC, the SN2 vote covers the peak.
    // The family's power states: 4 starts the bus (the cable-connected path of Apple's SoC classes), 0 takes it off.
    IOReturn cable(bool connected) {return changePowerStateTo(connected ? 4 : 0);}
    uint32_t XDCI_PERIODIC_USAGE() override {return *reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x58c) != 0;}
    void XDCI_PERIODIC_SET(uint32_t) override {}
};
OSDefineMetaClassAndStructors(QcomDwc3Xdci, AppleUSBXDCI)

// Host mode: Apple's xHCI driver as it is on a PCI controller, with what Linux's dwc3 host glue requires of the core.
class QcomDwc3Xhci : public AppleUSBXHCI {
    OSDeclareDefaultStructors(QcomDwc3Xhci)
public:
    // Once the ports exist the family leaves the controller's power to them, and an idle port lets it fall to off: a halted controller reports
    // no attach, and what wakes it on Apple's hardware is the Type-C transport's cable report, which this class does not have. The role follows
    // the port manager, so this controller exists only with a peripheral on the cable: kPowerStateOn for as long as it exists.
    uint32_t XHCI_DESIRED_POWER() override {return 3;}
    // write-64-hi-lo-quirk: a 64-bit register is two 32-bit accesses, the high half written first.
    uint64_t XHCI_READ64(void *base, uint32_t offset) override {
        volatile uint32_t *at = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(base) + offset);
        const uint64_t low = at[0];
        return low | (uint64_t)at[1] << 32;
    }
    void XHCI_WRITE64(void *base, uint32_t offset, uint64_t value) override {
        volatile uint32_t *at = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(base) + offset);
        at[1] = (uint32_t)(value >> 32);
        barrier();
        at[0] = (uint32_t)value;
        barrier();
    }
};
OSDefineMetaClassAndStructors(QcomDwc3Xhci, AppleUSBXHCI)

namespace {
bool bringUp() {
    stage(1);
    if (!mapRegion(gGcc, kGcc, 0x1f0000) || !mapRegion(gTcsrMap, kTcsr, 0x1000) || !mapRegion(gPhy, kPhy, 0x1000) ||
        !mapRegion(gScratch, kQscratch & ~0xfffull, 0x1000)) {stage(-1); return false;}
    gScratch.va += (kQscratch & 0xfff) / 4;
    stage(2);
    if (!powerRails()) {stage(-2); return false;}
    stage(3);
    if (!gdsc(0x50018, 6) || !gdsc(0x39004, 5)) {stage(-3); return false;}
    stage(4);
    if (!clocksAndResets()) {stage(-4); return false;}
    stage(5);
    if (!smmuIdentity()) {stage(-5); return false;}
    stage(6);
    if (!repeater(true, false)) {stage(-6); return false;}
    stage(7);
    phyInit();
    IOSleep(2);
    gSuperSpeed = superSpeedInit();
    if (!gSuperSpeed) selectUtmiClock();
    stage(8);
    Region window = {};
    if (!mapRegion(window, kAppleWindow, 0x1000)) {stage(-8); return false;}
    const uint32_t id = window.va[0x120 / 4];
    native_usb_stats[1] = id;
    window.map->release();
    if ((id & 0xffff0000u) != 0x55330000u && (id & 0xffff0000u) != 0x33310000u && (id & 0xffff0000u) != 0x33320000u) {stage(-9); return false;}
    stage(9);
    return true;
}
}  // namespace
QcomDwc3Xdci *gController;
int gVbusOn = -1, gVbusApplied = -1;
QcomUcsi gUcsi;
// One role at a time, changed on one thread: the system's start and stop paths block, and the battery work loop reports the cable.
enum Role {kRoleNone, kRoleDevice, kRoleHost};
IOLock *gRoleLock;
int gRoleWanted, gRoleApplied, gCoreRole = kRoleDevice;
bool gForceHost;
Region gGlobals, gXhciRegs;
QcomDwc3Nub *gDeviceNub, *gHostNub;
QcomDwc3Xhci *gHost;
void wantRole(int role) {
    if (!gRoleLock) return;
    IOLockLock(gRoleLock);
    gRoleWanted = role;
    IOLockWakeup(gRoleLock, &gRoleWanted, false);
    IOLockUnlock(gRoleLock);
}
// What is known now, by ucsi_partner_change: the port manager when it answered, which tells a USB host from a charger and a peripheral; the
// charger firmware's online value until then, or if UCSI does not come up.
void cableChanged() {
    int role = gVbusOn > 0 ? kRoleDevice : kRoleNone;
    if (gUcsi.status_valid) {
        role = kRoleNone;
        if (gUcsi.connected && gUcsi.usb) {
            if (gUcsi.partner == kUcsiPartnerDfp) role = kRoleDevice;
            else if (gUcsi.partner == kUcsiPartnerUfp || gUcsi.partner == kUcsiPartnerCableUfp) role = kRoleHost;
        }
    }
    wantRole(gForceHost ? kRoleHost : role);
}
void forceHostThread(void *seconds, wait_result_t) {
    IOSleep((uint32_t)reinterpret_cast<uintptr_t>(seconds) * 1000);
    gForceHost = true;
    cableChanged();
    // sheng-usb-back=<seconds in the host role>: then what the cable says, for an unattended run of the way back.
    unsigned back = 0;
    if (PE_parse_boot_argn("sheng-usb-back", &back, sizeof back) && back) {
        IOSleep(back * 1000);
        gForceHost = false;
        cableChanged();
    }
}
void leaveRole(int role) {
    if (role == kRoleDevice) {
        IOService::getResourceService()->removeProperty("AppleUSBCableType");
        // The family stops the bus through XDCI_POWER_OFF; with no description from configd yet the bus was never on and it does not come.
        const uint32_t before = native_usb_stats[13];
        native_usb_stats[20] = (uint32_t)gController->cable(false);
        for (unsigned i = 0; i < 200 && native_usb_stats[13] == before; ++i) IOSleep(10);
    } else if (role == kRoleHost && gHostNub) {
        // A removed PCI controller's path: the family stops its devices and the controller.
        gHostNub->terminate(kIOServiceSynchronous);
        gHostNub->release();
        gHostNub = nullptr; gHost = nullptr;
        QcomDwc3Nub::route(nullptr);
    }
}
// __dwc3_set_mode: a core soft reset between the roles, then the port capability.
void setCoreRole(int role) {
    if (gCoreRole == role) return;
    volatile uint32_t *gctl = gGlobals.va + 0x110 / 4;
    QcomDwc3Nub::route(nullptr);
    *gctl = *gctl | 1u << 11;
    barrier();
    IOSleep(100);
    *gctl = *gctl & ~(1u << 11);
    barrier();
    *gctl = (*gctl & ~(3u << 12)) | (role == kRoleHost ? 1u : 2u) << 12;
    barrier();
    // snps,dis_u2_susphy_quirk, snps,dis_enblslpm_quirk, as in the device reset.
    gGlobals.va[0x200 / 4] = gGlobals.va[0x200 / 4] & ~(1u << 6 | 1u << 8);
    gGlobals.va[0x2c0 / 4] = gGlobals.va[0x2c0 / 4] & ~(1u << 17);
    barrier();
    gCoreRole = role;
    gDeviceStackAllowed = role == kRoleDevice;
    repeater(false, role == kRoleHost);
}
// qmp_combo_typec_switch_set and ps5169: the lanes follow the plug. The GPIO means something once a cable is in.
void followOrientation() {
    Region tlmm = {};
    if (!mapRegion(tlmm, 0xf100000, 0x10000)) return;
    const uint32_t reverse = tlmm.va[(11 * 0x1000 + 4) / 4] & 1u;
    tlmm.map->release();
    if (gSuperSpeed && reverse != native_usb_stats[34]) {
        gSuperSpeed = superSpeedInit();
        if (!gSuperSpeed) selectUtmiClock();
    }
}
void enterHost() {
    QcomDwc3Nub *nub = OSTypeAlloc(QcomDwc3Nub);
    // DWC3_XHCI_REGS_END: the host registers below the global ones.
    IODeviceMemory *range = IODeviceMemory::withRange(kCore, 0x8000);
    OSArray *ranges = range ? OSArray::withObjects((const OSObject **)&range, 1) : nullptr;
    OSData *zero = OSData::withBytes("\0\0\0\0", 4);
    bool done = false;
    if (nub && nub->init() && ranges && zero) {
        nub->setDeviceMemory(ranges);
        nub->setName("usb-host0");
        nub->setProperty("bus-number", zero);
        if (nub->attach(IOService::getPlatform())) {
            nub->registerService();
            QcomDwc3Nub::route(nub);
            QcomDwc3Xhci *host = OSTypeAlloc(QcomDwc3Xhci);
            if (host && host->init() && host->attach(nub)) {
                done = host->start(nub);
                if (!done) host->detach(nub);
                else gHost = host;
            }
            OSSafeReleaseNULL(host);
        }
    }
    native_usb_stats[46] = (native_usb_stats[46] & 0xffff0000u) | (done ? 1u : 2u);
    OSSafeReleaseNULL(ranges); OSSafeReleaseNULL(range); OSSafeReleaseNULL(zero);
    if (done) gHostNub = nub;
    else if (nub) {QcomDwc3Nub::route(nullptr); nub->terminate(kIOServiceSynchronous); nub->release();}
}
void enterRole(int role) {
    if (role == kRoleNone) return;
    followOrientation();
    setCoreRole(role);
    if (role == kRoleHost) {
        vbusOverride(false);
        enterHost();
    } else {
        QcomDwc3Nub::route(gDeviceNub);
        // A USB host on the cable: lockdownd's launch event, then the family goes on the bus when configd's description arrives.
        IOService::getResourceService()->setProperty("AppleUSBCableType", "USBHost");
        native_usb_stats[20] = (uint32_t)gController->cable(true);
    }
}
void hostStats() {
    if (gCoreRole != kRoleHost) return;
    const uint32_t cap = gXhciRegs.va[0];
    const volatile uint32_t *op = gXhciRegs.va + (cap & 0xffu) / 4;
    native_usb_stats[47] = op[0]; native_usb_stats[48] = op[1];
    native_usb_stats[49] = op[0x400 / 4]; native_usb_stats[50] = op[0x410 / 4];
    native_usb_stats[51] = gXhciRegs.va[1];
    if (gHost) native_usb_stats[46] = (native_usb_stats[46] & 0xffff00ffu) | (uint32_t)gHost->getPowerState() << 8;
    // What the system's stack enumerated: how many devices now, the last one's vendor and product (kept after it leaves).
    OSDictionary *match = IOService::serviceMatching("IOUSBHostDevice");
    OSIterator *devices = match ? IOService::getMatchingServices(match) : nullptr;
    uint32_t count = 0;
    while (OSObject *object = devices ? devices->getNextObject() : nullptr) {
        ++count;
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, object);
        OSNumber *vendor = entry ? OSDynamicCast(OSNumber, entry->getProperty("idVendor")) : nullptr;
        OSNumber *product = entry ? OSDynamicCast(OSNumber, entry->getProperty("idProduct")) : nullptr;
        if (vendor && product) native_usb_stats[53] = vendor->unsigned32BitValue() << 16 | (product->unsigned32BitValue() & 0xffffu);
    }
    native_usb_stats[52] = count;
    OSSafeReleaseNULL(devices); OSSafeReleaseNULL(match);
}
void roleThread(void *, wait_result_t) {
    for (;;) {
        IOLockLock(gRoleLock);
        while (gRoleWanted == gRoleApplied) {
            if (gRoleApplied != kRoleHost) IOLockSleep(gRoleLock, &gRoleWanted, THREAD_UNINT);
            else {
                uint64_t deadline;
                clock_interval_to_deadline(1, kSecondScale, &deadline);
                IOLockSleepDeadline(gRoleLock, &gRoleWanted, deadline, THREAD_UNINT);
                hostStats();
            }
        }
        const int role = gRoleWanted;
        IOLockUnlock(gRoleLock);
        if (role == kRoleHost) gDeviceStackAllowed = false;
        leaveRole(gRoleApplied);
        gRoleApplied = kRoleNone;
        enterRole(role);
        gRoleApplied = role;
        native_usb_stats[44] = (uint32_t)role | (uint32_t)gRoleWanted << 8 | (uint32_t)gCoreRole << 16;
        native_usb_stats[45] = native_usb_stats[45] + 1;
        native_usb_stats[35] = ((native_usb_stats[35] & 0xffffu) + 1) | (uint32_t)(role == kRoleDevice) << 16;
    }
}
extern "C" void qcom_dwc3_vbus(int online) {
    gVbusOn = online != 0;
    cableChanged();
}
extern "C" void qcom_dwc3_host_load(int result) {native_usb_stats[46] = (uint32_t)result << 16;}
// The USB-C owner on the PMIC channel; the board's battery service owns the channel and its work loop, and calls these there.
extern "C" uint32_t qcom_ucsi_start(uint8_t *out) {return ucsi_start(&gUcsi, out);}
static void ucsiStats() {
    native_usb_stats[40] = (uint32_t)gUcsi.state | gUcsi.version << 16;
    native_usb_stats[41] = gUcsi.status;
    native_usb_stats[42] = gUcsi.cci;
    native_usb_stats[43] = gUcsi.changes | gUcsi.errors << 16;
}
extern "C" uint32_t qcom_ucsi_receive(const uint8_t *data, uint32_t length, uint8_t *out) {
    const uint32_t n = ucsi_receive(&gUcsi, data, length, out);
    ucsiStats();
    if (gUcsi.state == kUcsiReady && !n) cableChanged();
    return n;
}
extern "C" uint32_t qcom_ucsi_tick(uint8_t *out) {
    const uint32_t n = ucsi_tick(&gUcsi, out);
    ucsiStats();
    return n;
}
extern "C" bool qcom_ucsi_polling(void) {return gUcsi.polling != 0;}
// The hardware, before the classes exist: a failure leaves the system running with USB off.
extern "C" bool qcom_dwc3_bring_up(void) {return bringUp();}
// After the module's metaclasses are registered: publish the provider and start the controller.
// For bring-up (sheng-usb=2): what the core and the SMMU see while the system stack runs, sampled about every 5 ms for the link state.
// cycle (sheng-usb=3) also takes the controller off the bus and back once, the path a cable removal runs, for an unattended host check.
extern "C" void qcom_dwc3_diag(bool cycle) {
    Region window = {};
    if (!mapRegion(window, kAppleWindow, 0x1000) || !gSmmu.va) return;
    const uint32_t pages = gSmmuPages, bank = gSmmuBank;
    for (uint32_t tick = 0;; ++tick) {
        const uint32_t ltssm = window.va[0x164 / 4];
        native_usb_stats[36] = native_usb_stats[36] | 1u << ((ltssm >> 22) & 15u);
        native_usb_stats[37] = ltssm;
        if (!(tick % 400)) {
            native_usb_stats[22] = window.va[0x100 / 4]; native_usb_stats[23] = window.va[0x104 / 4]; native_usb_stats[24] = window.va[0x11c / 4];
            native_usb_stats[25] = gSmmu.va[0x48 / 4];
            native_usb_stats[26] = window.va[0x70c / 4]; native_usb_stats[27] = window.va[0x720 / 4]; native_usb_stats[28] = window.va[0x40c / 4];
            native_usb_stats[29] = window.va[0x80c / 4]; native_usb_stats[30] = gSmmu.va[(pages + bank) * 1024 + 0x58 / 4]; native_usb_stats[31] = window.va[0x408 / 4];
            native_usb_stats[38] = window.va[0x2c0 / 4];
        }
        if (cycle && tick == 16000) {wantRole(kRoleNone); native_usb_stats[39] = 1;}
        if (cycle && tick == 18000) {wantRole(kRoleDevice); native_usb_stats[39] = 2;}
        IOSleep(5);
    }
}
extern "C" void qcom_dwc3_publish(void) {
    do {
        OSData *zero = OSData::withBytes("\0\0\0\0", 1);
        QcomDwc3Nub *nub = OSTypeAlloc(QcomDwc3Nub);
        IOService *platform = IOService::getPlatform();
        IODeviceMemory *range = IODeviceMemory::withRange(kAppleWindow, 0x1000);
        OSArray *ranges = range ? OSArray::withObjects((const OSObject **)&range, 1) : nullptr;
        if (!nub || !nub->init() || !ranges || !zero) {stage(-12); break;}
        nub->setDeviceMemory(ranges);
        nub->setName("usb-drd0");
        nub->setProperty("bus-number", zero);
        nub->setProperty("configuration-string", (void *)"standardMuxOnly", 16);
        // snps,dis-u1-entry-quirk, snps,dis-u2-entry-quirk.
        nub->setProperty("u1-init-disable", true);
        nub->setProperty("u2-init-disable", true);
        if (!nub->attach(platform)) {stage(-13); break;}
        nub->registerService();
        OSSafeReleaseNULL(ranges); OSSafeReleaseNULL(range); OSSafeReleaseNULL(zero);
        stage(10);
        QcomDwc3Xdci *controller = OSTypeAlloc(QcomDwc3Xdci);
        if (!controller || !controller->init()) {stage(-14); break;}
        if (!controller->attach(nub)) {stage(-15); break;}
        const bool started = controller->start(nub);
        native_usb_stats[19] = started;
        if (!started) {stage(-16); break;}
        stage(11);
        gController = controller;
        gDeviceNub = nub;
        gActiveNub = nub;
        // The core stays in the device role until a role asks otherwise: the controller's reset set it.
        thread_t thread = nullptr;
        // sheng-usb-host=<seconds>: the host role that long after this point, whatever is on the cable, to bring the controller up with nothing
        // plugged in and to run the change of role.
        unsigned forced = 0;
        PE_parse_boot_argn("sheng-usb-host", &forced, sizeof forced);
        gRoleLock = IOLockAlloc();
        if (!gRoleLock || !mapRegion(gGlobals, kAppleWindow, 0x1000) || !mapRegion(gXhciRegs, kCore, 0x1000) ||
            kernel_thread_start(roleThread, nullptr, &thread) != KERN_SUCCESS) {stage(-17); break;}
        thread_deallocate(thread);
        if (forced && kernel_thread_start(forceHostThread, reinterpret_cast<void *>((uintptr_t)forced), &thread) == KERN_SUCCESS) thread_deallocate(thread);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        // The cable as the charger firmware reports it; it may have answered before the controller existed. With neither yet, the device
        // role: a host needs no action from this end to be seen.
        if (gVbusOn < 0 && !gUcsi.status_valid) gVbusOn = 1;
        cableChanged();
        stage(12);
    } while (false);
}
