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
#include <string.h>
#include "QcomRpmh.h"
#include "BootResetCore.h"

// [0] stage (1 mapped, 2 clocks read, 3 controller read; negative: the step that failed), [1] vote 0x52008, [2] vote 0x52000,
// [3] GDSC, [4] halt bits (bit i set: branch i of kBranches is off), [5] GPIO 80 | 82 << 8 | 94 << 16 (bit 0 input level, bit 1 output),
// [6] PARF LTSSM, [7] root port vendor/device, [8] link control/status, [9] PARF PHY control.
// Stage 2 steps in [0]: 10 WLAN_EN high, 11 GDSCs on, 12 clocks on, 13 PARF set up, 14 PHY ready and root port set up, 15 PERST# released, 16 link up,
// 17 endpoint ID read. [10] GPIO word after WLAN_EN, [11] GDSC bits (bit 0 PCIe0, bit 1 PHY) | halt bits << 8, [12] PCS_STATUS1 |
// PHY poll count << 16, [13] port debug 0 (LTSSM state in bits 0-5), [14] port debug 1, [15] endpoint vendor/device, [16] link ms
extern "C" {volatile uint32_t native_wifi_stats[29]; volatile uint32_t native_wifi_dump[6 * 18];
// PMIC state read through the SPMI arbiter, which does not depend on RPMh answering. [0] arbiter version, [1] APIDs, [2] regulators found before | after << 16,
// [3] PMK8550 gpio3 (the sleep clock pin) APID | owner << 16 (0xffff: none), [4..6] its registers 04 05 40 41 | 42 43 44 45 | 46 4a before, [7..9] after,
// [10] result of the sleep clock write (0 written, 0xff not needed, other: the step that failed), [11..16] command database addresses of the six rails
// (0 when not found), [17] pipe clock branch halt register after the PHY is ready, [18] PERST# pin config | io << 16 after its release, [19] port debug 0 20 ms after
// the LTSSM enable, [20] pipe source mux register at the end, [21] port debug 1 at 20 ms. Regulator tables follow: 112 entries of two words, first word PPID << 16 | type << 8 | subtype, second
// 0x08 << 24 | 0x46 << 16 | 0x40 << 8 | 0x41; before at [24], after at [248].
volatile uint32_t native_wifi_pmic[24 + 2 * 2 * 112];}

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

// ---- PMIC state through the SPMI arbiter (spmi@c400000, EE 0; the same layout the button driver uses) ----
// Regulator peripherals: type 0x04 LDO, 0x03 BUCK, 0x1c FTS, 0x21/0x22 ULT; status 0x08 (bit 7 in regulation), enable 0x46 (bit 7), the buck
// voltage in mV at 0x40/0x41. The sleep clock: mainline's sheng description has the WCN7850 PMU pinctrl select PMK8550 gpio3, function func1,
// output enabled, no bias, power source 0; the gpio block of PMK8550 starts at 0xb800 with 0x100 per pin, so gpio3 is peripheral 0xba of SID 0.
// A low voltage/medium voltage pin takes mode 1 (output) at 0x40, the function index (func1 = 2) at 0x44 and the master enable at 0x46.
constexpr uint64_t kSpmiCore = 0xc400000, kSpmiObs = 0xc440000, kSpmiChn = 0xc500000, kSpmiCnfg = 0xc42d000;
constexpr uint32_t kSleepClkPpid = 0x0ba, kMaxRegs = 112;
// The PMIC regulator peripherals the six rails live in, by the low byte of the PPID: SMPS n at 0x9b00 + 0x300 * (n - 1) pages (smps1 0x9b ... smps6 0xaa, the
// pattern seen in the first scan: smps4 0xa4, smps6 0xaa), LDO15 at 0x4e. Every SID 0 to 15 is looked at for these.
constexpr uint8_t kRegPages[] = {0x9b, 0x9e, 0xa1, 0xa4, 0xa7, 0xaa, 0x4e};

struct Pmic {Window core, cnfg, obs, chn; br_arbiter arb; uint32_t apids;};
Pmic gPmic;

bool pmicOpen(Pmic &p) {
    p.arb.ee = 0;
    if (!mapWindow(p.core, kSpmiCore, 0x3000) || !mapWindow(p.cnfg, kSpmiCnfg, 0x4000)) return false;
    p.arb.core = p.core.va; p.arb.cnfg = p.cnfg.va;
    native_wifi_pmic[0] = p.core.va[0];
    p.apids = p.core.va[BR_FEATURES / 4] & BR_FEATURES_APIDS_MASK;
    native_wifi_pmic[1] = p.apids;
    if (p.core.va[0] < BR_VERSION_V7_MIN || p.core.va[0] >= BR_VERSION_V8_MIN || !p.apids || p.apids > 1024) return false;
    return mapWindow(p.obs, kSpmiObs, ((uint64_t)BR_OBS_APID_STRIDE * p.apids + 0xfff) & ~0xfffull) &&
           mapWindow(p.chn, kSpmiChn, (uint64_t)BR_CHNL_APID_STRIDE * p.apids);
}

