// SPDX-License-Identifier: BSD-2-Clause-Patent
// WiFi bring-up on the sheng board. Stage 1 (boot argument sheng-wifi=1) is a read-only look at what the boot loaders left of the WCN7850
// path. Stage 2 (sheng-wifi=2) powers it and trains the PCIe0 link: WLAN_EN, the PCIe0 and PHY GDSCs, clocks, block resets, the QMP PHY
// tables (WifiPcieTables.h), the controller's PARF setup, PERST# release, LTSSM, and a configuration read of the endpoint's ID.
// The PHY supplies (L1E 0.88 V, L3E 1.2 V) are shared with the UFS PHY and the WCN7850 PMU inputs are RPMh rails, so the stage assumes
// they are on; the interconnect votes are not made either, which is enough for register access but not yet for the device's DMA.
// The devicetree facts: the WCN7850 sits on PCIe0 (pcie@1c00000, qcom,pcie-sm8550, two lanes, device 17cb:1107 below the root port),
// WLAN_EN is GPIO 80, BT_EN GPIO 81, SW_CTRL GPIO 82, PERST# GPIO 94 (active low), WAKE# GPIO 96.
// gcc-sm8550.c: PCIe0 GDSC at 0x6b004 (bit 31 PWR_ON); clock votes in 0x52008 (slv_axi 0, mstr_axi 1, cfg_ahb 2, aux 3, pipe 4,
// slv_q2a 5, cnoc_sf 6) and 0x52000 (aggre_noc 12, ddrss_sf 19, phy_rchng 22); each branch reports CLK_OFF in bit 31 of its halt
// register. The controller's registers are read only when the GDSC is on and the cfg_ahb, aux, master and slave branches run: a read
// of an unclocked block stalls the bus.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include "WifiPcieTables.h"

// [0] stage (1 mapped, 2 clocks read, 3 controller read; negative: the step that failed), [1] vote 0x52008, [2] vote 0x52000,
// [3] GDSC, [4] halt bits (bit i set: branch i of kBranches is off), [5] GPIO 80 | 82 << 8 | 94 << 16 (bit 0 input level, bit 1 output),
// [6] PARF LTSSM, [7] root port vendor/device, [8] link control/status, [9] PARF PHY control.
// Stage 2 steps in [0]: 10 WLAN_EN high, 11 GDSCs on, 12 clocks on, 13 PHY ready, 14 controller set up, 15 PERST# released, 16 link up,
// 17 endpoint ID read. [10] GPIO word after WLAN_EN, [11] GDSC bits (bit 0 PCIe0, bit 1 PHY) | halt bits << 8, [12] PCS_STATUS1 |
// PHY poll count << 16, [13] port debug 0 (LTSSM state in bits 0-5), [14] port debug 1, [15] endpoint vendor/device, [16] link ms
extern "C" {volatile uint32_t native_wifi_stats[17];}