int pmicApid(Pmic &p, uint32_t ppid, int *owner) {
    int found = -1;
    for (uint32_t i = 0; i < p.apids; ++i) {
        const uint32_t map = p.core.va[(BR_APID_MAP + 4 * i) / 4];
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        const int o = (int)(p.cnfg.va[(BR_CNFG_OWNER + 4 * i) / 4] & 7u);
        if (found < 0 || o == p.arb.ee) {found = (int)i; *owner = o;}
    }
    return found;
}

// The candidate regulator peripherals (kRegPages in every SID) that exist; entries go to out (two words each). Returns how many.
unsigned pmicScan(Pmic &p, volatile uint32_t *out) {
    unsigned n = 0;
    for (uint32_t sid = 0; sid < 16 && n < kMaxRegs; ++sid)
        for (unsigned k = 0; k < sizeof kRegPages && n < kMaxRegs; ++k) {
            const uint32_t ppid = sid << 8 | kRegPages[k];
            int owner = -1;
            const int apid = pmicApid(p, ppid, &owner);
            if (apid < 0) continue;
            uint8_t type = 0, sub = 0, st = 0, en = 0, lo = 0, hi = 0;
            if (br_read8(p.obs.va, &p.arb, apid, 0x04, &type) || br_read8(p.obs.va, &p.arb, apid, 0x05, &sub)) continue;
            if (type != 0x03 && type != 0x04 && type != 0x1c && type != 0x21 && type != 0x22) continue;
            if (br_read8(p.obs.va, &p.arb, apid, 0x08, &st) || br_read8(p.obs.va, &p.arb, apid, 0x46, &en) ||
                br_read8(p.obs.va, &p.arb, apid, 0x40, &lo) || br_read8(p.obs.va, &p.arb, apid, 0x41, &hi)) continue;
            out[2 * n] = ppid << 16 | (uint32_t)type << 8 | sub;
            out[2 * n + 1] = (uint32_t)st << 24 | (uint32_t)en << 16 | (uint32_t)lo << 8 | hi;
            ++n;
        }
    return n;
}

// The sleep clock pin's registers 04 05 40 41 | 42 43 44 45 | 46 4a, packed into three words at out.
void pmicSleepClk(Pmic &p, int apid, volatile uint32_t *out) {
    static const uint8_t regs[] = {0x04, 0x05, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x4a};
    uint32_t w[3] = {0, 0, 0};
    for (unsigned i = 0; i < sizeof regs; ++i) {
        uint8_t v = 0xee;
        br_read8(p.obs.va, &p.arb, apid, regs[i], &v);
        w[i / 4] |= (uint32_t)v << (8 * (i % 4));
    }
    out[0] = w[0]; out[1] = w[1]; out[2] = w[2];
}

void pmicSurvey(bool after) {
    Pmic &p = gPmic;
    const unsigned n = pmicScan(p, native_wifi_pmic + (after ? 24 + 2 * kMaxRegs : 24));
    native_wifi_pmic[2] = after ? (native_wifi_pmic[2] & 0xffff) | n << 16 : n;
}

// Before the PMU is powered: look at the sleep clock pin and switch it on when it is not already an enabled output of function 2.
void pmicPrepare() {
    Pmic &p = gPmic;
    if (!pmicOpen(p)) {native_wifi_pmic[10] = 1; return;}
    pmicSurvey(false);
    int owner = -1;
    const int apid = pmicApid(p, kSleepClkPpid, &owner);
    native_wifi_pmic[3] = apid < 0 ? 0xffff : (uint32_t)apid | (uint32_t)owner << 16;
    if (apid < 0) {native_wifi_pmic[10] = 2; return;}
    pmicSleepClk(p, apid, native_wifi_pmic + 4);
    const uint32_t mode = native_wifi_pmic[4] >> 16 & 0xff, source = native_wifi_pmic[5] >> 16 & 0xff, enable = native_wifi_pmic[6] & 0xff;
    if ((enable & 0x80) && (mode & 3) == 1 && (source & 0xf) == 2) {native_wifi_pmic[10] = 0xff;}
    else if (owner != 0) native_wifi_pmic[10] = 3;
    else if ((native_wifi_pmic[4] >> 8 & 0xff) < 0x10 || (native_wifi_pmic[4] >> 8 & 0xff) > 0x13) native_wifi_pmic[10] = 4;   // not a LV/MV pin: other register layout
    else {
        // pull disabled, power source 0, source function, output mode, master enable (the order the pinctrl driver applies them)
        int rc = br_write8(p.chn.va, apid, 0x42, 5);
        if (!rc) rc = br_write8(p.chn.va, apid, 0x41, 0);
        if (!rc) rc = br_write8(p.chn.va, apid, 0x44, 2);
        if (!rc) rc = br_write8(p.chn.va, apid, 0x40, 1);
        if (!rc) rc = br_write8(p.chn.va, apid, 0x46, 0x80);
        native_wifi_pmic[10] = rc ? 0x10u + (uint32_t)rc : 0;
    }
    pmicSleepClk(p, apid, native_wifi_pmic + 7);
}

// The WCN7850 PMU's six supplies are RPMh rails nothing else needs at boot, so nothing has turned them on. Each is a VRM resource of the
// command database (smp<pmic id><n>, ldo<pmic id><n>) with its voltage in millivolts at +0 and its enable at +4. Two rails go in one active
// request (four commands); the request's done flag is polled in the RSC's status register.
// The PMU's supplies in the order the power sequencer takes them (vdd, vddio, vddaon, vdddig, vddrfa1p2, vddrfa1p8). ppid is the PMIC peripheral
// that makes the rail (0: not known), used to see whether the request had an effect: SMPS n of a PM8550VS sits at page 0x9b + 3 * (n - 1) of its SID.
// The pairing of SID 6 with the "g" PMIC and SID 4 with "e" rests on the setpoint registers there reading 852 mV (smps2, smps5 of SID 6), 1304 mV
// (smps4) and 1856 mV (smps6), and 964 mV for smps4 of SID 4.
struct Rail {const char *name; uint32_t millivolts; uint32_t ppid;};
constexpr Rail kRails[] = {{"smpg5", 850, 0x6a7}, {"ldob15", 1800, 0}, {"smpg2", 850, 0x69e}, {"smpe4", 950, 0x4a4}, {"smpg4", 1300, 0x6a4}, {"smpg6", 1860, 0x6aa}};
constexpr uint64_t kRscDrv = 0x17a20000, kRscTcs = 0xd00, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;

// [20] bit i: rail i found in the command database and voted | 1 << (8 + i) request done; [21] RSC id | TCS << 24 (negative stage on failure)
int powerRails() {
    static uint8_t db[kCmdDbSize];
    Window dbw = {}, rsc = {};
    if (!mapWindow(dbw, kCmdDb, kCmdDbSize)) return -1;
    for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = dbw.va[i / 4]; memcpy(db + i, &w, 4);}
    release(dbw);
    if (!mapWindow(rsc, kRscDrv, 0x10000)) return -2;
    volatile uint32_t *r = rsc.va;
    const QcomRscLayout layout = qcom_rsc_layout(r[0]);
    // apps_rsc qcom,tcs-config is <2 3>,<0 2>,<1 2>,<3 0>: type 2 (active) x3, type 0 (sleep) x2, type 1 (wake) x2, TCSs numbered in that order,
    // so the active ones are 0 to 2; sleep and wake TCSs are only run by the solver on a low power entry or exit
    const unsigned tcs = 0;
    native_wifi_stats[21] = r[0];
    // the done interrupt of the active TCSs stays enabled, as the RSC driver leaves it: the latch in the status register needs it.
    // [26] the DRV's TCS configuration register (TCS count per DRV, commands per TCS), [27] the interrupt enable register afterwards, [28] the DRV id
    r[kRscTcs / 4] = 7u;
    __asm__ volatile("dsb sy" ::: "memory");
    native_wifi_stats[26] = r[0x0c / 4];
    native_wifi_stats[27] = r[kRscTcs / 4];
    native_wifi_stats[28] = r[0];
    uint32_t result = 0, enabledMask = 0;
    const uint64_t t0 = mach_absolute_time();
    // One request per register of a rail, as the RPMh regulator driver sends them: the mode (7: HPM for the SMPSs and LDOs of these PMICs), the voltage
    // in millivolts, then the enable. The done interrupt cannot be seen here (the TCS reads zero), so each request is followed by a fixed wait and the
    // enable request by a look at the PMIC's own enable register.
    auto send = [&](uint32_t address, uint32_t value) {
        QcomRpmhWrite writes[16];
        const unsigned n = qcom_rpmh_active_request(&layout, tcs, &address, &value, 1, writes, 16);
        for (unsigned i = 0; i < n; ++i) r[(kRscTcs + writes[i].offset) / 4] = writes[i].value;
        __asm__ volatile("dsb sy" ::: "memory");
        IOSleep(2);
        const unsigned m = qcom_rpmh_finish(&layout, tcs, writes, 16);
        for (unsigned i = 0; i < m; ++i) r[(kRscTcs + writes[i].offset) / 4] = writes[i].value;
    };
    for (unsigned i = 0; i < sizeof kRails / sizeof kRails[0]; ++i) {
        QcomRpmhResource res;
        if (qcom_cmd_db_find(db, sizeof db, kRails[i].name, &res)) continue;
        native_wifi_pmic[11 + i] = res.address;
        result |= 1u << i;
        send(res.address + 8, 7);
        send(res.address, kRails[i].millivolts);
        send(res.address + 4, 1);
        int owner = -1, apid = kRails[i].ppid ? pmicApid(gPmic, kRails[i].ppid, &owner) : -1;
        unsigned waited = 0;
        if (apid < 0) IOSleep(20);
        else for (; waited < 50; ++waited) {
            uint8_t en = 0;
            if (!br_read8(gPmic.obs.va, &gPmic.arb, apid, 0x46, &en) && (en & 0x80)) {enabledMask |= 1u << i; break;}
            IOSleep(1);
        }
        native_wifi_pmic[22] = enabledMask | (apid < 0 ? 0 : 1u << (8 + i)) | waited << 16;
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    native_wifi_pmic[23] = uint32_t(ns / 1000000);
    native_wifi_stats[20] = result;
    release(rsc);
    return 0;
}