namespace {
constexpr uint64_t kGcc = 0x100000, kTlmm = 0xf100000, kParf = 0x1c00000, kDbi = 0x60000000, kPhy = 0x1c06000, kTcsrClkref = 0x1fd5000;
constexpr uint64_t kAtu = 0x60001000, kConfig = 0x60100000;
constexpr uint32_t kVote8 = 0x52008, kVote0 = 0x52000, kGdsc = 0x6b004, kPhyGdsc = 0x6c000, kCollapse = 0x52020;
// slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a, cnoc_sf, aggre_noc, ddrss_sf, phy_rchng
constexpr uint32_t kBranches[] = {0x6b020, 0x6b02c, 0x6b038, 0x6b03c, 0x6b048, 0x6b01c, 0x10030, 0x1003c, 0x1004c, 0x6b054};
constexpr uint32_t kNeeded = 0xf;   // slv_axi, mstr_axi, cfg_ahb, aux
constexpr unsigned kGpios[] = {80, 82, 94};
constexpr uint32_t kParfLtssm = 0x1b0, kParfPhyCtrl = 0x40, kLinkControl = 0x80;
constexpr unsigned kWlanEn = 80, kPerst = 94, kClkReq = 95;
// gcc-sm8550.c: block resets, the pipe source mux (0 PHY, 2 TCXO) and the aux (TCXO) and rchng (GPLL0 even / 3) sources
constexpr uint32_t kPcieBcr = 0x6b000, kPhyBcr = 0x6c01c, kPipeMux = 0x6b070, kAuxRcg = 0x6b074, kRchngRcg = 0x6b058;
constexpr uint32_t kVote8Bits = 0x7f, kVote0Bits = 1u << 12 | 1u << 19 | 1u << 22;
// pcie-qcom.c
constexpr uint32_t kParfSysCtrl = 0x0, kParfPmCtrl = 0x20, kParfDbiBase = 0x350, kParfDbiBaseHi = 0x354, kParfMhiClockReset = 0x174;
constexpr uint32_t kParfAxiHalt = 0x1a8, kParfAtuBase = 0x634, kParfAtuBaseHi = 0x638, kParfDeviceType = 0x1000;
constexpr uint32_t kParfSlvSpace = 0x358, kParfSlvSpaceHi = 0x35c, kSlvSpaceBytes = 0x80000000;
// pcie-designware.h
constexpr uint32_t kPortDebug0 = 0x728, kPortDebug1 = 0x72c, kBusNumbers = 0x18, kCommand = 0x4;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void release(Window &w) {if (w.map) w.map->release(); w = {};}

// TLMM pin config: pull in bits 0-1 (1 down, 3 up), function in 2-5, drive (mA / 2 - 1) in 6-8, output enable bit 9; io bit 1 drives
void setPin(volatile uint32_t *tlmm, unsigned pin, uint32_t config, int level) {
    if (level >= 0) tlmm[(pin * 0x1000 + 4) / 4] = level ? 2u : 0u;
    tlmm[pin * 0x1000 / 4] = config;
    if (level >= 0) tlmm[(pin * 0x1000 + 4) / 4] = level ? 2u : 0u;
}

uint32_t pinWord(volatile uint32_t *tlmm) {
    uint32_t gpios = 0;
    for (unsigned i = 0; i < 3; ++i) gpios |= (tlmm[(kGpios[i] * 0x1000 + 4) / 4] & 3u) << (8 * i);
    return gpios;
}

bool waitClear(volatile uint32_t *reg, uint32_t mask, unsigned tries, unsigned *used = nullptr) {
    unsigned i = 0;
    for (; i < tries && (*reg & mask); ++i) IODelay(100);
    if (used) *used = i;
    return !(*reg & mask);
}

void pulseReset(volatile uint32_t *gcc, uint32_t bcr) {
    gcc[bcr / 4] |= 1u;
    IODelay(1000);
    gcc[bcr / 4] &= ~1u;
    IODelay(1000);
}

// Stage 2; the step reached stays in native_wifi_stats[0], a negative value names the step that failed.
void bringUp(Window &gcc, Window &tlmm) {
    volatile uint32_t *g = gcc.va, *t = tlmm.va;
    // PERST# held low, CLKREQ# to its PCIe function, then WLAN_EN high and the WCN7850 power-up delay (pwrseq-qcom-wcn.c: 50 ms)
    setPin(t, kPerst, 1u | 1u << 9, 0);
    setPin(t, kClkReq, 3u | 1u << 2, -1);
    setPin(t, kWlanEn, 1u | 3u << 6 | 1u << 9, 1);
    IOSleep(60);
    native_wifi_stats[10] = pinWord(t);
    native_wifi_stats[0] = 10;

    // both GDSCs are votable: a set bit in the collapse register (PCIe0 bit 0, PHY bit 3) keeps them collapsed
    g[kCollapse / 4] &= ~(1u << 0 | 1u << 3);
    unsigned tries = 0;
    while (tries++ < 100 && (!(g[kGdsc / 4] & (1u << 31)) || !(g[kPhyGdsc / 4] & (1u << 31)))) IODelay(10);
    uint32_t power = (g[kGdsc / 4] >> 31) | (g[kPhyGdsc / 4] >> 31) << 1;
    native_wifi_stats[11] = power;
    if (power != 3) {native_wifi_stats[0] = -11; return;}
    native_wifi_stats[0] = 11;

    // sources first (an RCG takes a new CFG on CMD bit 0), the pipe mux parked on TCXO until the PHY runs, then the branch votes
    g[(kAuxRcg + 4) / 4] = 0;
    g[kAuxRcg / 4] |= 1u;
    g[(kRchngRcg + 4) / 4] = 6u << 8 | 5u;
    g[kRchngRcg / 4] |= 1u;
    waitClear(&g[kAuxRcg / 4], 1u, 100);
    waitClear(&g[kRchngRcg / 4], 1u, 100);
    g[kPipeMux / 4] = (g[kPipeMux / 4] & ~3u) | 2u;
    g[kVote8 / 4] |= kVote8Bits;
    g[kVote0 / 4] |= kVote0Bits;
    Window clkref = {};
    if (!mapWindow(clkref, kTcsrClkref, 0x1000)) {native_wifi_stats[0] = -12; return;}
    clkref.va[0x100 / 4] |= 1u;
    release(clkref);
    IODelay(200);
    uint32_t off = 0;
    for (unsigned i = 0; i < sizeof kBranches / sizeof kBranches[0]; ++i)
        if (g[kBranches[i] / 4] & (1u << 31)) off |= 1u << i;
    native_wifi_stats[11] = power | off << 8;
    if (off & ~(1u << 4)) {native_wifi_stats[0] = -12; return;}   // the pipe branch runs only once the PHY drives it
    pulseReset(g, kPcieBcr);
    pulseReset(g, kPhyBcr);
    native_wifi_stats[0] = 12;

    // qmp_pcie_power_on: power-down control, the tables (both lanes), pipe clock from the PHY, out of reset, start, PHYSTATUS low
    Window phy = {};
    if (!mapWindow(phy, kPhy, 0x2000)) {native_wifi_stats[0] = -13; return;}
    volatile uint32_t *p = phy.va;
    using namespace wifi_phy;
    p[kPowerDownControl / 4] |= 3u;
    for (const Init &e : kSerdes) p[e.offset / 4] = e.value;
    for (const Init &e : kTx) {p[e.offset / 4] = e.value; p[(e.offset + kTx2) / 4] = e.value;}
    for (const Init &e : kRx) {p[e.offset / 4] = e.value; p[(e.offset + kRx2) / 4] = e.value;}
    for (const Init &e : kPcs) p[e.offset / 4] = e.value;
    for (const Init &e : kPcsMisc) p[e.offset / 4] = e.value;
    g[kPipeMux / 4] &= ~3u;
    p[kSwReset / 4] &= ~1u;
    p[kStartControl / 4] |= 3u;
    IODelay(1000);
    unsigned polls = 0;
    const bool ready = waitClear(&p[kPcsStatus1 / 4], 1u << 6, 100, &polls);
    native_wifi_stats[12] = (p[kPcsStatus1 / 4] & 0xffffu) | polls << 16;
    release(phy);
    if (!ready) {native_wifi_stats[0] = -13; return;}
    native_wifi_stats[0] = 13;

    // qcom_pcie_init_2_7_0 after its resets
    Window parf = {}, dbi = {};
    if (!mapWindow(parf, kParf, 0x2000) || !mapWindow(dbi, kDbi, 0x1000)) {release(parf); native_wifi_stats[0] = -14; return;}
    volatile uint32_t *f = parf.va, *d = dbi.va;
    f[kParfDeviceType / 4] = 4u;
    f[kParfPhyCtrl / 4] &= ~1u;
    f[kParfDbiBase / 4] = uint32_t(kDbi);
    f[kParfDbiBaseHi / 4] = uint32_t(kDbi >> 32);
    f[kParfAtuBase / 4] = uint32_t(kAtu);
    f[kParfAtuBaseHi / 4] = uint32_t(kAtu >> 32);
    f[kParfSlvSpace / 4] = 0;                 // qcom_pcie_configure_dbi_atu_base: the slave window spans 2 GiB << 32
    f[kParfSlvSpaceHi / 4] = kSlvSpaceBytes;
    f[kParfSysCtrl / 4] &= ~(1u << 29);
    f[kParfMhiClockReset / 4] |= 1u << 4;
    f[kParfPmCtrl / 4] &= ~(1u << 5);
    f[kParfAxiHalt / 4] |= 1u << 31;
    d[kBusNumbers / 4] = 0x00010100;   // primary 0, secondary 1, subordinate 1
    d[kCommand / 4] |= 0x6u;           // memory space and bus master
    native_wifi_stats[7] = d[0];
    native_wifi_stats[0] = 14;

    // qcom_ep_reset_deassert: PERST# low for at least 100 ms, then high, then the LTSSM (qcom_pcie_2_3_2_ltssm_enable)
    IOSleep(100);
    setPin(t, kPerst, 1u | 1u << 9, 1);
    IODelay(1000);
    native_wifi_stats[0] = 15;
    f[kParfLtssm / 4] |= 1u << 8;
    const uint64_t start = mach_absolute_time();
    unsigned ms = 0;
    for (; ms < 1000; ++ms) {
        if ((d[kPortDebug1 / 4] & (1u << 4)) && (d[kPortDebug0 / 4] & 0x3f) == 0x11) break;
        IOSleep(1);
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    native_wifi_stats[16] = uint32_t(ns / 1000000);
    native_wifi_stats[13] = d[kPortDebug0 / 4];
    native_wifi_stats[14] = d[kPortDebug1 / 4];
    native_wifi_stats[6] = f[kParfLtssm / 4];
    native_wifi_stats[8] = d[kLinkControl / 4];
    native_wifi_stats[9] = f[kParfPhyCtrl / 4];
    if (ms == 1000) {release(dbi); release(parf); native_wifi_stats[0] = -16; return;}
    native_wifi_stats[0] = 16;

    // outbound iATU region 0 (unrolled at the ATU base) turns the 1 MiB config window into type 0 accesses to bus 1 device 0
    Window atu = {}, config = {};
    if (mapWindow(atu, kAtu, 0x1000) && mapWindow(config, kConfig, 0x1000)) {
        volatile uint32_t *a = atu.va;
        a[0x08 / 4] = uint32_t(kConfig);
        a[0x0c / 4] = uint32_t(kConfig >> 32);
        a[0x10 / 4] = uint32_t(kConfig + 0xfffff);
        a[0x20 / 4] = uint32_t((kConfig + 0xfffff) >> 32);
        a[0x14 / 4] = 1u << 24;
        a[0x18 / 4] = 0;
        a[0x00 / 4] = 0x4;
        a[0x04 / 4] = 1u << 31;
        for (unsigned i = 0; i < 5 && !(a[0x04 / 4] & (1u << 31)); ++i) IODelay(10);
        native_wifi_stats[15] = config.va[0];
        native_wifi_stats[0] = 17;
    } else native_wifi_stats[0] = -17;
    release(config); release(atu); release(dbi); release(parf);
}

void wifiThread(void *arg, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng-wifi");
    Window gcc = {}, tlmm = {}, parf = {}, dbi = {};
    if (!mapWindow(gcc, kGcc, 0x80000) || !mapWindow(tlmm, kTlmm, 0x60000)) {native_wifi_stats[0] = -1; release(gcc); release(tlmm); return;}
    native_wifi_stats[0] = 1;
    native_wifi_stats[1] = gcc.va[kVote8 / 4];
    native_wifi_stats[2] = gcc.va[kVote0 / 4];
    const uint32_t gdsc = gcc.va[kGdsc / 4];
    native_wifi_stats[3] = gdsc;
    uint32_t off = 0;
    for (unsigned i = 0; i < sizeof kBranches / sizeof kBranches[0]; ++i)
        if (gcc.va[kBranches[i] / 4] & (1u << 31)) off |= 1u << i;
    native_wifi_stats[4] = off;
    native_wifi_stats[5] = pinWord(tlmm.va);
    native_wifi_stats[0] = 2;
    if (reinterpret_cast<uintptr_t>(arg) >= 2) {bringUp(gcc, tlmm); release(tlmm); release(gcc); return;}
    if ((gdsc & (1u << 31)) && !(off & kNeeded) && mapWindow(parf, kParf, 0x3000) && mapWindow(dbi, kDbi, 0x1000)) {
        native_wifi_stats[6] = parf.va[kParfLtssm / 4];
        native_wifi_stats[9] = parf.va[kParfPhyCtrl / 4];
        native_wifi_stats[7] = dbi.va[0];
        native_wifi_stats[8] = dbi.va[kLinkControl / 4];
        native_wifi_stats[0] = 3;
    }
    release(dbi); release(parf); release(tlmm); release(gcc);
}
}  // namespace

extern "C" void sheng_wifi_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("sheng-wifi", &stage, sizeof stage) || stage < 1 || native_wifi_stats[0]) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiThread, reinterpret_cast<void *>(uintptr_t(stage)), &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_wifi_stats[0] = -7;
}