// Stage 2; the step reached stays in native_wifi_stats[0], a negative value names the step that failed.
void bringUp(Window &gcc, Window &tlmm) {
    volatile uint32_t *g = gcc.va, *t = tlmm.va;
    pmicPrepare();
    const int rails = powerRails();
    IOSleep(20);
    if (gPmic.obs.va) pmicSurvey(true);
    if (rails) {native_wifi_stats[0] = -9; return;}
    IOSleep(5);
    // PERST# held low, CLKREQ# to its PCIe function, then WLAN_EN high and the WCN7850 power-up delay (pwrseq-qcom-wcn.c: 50 ms)
    setPin(t, kPerst, 1u | 1u << 9, 0);
    setPin(t, kClkReq, 3u | 1u << 2, -1);
    setPin(t, kWlanEn, 1u | 3u << 6 | 1u << 9, 1);
    IOSleep(60);
    native_wifi_stats[10] = pinWord(t);
    // what the pin registers hold now (a write TZ or the hypervisor drops reads back as written-nothing)
    native_wifi_stats[17] = (t[kWlanEn * 0x1000 / 4] & 0xffff) | t[(kWlanEn * 0x1000 + 4) / 4] << 16;
    native_wifi_stats[18] = (t[kPerst * 0x1000 / 4] & 0xffff) | t[(kPerst * 0x1000 + 4) / 4] << 16;
    native_wifi_stats[19] = (t[kClkReq * 0x1000 / 4] & 0xffff) | t[(kClkReq * 0x1000 + 4) / 4] << 16;
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

    // qcom_pcie_init_2_7_0 after its resets, before the PHY powers on (qcom_pcie_host_init): PHY_CTRL's test power-down bit holds the PHY
    Window parf = {}, dbi = {};
    if (!mapWindow(parf, kParf, 0x2000) || !mapWindow(dbi, kDbi, 0x1000)) {release(parf); native_wifi_stats[0] = -13; return;}
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
    native_wifi_stats[0] = 13;

    // qmp_pcie_power_on: power-down control, the tables (both lanes), pipe clock from the PHY, out of reset, start, PHYSTATUS low
    Window phy = {};
    if (!mapWindow(phy, kPhy, 0x2000)) {release(dbi); release(parf); native_wifi_stats[0] = -14; return;}
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
    native_wifi_pmic[17] = g[kBranches[4] / 4];
    release(phy);
    if (!ready) {release(dbi); release(parf); native_wifi_stats[0] = -14; return;}

    // qcom_pcie_post_init_2_7_0 and the root port's bus numbers and command (dw_pcie_setup_rc)
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
        if (ms == 20) {native_wifi_pmic[19] = d[kPortDebug0 / 4]; native_wifi_pmic[21] = d[kPortDebug1 / 4];}
        IOSleep(1);
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    native_wifi_stats[16] = uint32_t(ns / 1000000);
    native_wifi_pmic[18] = (t[kPerst * 0x1000 / 4] & 0xffff) | t[(kPerst * 0x1000 + 4) / 4] << 16;
    native_wifi_pmic[20] = g[kPipeMux / 4];
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
