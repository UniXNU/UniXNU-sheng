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
#include "MhiBoot.h"
#include "nw_abi.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <stdarg.h>
#include <kern/task.h>
#include <stdint.h>

// [0] stage (1 mapped, 2 clocks read, 3 controller read; negative: the step that failed), [1] vote 0x52008, [2] vote 0x52000,
// [3] GDSC, [4] halt bits (bit i set: branch i of kBranches is off), [5] GPIO 80 | 82 << 8 | 94 << 16 (bit 0 input level, bit 1 output),
// [6] PARF LTSSM, [7] root port vendor/device, [8] link control/status, [9] PARF PHY control.
// Stage 2 steps in [0]: 10 WLAN_EN high, 11 GDSCs on, 12 clocks on, 13 PARF set up, 14 PHY ready and root port set up, 15 PERST# released, 16 link up,
// 17 endpoint ID read. [10] GPIO word after WLAN_EN, [11] GDSC bits (bit 0 PCIe0, bit 1 PHY) | halt bits << 8, [12] PCS_STATUS1 |
// PHY poll count << 16, [13] port debug 0 (LTSSM state in bits 0-5), [14] port debug 1, [15] endpoint vendor/device, [16] link ms
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_extra_stream __attribute__((weak_import));
extern "C" uint8_t *native_ufs_extra_va __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_extra_uncached __attribute__((weak_import));
extern "C" int native_ufs_extra_fault(uint32_t *) __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_extra_atos[8] __attribute__((weak_import));
extern "C" volatile uint32_t native_ufs_extra_iova __attribute__((weak_import)), native_ufs_extra_bytes __attribute__((weak_import));
extern "C" int native_ufs_part_find(const char *, uint32_t *, uint64_t *, uint64_t *) __attribute__((weak_import));
extern "C" int native_ufs_part_read(uint32_t, uint64_t, uint32_t, void *) __attribute__((weak_import));
extern "C" {volatile uint32_t native_wifi_stats[29]; volatile uint32_t native_wifi_dump[6 * 18];
// PMIC state read through the SPMI arbiter, which does not depend on RPMh answering. [0] arbiter version, [1] APIDs, [2] regulators found before | after << 16,
// [3] PMK8550 gpio3 (the sleep clock pin) APID | owner << 16 (0xffff: none), [4..6] its registers 04 05 40 41 | 42 43 44 45 | 46 4a before, [7..9] after,
// [10] result of the sleep clock write (0 written, 0xff not needed, other: the step that failed), [11..16] command database addresses of the six rails
// (0 when not found), [17] pipe clock branch halt register after the PHY is ready, [18] PERST# pin config | io << 16 after its release, [19] port debug 0 20 ms after
// the LTSSM enable, [20] pipe source mux register at the end, [21] port debug 1 at 20 ms. Regulator tables follow: 112 entries of two words, first word PPID << 16 | type << 8 | subtype, second
// 0x08 << 24 | 0x46 << 16 | 0x40 << 8 | 0x41; before at [24], after at [248].
volatile uint32_t native_wifi_pmic[24 + 2 * 2 * 112];
// The endpoint after the link is up (stage 4): [0] stage reached, [1] BAR0 sizing mask, [2] BAR0 as assigned, [3] command register read back, [4] root port memory
// window register, [5..11] MHI MHIREGLEN MHIVER MHICFG BHIOFF BHIEOFF MHICTRL MHISTATUS, [12] BHI version major | minor << 16, [13] BHI execution environment,
// [14] BHI status, [15] BHI error code.
volatile uint32_t native_wifi_ep[16];
volatile uint32_t native_wifi_pon[12];
volatile uint32_t native_wifi_cfg[32];
volatile uint32_t native_wifi_chip[16];
volatile uint32_t native_wifi_ev[48];
volatile uint32_t native_wifi_ldo[24];
// The firmware download (stage 5): [0] step (1 firmware container found, 2 amss.bin read, 3 DMA buffers, 4 SMMU stream mapped, 5 MHI opened and SBL started,
// 6 contexts programmed, 7 mission mode), negative: the step that failed; [1] SMMU slot | bank << 16 (or the planner's error index); [2] partition LUN | first block << 8;
// [3] amss.bin size; [4] segments; [5] MhiBoot phase | error << 8 | failed phase << 16; [6] execution environment; [7] MHI state; [8] BHI status; [9] BHI error code;
// [10..12] BHI debug words; [13] MHI status register; [14] ms spent; [15] BHIe status.
volatile uint32_t native_wifi_mhi[16];
// Milliseconds since the download started when: [0] the buffers were ready, [1] the container was found, [2] the image was read, [3] the SMMU stream was mapped,
// [4] the boot loader was started, [5] the contexts were programmed, [6] the run ended.
volatile uint32_t native_wifi_mhi_ms[8];
// The WiFi module's messages: a ring of 512 lines, newest at native_wifi_text_n - 1.
char native_wifi_text[512][112]; volatile uint32_t native_wifi_text_n;
// The same messages of level 6 and below (notices, warnings, errors) in a ring of their own, which the driver's debug lines do not push out.
char native_wifi_note[128][112]; volatile uint32_t native_wifi_note_n;}

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_wifi_net_static_init(void) __asm__("__GLOBAL__sub_I_WifiNet.cpp");
extern "C" void sheng_wifi_net_start(const uint8_t mac[6]);
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
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
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

// The boot's power-on peripheral (type 0x09 in SID 0 or 1): ppid, then bytes 0x08 to 0x27 (the reason registers of the previous power-off and warm reset).
void pmicPon(Pmic &p) {
    for (uint32_t sid = 0; sid < 2; ++sid)
        for (uint32_t page = 0; page < 0x100; ++page) {
            int owner = -1;
            const int apid = pmicApid(p, sid << 8 | page, &owner);
            uint8_t type = 0;
            if (apid < 0 || br_read8(p.obs.va, &p.arb, apid, 0x04, &type) || type != 0x09) continue;
            native_wifi_pon[0] = (sid << 8 | page) << 8 | (uint32_t)owner;
            for (unsigned i = 0; i < 32; ++i) {
                uint8_t v = 0xee;
                br_read8(p.obs.va, &p.arb, apid, 0x08 + i, &v);
                native_wifi_pon[1 + i / 4] |= (uint32_t)v << (8 * (i % 4));
            }
            return;
        }
}

// Every peripheral (SIDs 0 to 7) whose voltage setpoint reads 1.8 V (1792 to 1808 mV): the VDDIO rail's peripheral address is not known. Up to six per call,
// before (out[0..11]) and after (out[12..23]) the rail votes; two words each as in pmicScan.
void pmicFind1800(Pmic &p, volatile uint32_t *out) {
    unsigned n = 0;
    for (uint32_t sid = 0; sid < 8 && n < 6; ++sid)
        for (uint32_t page = 0x40; page < 0x100 && n < 6; ++page) {
            int owner = -1;
            const int apid = pmicApid(p, sid << 8 | page, &owner);
            uint8_t type = 0, sub = 0, st = 0, en = 0, lo = 0, hi = 0;
            if (apid < 0 || br_read8(p.obs.va, &p.arb, apid, 0x04, &type) || type == 0x09 || type == 0x2c) continue;
            if (br_read8(p.obs.va, &p.arb, apid, 0x40, &lo) || br_read8(p.obs.va, &p.arb, apid, 0x41, &hi)) continue;
            const uint32_t v = (uint32_t)hi << 8 | lo;
            if (v < 1792 || v > 1808) continue;
            br_read8(p.obs.va, &p.arb, apid, 0x05, &sub);
            br_read8(p.obs.va, &p.arb, apid, 0x08, &st);
            br_read8(p.obs.va, &p.arb, apid, 0x46, &en);
            out[2 * n] = (sid << 8 | page) << 16 | (uint32_t)type << 8 | sub;
            out[2 * n + 1] = (uint32_t)st << 24 | (uint32_t)en << 16 | v;
            ++n;
        }
}

// Before the PMU is powered: look at the sleep clock pin and switch it on when it is not already an enabled output of function 2.
void pmicPrepare() {
    Pmic &p = gPmic;
    if (!pmicOpen(p)) {native_wifi_pmic[10] = 1; return;}
    pmicSurvey(false);
    pmicPon(p);
    pmicFind1800(p, native_wifi_ldo);
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
    // The endpoint's DMA runs over the PCIe NoC, whose bus clock manager (SN7) the interconnect driver votes for when the link is brought up (pcie-mem path,
    // always tag); nothing has done that here. Bandwidth in the units of the manager's aux data: vote = ceil(ceil(kBps * width / buswidth) * 1000 / unit), buswidth 16.
    {
        QcomRpmhResource bcm;
        if (!qcom_cmd_db_find(db, sizeof db, "SN7", &bcm)) {
            uint32_t vote = 0x400;
            if (bcm.level_count >= 3 && (bcm.levels[0] | (uint32_t)bcm.levels[1] << 16) && bcm.levels[2]) {
                const uint64_t unit = bcm.levels[0] | (uint32_t)bcm.levels[1] << 16;
                const uint64_t kBps = 2000000;
                const uint64_t scaled = (kBps * bcm.levels[2] + 15) / 16;
                const uint64_t x = (scaled * 1000 + unit - 1) / unit;
                vote = x < 1 ? 1 : x > 0x3fff ? 0x3fff : (uint32_t)x;
            }
            send(bcm.address, 1u << 30 | 1u << 29 | vote << 14 | vote);
            native_wifi_pon[9] = vote | (bcm.level_count << 16) | 0x80000000u;
        } else native_wifi_pon[9] = 0x40000000u;
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    native_wifi_pmic[23] = uint32_t(ns / 1000000);
    native_wifi_stats[20] = result;
    release(rsc);
    return 0;
}

// The MEM window of pcie@1c00000: CPU and PCI addresses are the same, 0x60300000 for 0x3d00000 bytes (sm8550.dtsi ranges).
constexpr uint64_t kMemBase = 0x60300000, kMemSize = 0x3d00000;

// ---- stage 5: the firmware into the endpoint (SMMU stream, DMA buffers, BHI secondary boot loader, MHI contexts, BHIe full image) ----
constexpr uint32_t kSegment = 128 * 1024, kSbl = 512 * 1024, kMaxSegments = 48;

// DMA memory: carved in order out of the buffer the storage driver mapped for the endpoint's stream (see NativeUfs); pa is the address the endpoint uses.
struct Dma {uint8_t *va; uint64_t pa; size_t bytes;};
uint32_t gDmaUsed;
bool dmaAlloc(Dma &b, size_t bytes) {
    if (&native_ufs_extra_va == nullptr || !native_ufs_extra_va || gDmaUsed + bytes > native_ufs_extra_bytes) {native_wifi_mhi[11] = 1; return false;}
    b.va = native_ufs_extra_va + gDmaUsed;
    b.pa = (uint64_t)native_ufs_extra_iova + gDmaUsed;
    b.bytes = bytes;
    gDmaUsed += (uint32_t)((bytes + 4095) & ~4095ull);
    bzero(b.va, bytes);
    return true;
}

struct Fw {uint32_t lun; uint64_t first; uint32_t amssOffset, amssSize;};
// The container (local-tools/make-wifi-fw.py) in the partition named oops: 'WFW1', count, then name[24] offset size.
bool findFirmware(Fw &f) {
    if (&native_ufs_part_find == nullptr || &native_ufs_part_read == nullptr || &native_ufs_extra_stream == nullptr) return false;
    uint64_t blocks = 0;
    for (unsigned i = 0; i < 240; ++i) {   // the storage driver comes up a while into the boot
        if (native_ufs_part_find("oops", &f.lun, &f.first, &blocks)) break;
        IOSleep(500);
    }
    if (!blocks) return false;
    native_wifi_mhi[2] = f.lun | (uint32_t)f.first << 8;
    uint32_t which = 1;
    PE_parse_boot_argn("sheng-wifi-fw", &which, sizeof which);   // 2: the second image of the container (amss2.bin)
    const char *wanted = which == 2 ? "amss2.bin" : "amss.bin";
    uint8_t *header = static_cast<uint8_t *>(IOMallocData(4096));
    if (!header) return false;
    bool found = false;
    if (native_ufs_part_read(f.lun, f.first, 1, header) && !memcmp(header, "WFW1", 4)) {
        uint32_t count;
        memcpy(&count, header + 4, 4);
        for (uint32_t i = 0; i < count && i < 100; ++i) {
            const uint8_t *e = header + 8 + 32 * i;
            if (!strncmp((const char *)e, wanted, 24)) {memcpy(&f.amssOffset, e + 24, 4); memcpy(&f.amssSize, e + 28, 4); found = true;}
        }
    }
    IOFreeData(header, 4096);
    return found && f.amssSize && !(f.amssOffset & 4095);
}

struct BarIo {volatile uint32_t *m;};
uint32_t barRead(void *ctx, uint32_t reg) {return static_cast<BarIo *>(ctx)->m[reg / 4];}
uint32_t gTraceWrites, gSkipReg;   // a register whose write is left out (the boot loader's doorbell)
uint32_t gSkipped;
   // while non-zero every register write is announced in the log and followed by a pause long enough for a log slot to be written
void barWrite(void *ctx, uint32_t reg, uint32_t value) {
    if (gTraceWrites) {
        native_wifi_mhi[14] = (gTraceWrites << 16) | (reg & 0xffff); native_wifi_mhi[13] = value;
        IOSleep(2500);
        ++gTraceWrites;
    }
    if (gSkipReg && reg == gSkipReg) {gSkipped = value | 0x80000000u; return;}
    static_cast<BarIo *>(ctx)->m[reg / 4] = value; __asm__ volatile("dsb sy" ::: "memory");
}

void put64(volatile uint32_t *regs, uint32_t lowOffset, uint64_t value) {regs[lowOffset / 4] = (uint32_t)value; regs[(lowOffset + 4) / 4] = (uint32_t)(value >> 32);}

// The device context arrays and rings (mhi_init_dev_ctxt): 128 channel contexts left disabled, two event rings (control 32 elements, data 256) and the
// primary command ring, in one buffer; then the registers that point the device at them (mhi_init_mmio).
uint8_t *gEvVa[2]; uint8_t *gCtxVa; uint8_t *gSinkVa;
void programContexts(volatile uint32_t *m, Dma &c, uint64_t mapped) {
    constexpr uint32_t kCtx = 44, kChannels = 128, kEvents = 2, kCmdEl = 8, kEl = 16;
    const uint32_t evLen[kEvents] = {32 * kEl, 256 * kEl};
    uint8_t *va = c.va;
    gCtxVa = va;
    const uint64_t pa = c.pa;
    const uint32_t chOff = 0, evOff = kChannels * kCtx, cmdOff = evOff + kEvents * kCtx;
    uint32_t ringOff = (cmdOff + kCtx + 4095) & ~4095u;
    uint32_t cur = ringOff;
    auto w32 = [&](uint32_t at, uint32_t v) {memcpy(va + at, &v, 4);};
    auto w64 = [&](uint32_t at, uint64_t v) {memcpy(va + at, &v, 8);};
    uint64_t evBase[kEvents];
    for (uint32_t i = 0; i < kEvents; ++i) {
        evBase[i] = pa + cur;
        gEvVa[i] = va + cur;
        const uint32_t at = evOff + i * kCtx;
        w32(at, i == 0 ? 0 : 1u << 16);          // interrupt moderation 0 / 1 ms
        w32(at + 4, 1);                          // MHI_ER_TYPE_VALID
        w32(at + 8, i + 1);                      // MSI vector
        w64(at + 12, evBase[i]);
        w64(at + 20, evLen[i]);
        w64(at + 28, evBase[i]);                 // read pointer
        w64(at + 36, evBase[i] + evLen[i] - kEl);   // write pointer: the whole ring is free for the device
        cur += evLen[i];
    }
    const uint64_t cmdBase = pa + cur;
    w64(cmdOff + 12, cmdBase);
    w64(cmdOff + 20, kCmdEl * kEl);
    w64(cmdOff + 28, cmdBase);
    w64(cmdOff + 36, cmdBase);
    __asm__ volatile("dsb sy" ::: "memory");
    put64(m, 0x58, pa + chOff);                  // CCABAP
    put64(m, 0x60, pa + evOff);                  // ECABAP
    put64(m, 0x68, pa + cmdOff);                 // CRCBAP
    put64(m, 0x80, 0); put64(m, 0x88, 0xffffffffull);   // control base and limit
    put64(m, 0x98, 0); put64(m, 0xa0, 0xffffffffull);   // data base and limit
    m[0x10 / 4] = (m[0x10 / 4] & ~0xff000000u & ~0x00ff0000u) | (kEvents << 16);   // MHICFG: NER = 2, NHWER = 0
    // event ring doorbells (ERDBOFF + 8 * ring): the write pointers
    const uint32_t erdb = m[0x20 / 4];
    if (erdb + 8 * kEvents <= mapped) for (uint32_t i = 0; i < kEvents; ++i) put64(m, erdb + 8 * i, evBase[i] + evLen[i] - kEl);
    __asm__ volatile("dsb sy" ::: "memory");
}

// The endpoint's and the root port's configuration view, kept for looking at the link while the boot loader runs. When the endpoint answers in
// configuration space but its BAR0 or memory enable was cleared (its PCIe core restarting), they are written back.
volatile uint32_t *gCfg, *gDbi;
uint32_t gBar0, gRestored;
void linkSnapshot(volatile uint32_t *out) {
    out[0] = gCfg[0]; out[1] = gCfg[1]; out[2] = gCfg[0x10 / 4]; out[3] = gCfg[0x14 / 4];
    out[4] = gDbi[0x80 / 4]; out[5] = gDbi[kPortDebug0 / 4]; out[6] = gDbi[0x20 / 4]; out[7] = gDbi[1];
}
void linkRecover() {
    if (gCfg[0] != 0x110717cb) return;
    if ((gCfg[0x10 / 4] & ~0xfu) != (gBar0 & ~0xfu) || (gCfg[1] & 6u) != 6u) {
        gCfg[0x10 / 4] = gBar0; gCfg[0x14 / 4] = 0;
        gCfg[1] = (gCfg[1] & 0xffffu) | 0x6u;
        __asm__ volatile("dsb sy" ::: "memory");
        ++gRestored;
    }
}

// The chip's own power-up recipe (ath12k_pci_power_up): the boot loader is only started on a chip whose power control, interrupts and warm-boot marker were
// put in order first. Chip registers above 0x80000 are seen through a window: the window register at BAR + 0x310c takes 0x40000000 | (offset & GENMASK(24, 19)),
// (the field value, offset bits 24 to 19 shifted down), the register is then at BAR + 0x80000 + (offset & 0x7ffff); the MHI block (0x1e0e100 and up) is at the start of the BAR.
uint32_t gWindow = 0xffffffffu;
uint32_t chipRead(volatile uint32_t *m, uint32_t off) {
    if (off < 0x80000) return m[off / 4];
    const uint32_t window = (off >> 19) & 0x3fu;
    if (window != gWindow) {m[0x310c / 4] = 0x40000000u | window; (void)m[0x310c / 4]; gWindow = window;}
    if (off >= 0x1e0e100u && off <= 0x1e0effcu) return m[((off - 0x1e0e100u) & 0x7ffffu) / 4];
    return m[(0x80000u + (off & 0x7ffffu)) / 4];
}
void chipWrite(volatile uint32_t *m, uint32_t off, uint32_t value) {
    if (off < 0x80000) {m[off / 4] = value; __asm__ volatile("dsb sy" ::: "memory"); return;}
    const uint32_t window = (off >> 19) & 0x3fu;
    if (window != gWindow) {m[0x310c / 4] = 0x40000000u | window; (void)m[0x310c / 4]; gWindow = window;}
    if (off >= 0x1e0e100u && off <= 0x1e0effcu) m[((off - 0x1e0e100u) & 0x7ffffu) / 4] = value;
    else m[(0x80000u + (off & 0x7ffffu)) / 4] = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
// ath12k_pci_sw_reset(power on). Values seen along the way go to native_wifi_chip: [0] LTSSM word, [1] hot reset word, [2] Q6 cookie, [3] warm entry,
// [4] warm entry after clearing, [5] reset cause, [6] global reset word before, [7] after, [8] MHISTATUS before the MHI reset, [9] MHISTATUS after, [10] EE after.
void chipPowerUp(volatile uint32_t *m) {
    volatile uint32_t *c = native_wifi_chip;
    gWindow = 0xffffffffu;
    uint32_t v = chipRead(m, 0x1e081b0u);
    for (unsigned i = 0; v != 0x111u && i < 5; ++i) {
        if (v == 0xffffffffu) IOSleep(5);
        chipWrite(m, 0x1e081b0u, 0x111u);
        v = chipRead(m, 0x1e081b0u);
    }
    c[0] = v;
    v = chipRead(m, 0x1e40304u); v |= 0x10u; chipWrite(m, 0x1e40304u, v); c[1] = chipRead(m, 0x1e40304u);
    IOSleep(5);
    chipWrite(m, 0x1e08228u, 0xffffffffu);   // interrupts left over from a hot reset
    v = chipRead(m, 0x1f8031cu); v &= ~0x4u; chipWrite(m, 0x1f8031cu, v);
    chipWrite(m, 0x360, 0); chipWrite(m, 0x368, 0); chipWrite(m, 0x394, 0); chipWrite(m, 0x39c, 0);   // BHIe vector doorbells and status
    c[2] = chipRead(m, 0x1f80500u);
    c[3] = chipRead(m, 0x1f80504u);
    IOSleep(10);
    chipWrite(m, 0x1f80504u, 0);             // keeps the Q6 off the warm boot path
    IOSleep(10);
    c[4] = chipRead(m, 0x1f80504u);
    c[5] = chipRead(m, 0x1f8060cu);          // read-clear
    v = chipRead(m, 0x3008u); c[6] = v;
    chipWrite(m, 0x3008u, v | 1u); (void)chipRead(m, 0x3008u);
    IOSleep(10);
    chipWrite(m, 0x3008u, v & ~1u); (void)chipRead(m, 0x3008u);
    IOSleep(10);
    c[7] = chipRead(m, 0x3008u);
    m[0x310c / 4] = 0x40000000u | (gWindow == 0xffffffffu ? 0u : gWindow);   // the window register is cleared by the reset; put the cached window back
    (void)m[0x310c / 4];
    c[8] = m[0x48 / 4];
    m[0x38 / 4] = 2;                         // MHICTRL reset: clears a SYSERR left by the global reset
    __asm__ volatile("dsb sy" ::: "memory");
    IOSleep(10);
    c[9] = m[0x48 / 4];
    c[10] = m[(m[0x28 / 4] + 0x28) / 4];
}
// Without ASPM during the load, and MSI enabled (the endpoint raises its completions as MSI writes; they land in a sink in the DMA buffer, polled later).
void endpointLinkSetup(uint64_t sinkAddress) {
    uint8_t cap = (uint8_t)(gCfg[0x34 / 4] & 0xff);
    for (unsigned guard = 0; cap >= 0x40 && guard < 48; ++guard) {
        const uint32_t w = gCfg[cap / 4];
        const uint8_t id = (uint8_t)w, next = (uint8_t)(w >> 8);
        if (id == 0x10) {   // PCI Express: link control, ASPM bits
            const uint32_t lc = gCfg[(cap + 0x10) / 4];
            native_wifi_chip[11] = lc;
            gCfg[(cap + 0x10) / 4] = lc & ~3u;
        } else if (id == 0x05) {   // MSI: address, data, then 4 vectors and enable
            const bool is64 = (w >> 16) & 0x80;
            gCfg[(cap + 4) / 4] = (uint32_t)sinkAddress;
            if (is64) {gCfg[(cap + 8) / 4] = (uint32_t)(sinkAddress >> 32); gCfg[(cap + 0xc) / 4] = 0;}
            else gCfg[(cap + 8) / 4] = 0;
            gCfg[cap / 4] = (w & 0xffffu) | (((w >> 16) & ~0x70u) | 0x20u | 1u) << 16;
            native_wifi_chip[12] = gCfg[cap / 4];
        }
        cap = next;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}


// ---- Stage 6: the WiFi module (Ath12kWifi) on this board. It does its own chip power-up, MHI boot, QRTR/QMI and driver start; the board provides
// register access, configuration space, DMA memory from the endpoint's SMMU window, firmware files from the container, a clock and a log.
struct Arena {uint8_t *va; uint64_t iova; size_t bytes; struct Block {uint32_t off, size; bool used;} blocks[16384]; unsigned count;} gArena;
bool arenaInit() {
    if (&native_ufs_extra_va == nullptr || !native_ufs_extra_va || !native_ufs_extra_bytes) return false;
    gArena.va = native_ufs_extra_va; gArena.iova = native_ufs_extra_iova; gArena.bytes = native_ufs_extra_bytes;
    gArena.blocks[0] = {4096, (uint32_t)(gArena.bytes - 4096), false}; gArena.count = 1;   // the first page is the interrupt sink
    bzero(gArena.va, gArena.bytes);
    return true;
}
// Buffers the endpoint reads or writes sit at addresses aligned to their size rounded up to a power of two, with no upper limit: the boot loader
// refuses an image address that is only page aligned, before it makes any transfer (the registers that name the address and size of a failed transfer
// stay zero), the firmware's host memory regions (several MiB) go through the same address translation, and an IOMMU-backed DMA allocator hands out
// addresses aligned that way.
void pLog(int level, const char *line);
void *arenaAlloc(size_t n, uint64_t *bus) {
    n = (n + 63) & ~(size_t)63;
    if (n >= 4096) n = (n + 4095) & ~(size_t)4095;
    uint32_t align = 64;
    if (n >= 4096) {align = 4096; while (align < n) align <<= 1;}
    for (unsigned i = 0; i < gArena.count; ++i) {
        Arena::Block &b = gArena.blocks[i];
        if (b.used) continue;
        const uint32_t start = (b.off + align - 1) & ~(align - 1), pad = start - b.off;
        if (b.size < pad + n) continue;
        const bool tail = b.size > pad + n;
        if (gArena.count + (pad ? 1 : 0) + (tail ? 1 : 0) > 16384) continue;
        const unsigned extra = (pad ? 1u : 0u) + (tail ? 1u : 0u);
        const Arena::Block whole = b;
        memmove(&gArena.blocks[i + extra], &gArena.blocks[i], (gArena.count - i) * sizeof(Arena::Block));
        gArena.count += extra;
        unsigned at = i;
        if (pad) {gArena.blocks[at] = {whole.off, pad, false}; ++at;}
        gArena.blocks[at] = {start, (uint32_t)n, true};
        if (tail) gArena.blocks[at + 1] = {start + (uint32_t)n, whole.size - pad - (uint32_t)n, false};
        uint8_t *p = gArena.va + start;
        bzero(p, n);
        if (bus) *bus = gArena.iova + start;
        return p;
    }
    uint32_t freeTotal = 0, freeMax = 0;
    for (unsigned i = 0; i < gArena.count; ++i) if (!gArena.blocks[i].used) {freeTotal += gArena.blocks[i].size; if (gArena.blocks[i].size > freeMax) freeMax = gArena.blocks[i].size;}
    char line[96];
    snprintf(line, sizeof line, "dma window full: want %zu free %u largest %u blocks %u", n, freeTotal, freeMax, gArena.count);
    pLog(3, line);
    // used blocks by size class: up to 4 KiB, 64 KiB, 512 KiB, larger (count and KiB)
    uint32_t cnt[4] = {0, 0, 0, 0}, kib[4] = {0, 0, 0, 0}, big[4] = {0, 0, 0, 0};
    for (unsigned i = 0; i < gArena.count; ++i) if (gArena.blocks[i].used) {
        const uint32_t sz = gArena.blocks[i].size;
        const unsigned c = sz <= 4096 ? 0 : sz <= 65536 ? 1 : sz <= 524288 ? 2 : 3;
        ++cnt[c]; kib[c] += sz / 1024;
        for (unsigned k = 0; k < 4; ++k) if (sz > big[k]) {for (unsigned m = 3; m > k; --m) big[m] = big[m - 1]; big[k] = sz; break;}
    }
    snprintf(line, sizeof line, "dma used: <=4K %u/%uK <=64K %u/%uK <=512K %u/%uK more %u/%uK", cnt[0], kib[0], cnt[1], kib[1], cnt[2], kib[2], cnt[3], kib[3]);
    pLog(3, line);
    snprintf(line, sizeof line, "dma largest used: %u %u %u %u", big[0], big[1], big[2], big[3]);
    pLog(3, line);
    return nullptr;
}
void arenaFree(void *p) {
    if (!p || (uint8_t *)p < gArena.va || (uint8_t *)p >= gArena.va + gArena.bytes) return;
    const uint32_t off = (uint32_t)((uint8_t *)p - gArena.va);
    for (unsigned i = 0; i < gArena.count; ++i) {
        if (gArena.blocks[i].off != off || !gArena.blocks[i].used) continue;
        gArena.blocks[i].used = false;
        if (i + 1 < gArena.count && !gArena.blocks[i + 1].used) {gArena.blocks[i].size += gArena.blocks[i + 1].size; memmove(&gArena.blocks[i + 1], &gArena.blocks[i + 2], (gArena.count - i - 2) * sizeof(Arena::Block)); --gArena.count;}
        if (i > 0 && !gArena.blocks[i - 1].used) {gArena.blocks[i - 1].size += gArena.blocks[i].size; memmove(&gArena.blocks[i], &gArena.blocks[i + 1], (gArena.count - i - 1) * sizeof(Arena::Block)); --gArena.count;}
        return;
    }
}

uint64_t pNow() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
void pSleep(unsigned ms) {IOSleep(ms);}
void pPoll() {}
// Each line carries the milliseconds since the first one; a line equal to one of the last sixteen is only counted, so that the messages the driver
// repeats every round (the seven interrupt groups it polls each millisecond, a send that waits for credits) cannot push the history out of the ring.
uint32_t gLogRepeats;
volatile uint32_t *gNwBar;   // the endpoint's registers, for the RAM dump reader
void rddmScan();
extern "C" int boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset, const void *data, uint32_t size) __attribute__((weak_import));
bool has(const char *s, const char *w) {for (; *s; ++s) {size_t k = 0; while (w[k] && s[k] == w[k]) ++k; if (!w[k]) return true;} return false;}
bool hasRddm(const char *s) {return has(s, "MHI_CB_EE_RDDM");}
void pLog(int level, const char *line) {
    // one line per channel would push the start-up messages out of the ring
    if (has(line, "chan scan list") || has(line, "mac channel [")) {++gLogRepeats; return;}
    // nor may the lines of every received buffer and interrupt (the beacon count is in [wifi-nw])
    if (level >= 7 && (has(line, "htc rx completion") || has(line, "rx ce pipe") || has(line, "ext irq:") || has(line, "event mgmt rx") ||
                       has(line, "mgmt rx event") || has(line, "chan info vdev_id") || has(line, "scan event foreign channel"))) {++gLogRepeats; return;}
    static uint64_t first;
    static uint32_t recent[16];
    uint32_t h = 2166136261u;
    for (const char *c = line; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    h ^= static_cast<uint32_t>(level);
    for (uint32_t r : recent) if (r == h) {++gLogRepeats; return;}
    memmove(&recent[1], &recent[0], sizeof recent - sizeof recent[0]);
    recent[0] = h;
    const uint64_t now = pNow();
    if (!first) first = now;
    const uint32_t i = __atomic_fetch_add(const_cast<uint32_t *>(&native_wifi_text_n), 1u, __ATOMIC_RELAXED) % 512;
    snprintf(native_wifi_text[i], sizeof native_wifi_text[i], "%u %d %.100s", static_cast<uint32_t>(now - first), level, line);
    if (level <= 6) {
        const uint32_t j = __atomic_fetch_add(const_cast<uint32_t *>(&native_wifi_note_n), 1u, __ATOMIC_RELAXED) % 128;
        memcpy(native_wifi_note[j], native_wifi_text[i], sizeof native_wifi_note[j]);
    }
    // when the driver reports a command stuck: what the root port saw (device status, link, advanced error reporting)
    static bool rootShown;
    if (!rootShown && gDbi && has(line, "stuck: wmi credits")) {
        rootShown = true;
        char text[112];
        snprintf(text, sizeof text, "root port: devctl/sta %08x link %08x ue %08x mask %08x ce %08x rootsta %08x src %08x",
                 gDbi[0x78 / 4], gDbi[0x80 / 4], gDbi[0x104 / 4], gDbi[0x108 / 4], gDbi[0x110 / 4], gDbi[0x130 / 4], gDbi[0x134 / 4]);
        pLog(3, text);
        snprintf(text, sizeof text, "root port: aer header %08x %08x %08x %08x ltssm-debug %08x", gDbi[0x11c / 4], gDbi[0x120 / 4], gDbi[0x124 / 4], gDbi[0x128 / 4], gDbi[kPortDebug0 / 4]);
        pLog(3, text);
    }
    // the first time the device reports its RAM dump state: did its DMA fault in the SMMU (global FSR, context FSR, FAR, FSYNR0)?
    static bool faultShown;
    if (!faultShown && &native_ufs_extra_fault && hasRddm(line)) {
        faultShown = true;
        uint32_t f[10] = {};
        native_ufs_extra_fault(f);
        char text[112];
        snprintf(text, sizeof text, "smmu at rddm: gfsr %08x cb-fsr %08x far %08x%08x fsynr0 %08x sctlr %08x", f[0], f[2], f[4], f[3], f[5], f[6]);
        pLog(3, text);
        if (&native_ufs_extra_atos) {
            snprintf(text, sizeof text, "smmu window sweep: tested %u pages, %u faults, %u elsewhere, first bad %08x", native_ufs_extra_atos[0], native_ufs_extra_atos[1], native_ufs_extra_atos[2], native_ufs_extra_atos[3]);
            pLog(3, text);
            snprintf(text, sizeof text, "smmu window sweep: first page par %08x%08x, table pa %08x%08x", native_ufs_extra_atos[5], native_ufs_extra_atos[4], native_ufs_extra_atos[7], native_ufs_extra_atos[6]);
            pLog(3, text);
        }
        rddmScan();
    }
}

// The device uploads its RAM dump into the buffers of the receive vector table the MHI core programmed at power up. Once the transfer is complete
// the dump is searched for the messages a firmware error leaves (an assertion or fatal error text, or a source file with a line number).
bool printable(uint8_t c) {return c >= 0x20 && c < 0x7f;}
bool looksLikeError(const char *t, size_t n) {
    for (size_t i = 0; i < n; ++i) if (t[i] == '%') return false;   // a format string of the image, not a message
    for (size_t i = 0; i + 3 < n; ++i) {
        if (t[i] == '.' && t[i + 1] == 'c' && t[i + 2] == ':' && t[i + 3] >= '0' && t[i + 3] <= '9') return true;
        if (i + 5 <= n && (!memcmp(t + i, "ssert", 5) || !memcmp(t + i, "SSERT", 5) || !memcmp(t + i, "FATAL", 5))) return true;
        if (i + 8 <= n && !memcmp(t + i, "rr_fatal", 8)) return true;
    }
    return false;
}
void rddmScan() {
    volatile uint32_t *m = gNwBar;
    if (!m) return;
    const uint32_t bhie = m[0x2c / 4];
    if (bhie == 0 || bhie > 0x1000 || (bhie & 3)) return;
    uint32_t status = 0;
    unsigned waited = 0;
    for (; waited < 400; ++waited) {status = m[(bhie + 0x78) / 4]; if ((status >> 30) >= 2) break; IOSleep(10);}
    const uint64_t vec = m[(bhie + 0x60) / 4] | (uint64_t)m[(bhie + 0x64) / 4] << 32;
    const uint32_t vecBytes = m[(bhie + 0x68) / 4];
    char text[112];
    snprintf(text, sizeof text, "rddm upload: status %08x after %u ms, table %llx %u bytes", status, waited * 10, (unsigned long long)vec, vecBytes);
    pLog(3, text);
    auto toVa = [](uint64_t iova, uint64_t n) -> const uint8_t * {
        if (iova < gArena.iova || iova + n > gArena.iova + gArena.bytes) return nullptr;
        return gArena.va + (iova - gArena.iova);
    };
    const uint8_t *table = toVa(vec, vecBytes);
    if (!table || (status >> 30) != 2) return;
    unsigned found = 0;
    uint64_t scanned = 0;
    uint32_t seen[48] = {};
    for (uint32_t e = 0; e + 16 <= vecBytes && found < 40; e += 16) {
        uint64_t addr, size;
        memcpy(&addr, table + e, 8); memcpy(&size, table + e + 8, 8);
        const uint8_t *seg = toVa(addr, size);
        if (!seg) continue;
        scanned += size;
        for (uint64_t i = 0; i < size && found < 40;) {
            if (!printable(seg[i])) {++i; continue;}
            uint64_t j = i;
            while (j < size && printable(seg[j]) && j - i < 200) ++j;
            const size_t n = (size_t)(j - i);
            if (n >= 10 && looksLikeError(reinterpret_cast<const char *>(seg + i), n)) {
                uint32_t h = 2166136261u;
                for (size_t k = 0; k < n; ++k) h = (h ^ seg[i + k]) * 16777619u;
                bool dup = false;
                for (unsigned k = 0; k < found && k < 48; ++k) if (seen[k] == h) dup = true;
                if (!dup) {
                    seen[found] = h;
                    ++found;
                    char body[85];
                    const size_t len = n < 84 ? n : 84;
                    memcpy(body, seg + i, len); body[len] = 0;
                    snprintf(text, sizeof text, "rddm %u+%llx: %s", e / 16, (unsigned long long)i, body);
                    for (char *c = text; *c; ++c) if (!printable((uint8_t)*c)) *c = '?';
                    pLog(3, text);
                }
            }
            i = j;
        }
    }
    snprintf(text, sizeof text, "rddm scan: %llu bytes, %u messages", (unsigned long long)scanned, found);
    pLog(3, text);
    // Context: every text in the crash record (segment 0 up to 0x1000), and the runtime log the firmware kept before it (segment 1, 0x78000 on): the
    // first as it is, the second without format strings, 4 bytes per word of the record shown where no text is.
    struct Range {uint32_t seg; uint64_t from, to; size_t min; bool formats;};
    const Range ranges[] = {{0, 0, 0x1000, 4, true}, {0, 0xec00, 0xf400, 4, true}, {1, 0x78000, 0x80000, 12, false}};
    unsigned lines = 0;
    for (const Range &r : ranges) {
        if ((r.seg + 1) * 16 > vecBytes) continue;
        uint64_t addr, size;
        memcpy(&addr, table + r.seg * 16, 8); memcpy(&size, table + r.seg * 16 + 8, 8);
        const uint8_t *seg = toVa(addr, size);
        if (!seg) continue;
        const uint64_t to = r.to < size ? r.to : size;
        for (uint64_t i = r.from; i < to && lines < 160;) {
            if (!printable(seg[i])) {++i; continue;}
            uint64_t j = i;
            while (j < to && printable(seg[j]) && j - i < 200) ++j;
            const size_t n = (size_t)(j - i);
            bool format = false;
            for (size_t k = 0; k < n; ++k) if (seg[i + k] == '%') format = true;
            if (n >= r.min && (r.formats || !format)) {
                char body[85];
                const size_t len = n < 84 ? n : 84;
                memcpy(body, seg + i, len); body[len] = 0;
                snprintf(text, sizeof text, "rddm-ctx %u+%llx: %s", r.seg, (unsigned long long)i, body);
                pLog(3, text);
                ++lines;
            }
            i = j;
        }
    }
    // the crash record's first 0x200 bytes as words (registers and addresses sit there)
    {
        uint64_t addr, size;
        memcpy(&addr, table, 8); memcpy(&size, table + 8, 8);
        const uint8_t *seg = toVa(addr, size);
        if (seg && size >= 0x200) for (uint32_t o = 0; o < 0x200; o += 32) {
            uint32_t w[8];
            memcpy(w, seg + o, 32);
            snprintf(text, sizeof text, "rddm-rec %03x: %08x %08x %08x %08x %08x %08x %08x %08x", o, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            pLog(3, text);
        }
    }
    // The whole dump goes to the boot log's export area, read back on the host: the receive vector table as table.bin and each of its buffers as
    // segNN.bin, in the container the log reader unpacks (SHEXR001 holding an M2VCACH1 file list). Its generation has the top bit set, which keeps
    // a later export from replacing it.
    if (!&boot_log_export_put) return;
    static const char root[] = "/rddm";
    uint64_t total = 8 + 4 + sizeof root - 1 + 4, files = 1;
    total += 24 + 9 + vecBytes;
    for (uint32_t e = 0; e + 16 <= vecBytes; e += 16) {
        uint64_t addr, size;
        memcpy(&addr, table + e, 8); memcpy(&size, table + e + 8, 8);
        if (toVa(addr, size)) {total += 24 + 9 + size; ++files;}
    }
    const uint64_t blobBytes = 16 + total;
    uint8_t *blob = blobBytes < (64u << 20) ? static_cast<uint8_t *>(IOMallocData(blobBytes)) : nullptr;
    if (!blob) {pLog(3, "rddm export: no memory"); return;}
    uint8_t *w = blob;
    auto put = [&w](const void *d, size_t n) {memcpy(w, d, n); w += n;};
    auto put32 = [&put](uint32_t v) {put(&v, 4);};
    auto file = [&](const char *name, const uint8_t *d, uint64_t n) {
        const uint32_t head[4] = {(uint32_t)strlen(name), 0644, 0, 0};
        put(head, 16); put(&n, 8); put(name, strlen(name)); put(d, n);
    };
    put("SHEXR001", 8); put32((uint32_t)total); put32(0);
    put("M2VCACH1", 8); put32(sizeof root - 1); put(root, sizeof root - 1); put32((uint32_t)files);
    file("table.bin", table, vecBytes);
    for (uint32_t e = 0; e + 16 <= vecBytes; e += 16) {
        uint64_t addr, size;
        memcpy(&addr, table + e, 8); memcpy(&size, table + e + 8, 8);
        const uint8_t *seg = toVa(addr, size);
        if (!seg) continue;
        char name[16];
        snprintf(name, sizeof name, "seg%02u.bin", e / 16);
        file(name, seg, size);
    }
    const uint32_t generation = 0x80000000u | (uint32_t)(pNow() & 0x7fffffff);
    const int rc = (uint64_t)(w - blob) == blobBytes ? boot_log_export_put(generation, (uint32_t)blobBytes, 0, blob, (uint32_t)blobBytes) : -100;
    IOFreeData(blob, blobBytes);
    snprintf(text, sizeof text, "rddm export: %llu bytes in %llu files, result %d", (unsigned long long)blobBytes, (unsigned long long)files, rc);
    pLog(3, text);
}
void pHalt() {for (;;) IOSleep(1000);}
void *hAlloc(size_t n) {
    uint8_t *p = static_cast<uint8_t *>(IOMallocData(n + 16));
    if (!p) return nullptr;
    *reinterpret_cast<size_t *>(p) = n + 16;
    return p + 16;
}
void hFree(void *q) {
    if (!q) return;
    uint8_t *p = static_cast<uint8_t *>(q) - 16;
    IOFreeData(p, *reinterpret_cast<size_t *>(p));
}
void *pDmaAlloc(size_t n, uint64_t *bus) {return arenaAlloc(n, bus);}
void pDmaFree(size_t, void *p, uint64_t) {arenaFree(p);}
uint64_t pDmaMap(void *p, size_t, int) {
    if ((uint8_t *)p < gArena.va || (uint8_t *)p >= gArena.va + gArena.bytes) return 0;
    return gArena.iova + ((uint8_t *)p - gArena.va);
}
void pDmaSync(uint64_t, size_t, int) {}

// Firmware files: the container in the partition named oops ('WFW1', count, then name[24] offset size), each file read once and kept.
struct FwCache {char name[24]; uint8_t *data; size_t size; bool tried;} gFw[8];
// The radio's address (wlan_mac.bin): this board's firmware reports none, so a locally administered address is made from /chosen/unique-chip-id,
// the same on every boot of the same image.
// The address itself: made from /chosen/unique-chip-id (FNV-1a, locally administered, unicast), the same at every boot.
bool boardMac(uint8_t m[6]) {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    uint64_t seed = 0;
    if (id && id->getLength() >= sizeof seed) memcpy(&seed, id->getBytesNoCopy(), sizeof seed);
    OSSafeReleaseNULL(chosen);
    if (!seed) return false;
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < 8; ++i) h = (h ^ ((seed >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    for (unsigned i = 0; i < 6; ++i) m[i] = (uint8_t)(h >> (8 * i));
    m[0] = (uint8_t)((m[0] & 0xfe) | 0x02);
    return true;
}
int macFile(const uint8_t **data, size_t *size) {
    static char text[40]; static size_t len;
    if (!len) {
        uint8_t m[6];
        if (!boardMac(m)) return -1;
        len = (size_t)snprintf(text, sizeof text, "address=%02x%02x%02x%02x%02x%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    *data = (const uint8_t *)text; *size = len;
    return 0;
}
// The system's own record of the Wi-Fi address, as a boot loader leaves it in the device tree: the wireless daemon derives every network's
// private address from it (MobileGestalt WifiAddressData) and aborts a join without it.
void publishMac() {
    uint8_t m[6];
    if (!boardMac(m)) return;
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        if (!chosen->getProperty("mac-address-wifi0")) chosen->setProperty("mac-address-wifi0", m, sizeof m);
        chosen->release();
    }
    if (IORegistryEntry *wlan = IORegistryEntry::fromPath("/arm-io/wlan", gIODTPlane)) {
        if (!wlan->getProperty("local-mac-address")) wlan->setProperty("local-mac-address", m, sizeof m);
        wlan->release();
    }
}
int pFwGet(const char *path, const uint8_t **data, size_t *size) {
    const char *base = path;
    for (const char *c = path; *c; ++c) if (*c == '/') base = c + 1;
    if (!strcmp(base, "wlan_mac.bin")) return macFile(data, size);
    char want[24]; strlcpy(want, base, sizeof want);
    uint32_t which = 1;
    PE_parse_boot_argn("sheng-wifi-fw", &which, sizeof which);
    if (!strcmp(want, "amss.bin") && which == 2) strlcpy(want, "amss2.bin", sizeof want);
    FwCache *slot = nullptr;
    for (auto &f : gFw) {if (f.tried && !strncmp(f.name, want, 24)) {slot = &f; break;} }
    if (!slot) for (auto &f : gFw) if (!f.tried) {slot = &f; strlcpy(f.name, want, sizeof f.name); f.tried = true; break;}
    if (!slot) return -1;
    if (!slot->data && slot->size == 0) {
        uint32_t lun = 0; uint64_t first = 0, blocks = 0;
        if (&native_ufs_part_find == nullptr || !native_ufs_part_find("oops", &lun, &first, &blocks)) return -1;
        uint8_t *header = static_cast<uint8_t *>(IOMallocData(4096));
        if (!header) return -1;
        uint32_t offset = 0, bytes = 0;
        if (native_ufs_part_read(lun, first, 1, header) && !memcmp(header, "WFW1", 4)) {
            uint32_t count; memcpy(&count, header + 4, 4);
            for (uint32_t i = 0; i < count && i < 100; ++i) {
                const uint8_t *e = header + 8 + 32 * i;
                if (!strncmp((const char *)e, want, 24)) {memcpy(&offset, e + 24, 4); memcpy(&bytes, e + 28, 4);}
            }
        }
        IOFreeData(header, 4096);
        if (!bytes || (offset & 4095)) return -1;
        const size_t rounded = (bytes + 4095) & ~(size_t)4095;
        uint8_t *buf = static_cast<uint8_t *>(IOMallocData(rounded));
        if (!buf) return -1;
        if (!native_ufs_part_read(lun, first + offset / 4096, (uint32_t)(rounded / 4096), buf)) {IOFreeData(buf, rounded); return -1;}
        slot->data = buf; slot->size = bytes;
    }
    if (!slot->data) return -1;
    *data = slot->data; *size = slot->size;
    return 0;
}

// Configuration space of the endpoint: 32-bit accesses through the iATU window; offsets with bit 31 set are relative to the PCI Express capability.
uint32_t gPcieCap, gMsiCap;
int findCaps() {
    uint8_t cap = (uint8_t)(gCfg[0x34 / 4] & 0xff);
    for (unsigned guard = 0; cap >= 0x40 && guard < 48; ++guard) {
        const uint32_t w = gCfg[cap / 4];
        if ((uint8_t)w == 0x10) gPcieCap = cap;
        if ((uint8_t)w == 0x05) gMsiCap = cap;
        cap = (uint8_t)(w >> 8);
    }
    return gPcieCap && gMsiCap;
}
int pCfgRead(int off, int size, uint32_t *v) {
    uint32_t o = (uint32_t)off;
    if (o & 0x80000000u) o = gPcieCap + (o & 0x7fffffffu);
    const uint32_t w = gCfg[(o & ~3u) / 4];
    *v = size == 4 ? w : size == 2 ? (w >> ((o & 2) * 8)) & 0xffff : (w >> ((o & 3) * 8)) & 0xff;
    return 0;
}
int pCfgWrite(int off, int size, uint32_t v) {
    uint32_t o = (uint32_t)off;
    if (o & 0x80000000u) o = gPcieCap + (o & 0x7fffffffu);
    if (size == 4) {gCfg[o / 4] = v; return 0;}
    const unsigned shift = (size == 2 ? (o & 2) : (o & 3)) * 8;
    const uint32_t mask = (size == 2 ? 0xffffu : 0xffu) << shift;
    gCfg[(o & ~3u) / 4] = (gCfg[(o & ~3u) / 4] & ~mask) | ((v << shift) & mask);
    return 0;
}
int pMsiTarget(uint32_t *lo, uint32_t *hi, uint32_t *data) {*lo = (uint32_t)gArena.iova; *hi = (uint32_t)(gArena.iova >> 32); *data = 0; return 0;}

// The network side (WifiNet.cpp): its classes are registered with the kernel's module like the other late classes, then the interface is made.
extern "C" void native_wifi_logf(int level, const char *fmt, ...) {
    char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    pLog(level, line);
}
void netStart() {
    struct nw_wifi_status status; nw_wifi_status_get(&status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {native_wifi_logf(3, "wifi: network classes: no module handle"); return;}
    native_wifi_net_static_init();
    const OSReturn r = OSMetaClass::postModLoad(handle);
    if (r) {native_wifi_logf(3, "wifi: network classes not registered: %d", (int)r); return;}
    sheng_wifi_net_start(status.mac);
}

// ---- the endpoint's MSIs as a real interrupt ----
// The root complex's own MSI receiver (DesignWare iMSI): an inbound write to the address in MSI_ADDR is taken by the controller instead of
// memory, sets bit <data> of controller 0's status and raises PCIe0's "msi0" line (SPI 141, level). The address is the arena's first page,
// the one the endpoint was already given as its MSI target, so nothing changes for the driver. The handler clears the status and wakes
// the WiFi thread, which otherwise sleeps: no polling while the link is quiet.
constexpr uint32_t kMsiAddrLo = 0x820, kMsiAddrHi = 0x824, kMsiEnable0 = 0x828, kMsiMask0 = 0x82c, kMsiStatus0 = 0x830, kMsiSpi = 141;
Window gMsiDbi;
volatile uint32_t gMsiCount, gMsiBits, gMsiWake;
bool gMsiAttached;
void msiIrq(void *) {
    const uint32_t status = gMsiDbi.va[kMsiStatus0 / 4];
    if (status) gMsiDbi.va[kMsiStatus0 / 4] = status;   // write-one-to-clear: the level line drops before the end of interrupt
    gMsiCount = gMsiCount + 1;
    gMsiBits = gMsiBits | status;
    // a line with nothing to clear would storm: turn the receiver off and let the loop poll again
    static uint32_t empty;
    empty = status ? 0 : empty + 1;
    if (empty > 100000) {gMsiDbi.va[kMsiEnable0 / 4] = 0; gMsiAttached = false;}
    gMsiWake = 1;
    thread_wakeup(event_t(&gMsiWake));
}
int msiAttach() {
    if (!armcpu_spi_attach) return -78;
    if (!gMsiDbi.va && !mapWindow(gMsiDbi, kDbi, 0x1000)) return -12;
    gMsiDbi.va[kMsiAddrLo / 4] = uint32_t(gArena.iova);
    gMsiDbi.va[kMsiAddrHi / 4] = uint32_t(gArena.iova >> 32);
    gMsiDbi.va[kMsiMask0 / 4] = 0;
    gMsiDbi.va[kMsiEnable0 / 4] = 0xffffffffu;
    const int rc = armcpu_spi_attach(32 + kMsiSpi, false, msiIrq, nullptr, nullptr, nullptr);
    gMsiAttached = rc == 0;
    return rc;
}
// Sleeps until an MSI, a frame to send (sheng_wifi_wake) or ms milliseconds pass; returns at once if one came since the last wait.
void msiWait(unsigned ms) {
    if (__atomic_exchange_n(&gMsiWake, 0u, __ATOMIC_ACQ_REL)) return;
    assert_wait_timeout(event_t(&gMsiWake), THREAD_UNINT, ms, NSEC_PER_MSEC);
    if (__atomic_exchange_n(&gMsiWake, 0u, __ATOMIC_ACQ_REL)) {clear_wait(current_thread(), THREAD_AWAKENED); return;}
    thread_block(THREAD_CONTINUE_NULL);
    gMsiWake = 0;
}
}  // namespace
// The network side calls this after it queued a frame: the WiFi thread sends it now instead of at its next wake.
extern "C" void sheng_wifi_wake(void) {gMsiWake = 1; thread_wakeup(event_t(&gMsiWake));}
namespace {

void wifiStack(volatile uint32_t *m, uint64_t barBytes) {
    volatile uint32_t *st = native_wifi_mhi;
    if (!arenaInit()) {st[0] = (uint32_t)-11; return;}
    pLog(6, native_ufs_extra_uncached ? "wifi: DMA window mapped uncached" : "wifi: DMA window mapped cached (no second mapping)");
    if (!findCaps()) {st[0] = (uint32_t)-12; return;}
    static const struct lx_platform plat = {pNow, pSleep, pPoll, pLog, pHalt, hAlloc, hFree, pDmaAlloc, pDmaFree, pDmaMap, pDmaSync, pFwGet};
    static struct nw_board board;
    gNwBar = m;
    board.bar = const_cast<uint32_t *>(m); board.bar_len = barBytes; board.device_id = (uint16_t)(gCfg[0] >> 16); board.msi_cap = (uint16_t)gMsiCap; board.irq_base = 1000;
    board.cfg_read = pCfgRead; board.cfg_write = pCfgWrite; board.msi_target = pMsiTarget; board.map_phys = nullptr;
    // the board's address window for the endpoint to reach sleep clock and sibling blocks is already set up by the earlier stages
    st[0] = 20;
    const int r = nw_wifi_start(&plat, &board);
    st[0] = r ? (uint32_t)(0x80000000u | (uint32_t)(-r & 0xffff)) : 21;
    bool netDone = false;
    // The loop runs on interrupts once the MSI line is attached: it sleeps until the endpoint signals, a frame is queued to send or the
    // module's next timer may be due (10 ms), and goes round again at once while frames move. Without the line it falls back to polling
    // every millisecond.
    const int msi = msiAttach();
    pLog(6, msi ? "wifi: MSI line not attached, polling" : "wifi: MSI line attached (SPI 141), sleeping between interrupts");
    if (msi) native_wifi_logf(4, "wifi: MSI attach result %d", msi);
    uint64_t nextReport = 0;
    for (uint64_t n = 0;; ++n) {
        const int busy = nw_wifi_step();
        if (!netDone && nw_wifi_stats.stage >= 4) {netDone = true; netStart();}
        if (!busy) {if (gMsiAttached) msiWait(10); else IOSleep(1);}
        const uint64_t now = pNow();
        if (gMsiAttached && now >= nextReport) {nextReport = now + 30000; native_wifi_logf(6, "wifi: %u MSIs so far, vectors seen %08x", gMsiCount, gMsiBits);}
        if ((n & 63) == 0) st[1] = nw_wifi_stats.stage | nw_wifi_stats.scans << 8 | nw_wifi_stats.bss_seen << 16;
    }
}

// The host's half of the event rings (mhi_process_ctrl_ev_ring): the device advances the ring context's read pointer past each event it posts; the host takes
// them and tells the device the ring is free again by moving the write pointer to just behind that and ringing the ring's doorbell. Returns how many rings moved.
unsigned eventAck(volatile uint32_t *m) {
    constexpr uint32_t kCtx = 44, kChannels = 128;
    unsigned moved = 0;
    const uint32_t erdb = m[0x20 / 4];
    static uint64_t acked[2];
    for (unsigned r = 0; r < 2; ++r) {
        uint8_t *ctx = gCtxVa + kChannels * kCtx + r * kCtx;
        uint64_t base, len, rp;
        memcpy(&base, ctx + 12, 8); memcpy(&len, ctx + 20, 8); memcpy(&rp, ctx + 28, 8);
        if (rp == acked[r] || rp < base || rp >= base + len) continue;
        acked[r] = rp;
        const uint64_t wp = rp == base ? base + len - 16 : rp - 16;
        memcpy(ctx + 36, &wp, 8);
        __asm__ volatile("dc civac, %0\n dsb sy" :: "r"(ctx + 36) : "memory");
        m[(erdb + 8 * r) / 4] = (uint32_t)wp; m[(erdb + 8 * r + 4) / 4] = (uint32_t)(wp >> 32);
        ++moved;
    }
    return moved;
}

// What the device left for the host: the first events of both event rings, the event ring contexts' read and write pointers, the MSI sink and the BHIe block.
void eventSnapshot(volatile uint32_t *m, uint32_t slot) {
    volatile uint32_t *o = native_wifi_ev + slot * 24;
    if (!gCtxVa || !gEvVa[0]) return;
    for (unsigned i = 0; i < 8; ++i) {uint32_t w; memcpy(&w, gEvVa[0] + 4 * i, 4); o[i] = w;}      // ring 0: two entries
    for (unsigned i = 0; i < 4; ++i) {uint32_t w; memcpy(&w, gEvVa[1] + 4 * i, 4); o[8 + i] = w;}   // ring 1: one entry
    for (unsigned r = 0; r < 2; ++r) {
        uint32_t rp, wp;
        memcpy(&rp, gCtxVa + 128 * 44 + r * 44 + 28, 4); memcpy(&wp, gCtxVa + 128 * 44 + r * 44 + 36, 4);
        o[12 + 2 * r] = rp; o[13 + 2 * r] = wp;
    }
    for (unsigned i = 0; i < 2; ++i) {uint32_t w; memcpy(&w, gSinkVa + 4 * i, 4); o[16 + i] = w;}
    const uint32_t bhie = m[0x2c / 4];
    for (unsigned i = 0; i < 6; ++i) o[18 + i] = m[(bhie + 0x2c + 4 * i + (i >= 4 ? 4 : 0)) / 4];   // ADDR_LOW, ADDR_HIGH, SIZE, (0x38), DB, STATUS
}

void mhiBring(volatile uint32_t *m, uint64_t barBytes) {
    volatile uint32_t *st = native_wifi_mhi;
    const uint64_t t0 = mach_absolute_time();
    auto elapsed = [&]() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns); return (uint32_t)(ns / 1000000);};
    static Dma seg[kMaxSegments], sbl, table, ctx;
    // the endpoint's DMA is limited to 32 bits (the Linux driver sets that mask): see whether the allocator has any memory below 4 GiB at all
    const bool low = &native_ufs_extra_va != nullptr && native_ufs_extra_va && native_ufs_extra_bytes >= 7u << 20;
    native_wifi_mhi_ms[7] = low ? native_ufs_extra_iova : 0xdead0000u | st[11];
    for (uint32_t i = 0; i < kMaxSegments; ++i)
        if (!dmaAlloc(seg[i], kSegment)) {st[0] = (uint32_t)-3; st[10] = 100 + i; return;}
    sbl = seg[0];   // the boot loader is the first 512 KiB of the image, consecutive segments
    if (!dmaAlloc(table, 4096)) {st[0] = (uint32_t)-3; st[10] = 201; return;}
    if (!dmaAlloc(ctx, 32 * 1024)) {st[0] = (uint32_t)-3; st[10] = 202; return;}
    st[15] = (uint32_t)(seg[0].pa >> 12);   // where the buffers landed (page numbers), kept until the registers are read after the load
    st[8] = (uint32_t)(sbl.pa >> 12); st[9] = (uint32_t)(table.pa >> 12); st[12] = (uint32_t)(ctx.pa >> 12);
    st[0] = 1; native_wifi_mhi_ms[0] = elapsed();
    Fw fw = {};
    if (!findFirmware(fw)) {st[0] = (uint32_t)-1; return;}
    native_wifi_mhi_ms[1] = elapsed();
    st[3] = fw.amssSize;
    const uint32_t segments = (fw.amssSize + kSegment - 1) / kSegment;
    st[4] = segments;
    if (segments > kMaxSegments) {st[0] = (uint32_t)-2; return;}
    constexpr size_t kChunk = 128 * 1024;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kChunk));
    if (!bounce) {st[0] = (uint32_t)-3; st[10] = 2; return;}
    for (uint32_t done = 0; done < fw.amssSize; done += kChunk) {
        const uint32_t left = fw.amssSize - done, bytes = left < kChunk ? left : kChunk;
        if (!native_ufs_part_read(fw.lun, fw.first + (fw.amssOffset + done) / 4096, (bytes + 4095) / 4096, bounce)) {IOFreeData(bounce, kChunk); st[0] = (uint32_t)-2; return;}
        for (uint32_t off = 0; off < bytes; off += kSegment) memcpy(seg[(done + off) / kSegment].va, bounce + off, bytes - off < kSegment ? bytes - off : kSegment);
    }
    IOFreeData(bounce, kChunk);
    st[0] = 2; native_wifi_mhi_ms[2] = elapsed();
    uint64_t addresses[kMaxSegments];
    for (uint32_t i = 0; i < segments; ++i) addresses[i] = seg[i].pa;
    const uint32_t entries = mhi_bhie_table(reinterpret_cast<MhiBhieEntry *>(table.va), 4096 / sizeof(MhiBhieEntry), addresses, fw.amssSize, kSegment);
    if (!entries) {st[0] = (uint32_t)-3; st[10] = 3; return;}
    {
        uint32_t sum = 0;
        for (uint32_t off = 0; off < kSbl; off += 4) {uint32_t w; memcpy(&w, seg[0].va + off, 4); sum += w;}
        uint32_t first; memcpy(&first, seg[0].va, 4);
        native_wifi_cfg[28] = sum; native_wifi_cfg[29] = first; native_wifi_cfg[31] = fw.amssSize;
    }
    for (uint32_t off = 0; off < gDmaUsed; off += 64) __asm__ volatile("dc civac, %0" :: "r"(native_ufs_extra_va + off) : "memory");   // the endpoint reads DRAM
    __asm__ volatile("dsb sy" ::: "memory");
    st[0] = 3;
    // the endpoint's pass-through stream 0x1401 was set up by the storage driver when it came up (it reads sheng-wifi): slot | bank << 16, bit 31 set
    st[1] = native_ufs_extra_stream;
    if (!(st[1] & 0x80000000u)) {st[0] = (uint32_t)-4; return;}
    st[0] = 4; native_wifi_mhi_ms[3] = elapsed();
    static Dma sink;
    if (!dmaAlloc(sink, 4096)) {st[0] = (uint32_t)-3; st[10] = 203; return;}
    gSinkVa = sink.va;
    uint32_t level0 = 0;
    PE_parse_boot_argn("sheng-wifi", &level0, sizeof level0);
    if (level0 >= 4) {chipPowerUp(m); endpointLinkSetup(sink.pa);}
    BarIo io = {m};
    MhiBoot boot = {};
    MhiIo mio = {barRead, barWrite, &io, (uint32_t)barBytes};
    if (mhi_boot_open(&boot, &mio)) {st[0] = (uint32_t)-5; return;}
    // the log writer needs a while to start; the next step is the one that can end the boot, so let it write a few slots first
    st[0] = 4; st[13] = elapsed();
    uint32_t level = 0;
    if (!PE_parse_boot_argn("sheng-wifi", &level, sizeof level) || level < 3 || !low) return;   // the load itself starts with sheng-wifi=3
    IOSleep(8000);
    programContexts(m, ctx, barBytes);
    for (uint32_t off = 0; off < gDmaUsed; off += 64) __asm__ volatile("dc civac, %0" :: "r"(native_ufs_extra_va + off) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");   // the driver for this chip sets the device's context arrays up before the boot loader is started
    gTraceWrites = 1;
    gSkipReg = level >= 4 ? 0 : m[0x28 / 4] + MHI_BHI_IMGTXDB;   // sheng-wifi=3 sets everything up but leaves the doorbell alone
    st[14] = 1;   // about to write the boot loader's address and size into BHI
    uint32_t phase = mhi_boot_begin(&boot, 0, sbl.pa, kSbl < fw.amssSize ? kSbl : fw.amssSize, table.pa, entries * (uint32_t)sizeof(MhiBhieEntry), 0x5eed);
    gTraceWrites = 0;
    // the doorbell is written: stay off the endpoint for a while, each marker telling the log how far the boot got without touching it
    for (uint32_t quiet = 0; quiet < 3; ++quiet) {st[14] = 0x200 + quiet; IOSleep(2500);}
    st[14] = 0x203;
    st[14] = 2; st[0] = 5; native_wifi_mhi_ms[4] = elapsed();
    // one step per millisecond; every phase gets two seconds
    uint32_t last = phase, since = 0;
    for (unsigned i = 0; i < 20000 && phase != MhiDone && phase != MhiFailed; ++i) {   // five milliseconds a step, twenty seconds a phase (the driver for this chip waits as long)
        if (phase == MhiNeedContexts) {programContexts(m, ctx, barBytes); /* the boot loader resets the MHI registers when it starts: the contexts go in again once the device reports READY */ st[0] = 6; native_wifi_mhi_ms[5] = elapsed(); phase = mhi_boot_contexts_done(&boot);}
        else {
            if (i % 100 == 0 && (m[0x48 / 4] == 0xffffffffu)) {   // not answering: look at the link, put a cleared BAR back
                if (!native_wifi_cfg[16]) {linkSnapshot(native_wifi_cfg); native_wifi_cfg[16] = i + 1;}
                linkRecover();
                native_wifi_cfg[17] = gRestored;
            }
            phase = mhi_boot_step(&boot);
        }
        st[5] = boot.phase | boot.error << 8 | boot.failed_phase << 16; st[6] = boot.ee; st[7] = boot.state; st[14] = 100 + i;   // live, for a log slot written just before a reset
        if (phase != MhiNeedContexts && phase != MhiDone && phase != MhiFailed) native_wifi_chip[13] += eventAck(m);
        if (phase == MhiImageLoading && since == 600) eventSnapshot(m, 0);
        if (phase != last) {last = phase; since = 0;}
        else if (++since > 4000) {phase = mhi_boot_timeout(&boot); last = phase; since = 0;}
        IOSleep(5);
    }
    if (phase == MhiDone) st[0] = 7;
    eventSnapshot(m, 1);
    IOSleep(3000);
    linkSnapshot(native_wifi_cfg + 8);
    {uint32_t sum = 0; for (uint32_t off = 0; off < kSbl; off += 4) {uint32_t w; memcpy(&w, seg[0].va + off, 4); sum += w;} native_wifi_cfg[30] = sum;}
    if (&native_ufs_extra_fault) native_ufs_extra_fault(const_cast<uint32_t *>(native_wifi_cfg) + 18);
    st[5] = boot.phase | boot.error << 8 | boot.failed_phase << 16;
    st[6] = mhi_boot_ee(&boot); st[7] = mhi_boot_state(&boot);
    const uint32_t bhi = m[0x28 / 4], bhie = m[0x2c / 4];
    st[8] = m[(bhi + 0x2c) / 4]; st[9] = m[(bhi + 0x30) / 4];
    st[10] = m[(bhi + 0x34) / 4]; st[11] = m[(bhi + 0x38) / 4]; st[12] = m[(bhi + 0x3c) / 4];
    st[13] = m[0x48 / 4]; st[14] = elapsed(); st[15] = m[(bhie + 0x44) / 4]; native_wifi_mhi_ms[6] = st[14];
}

// Stage 4: forward the window to bus 1 (the root port's memory base and limit, outbound iATU region 1), give the endpoint's BAR0 an address in it, turn
// memory decoding and bus mastering on and read the MHI registers the BAR starts with. The endpoint's configuration space is reached through
// the 1 MiB window iATU region 0 maps to bus 1 (c), the root port's through the DBI (d), the iATU through a (unrolled, 0x200 per region).
void endpointProbe(volatile uint32_t *a, volatile uint32_t *c, volatile uint32_t *d) {
    gCfg = c; gDbi = d;
    volatile uint32_t *ep = native_wifi_ep;
    // region 1: memory, CPU window to the same PCI addresses
    volatile uint32_t *r1 = a + 0x200 / 4;
    r1[0x08 / 4] = uint32_t(kMemBase);
    r1[0x0c / 4] = uint32_t(kMemBase >> 32);
    r1[0x10 / 4] = uint32_t(kMemBase + kMemSize - 1);
    r1[0x20 / 4] = uint32_t((kMemBase + kMemSize - 1) >> 32);
    r1[0x14 / 4] = uint32_t(kMemBase);
    r1[0x18 / 4] = uint32_t(kMemBase >> 32);
    r1[0x00 / 4] = 0x0;
    r1[0x04 / 4] = 1u << 31;
    for (unsigned i = 0; i < 5 && !(r1[0x04 / 4] & (1u << 31)); ++i) IODelay(10);
    // type 1 header memory base (bits 15:4 = address bits 31:20) and limit (31:20)
    d[0x20 / 4] = uint32_t(((kMemBase + kMemSize - 1) & 0xfff00000u) | ((kMemBase >> 16) & 0xfff0u));
    ep[4] = d[0x20 / 4];
    ep[0] = 1;
    // BAR0: size it, put it at the start of the window
    const uint32_t saved = c[0x10 / 4];
    c[0x10 / 4] = 0xffffffffu;
    const uint32_t mask = c[0x10 / 4];
    c[0x10 / 4] = saved;
    ep[1] = mask;
    const uint32_t size = ~(mask & ~0xfu) + 1;
    if (!(mask & ~0xfu) || !size || size > kMemSize) return;
    // a BAR sits at an address aligned to its size: the first such address of the window
    const uint32_t address = uint32_t((kMemBase + size - 1) & ~uint64_t(size - 1));
    ep[1] = mask;
    c[0x10 / 4] = address | (mask & 0xfu);
    if ((mask & 6u) == 4u) c[0x14 / 4] = 0;
    c[0x04 / 4] = (c[0x04 / 4] & 0xffffu) | 0x6u;
    gBar0 = address | (mask & 0xfu);
    ep[2] = c[0x10 / 4];
    ep[3] = c[0x04 / 4];
    ep[0] = 2;
    Window bar = {};
    if (!mapWindow(bar, address, size < 0x200000 ? size : 0x200000)) return;
    volatile uint32_t *m = bar.va;
    for (unsigned i = 0; i < 7; ++i) {
        static const uint32_t regs[7] = {0x00, 0x08, 0x10, 0x28, 0x2c, 0x38, 0x48};
        ep[5 + i] = m[regs[i] / 4];
    }
    ep[0] = 3;
    uint32_t level5 = 0;
    PE_parse_boot_argn("sheng-wifi", &level5, sizeof level5);
    if (level5 >= 5) {ep[0] = 4; wifiStack(m, size < 0x200000 ? size : 0x200000); release(bar); return;}
    const uint32_t bhi = m[0x28 / 4];
    if (bhi && bhi < 0x20000 - 0x40) {
        ep[12] = m[bhi / 4 + 1] | m[bhi / 4] << 16;
        ep[13] = m[(bhi + 0x28) / 4];
        ep[14] = m[(bhi + 0x2c) / 4];
        ep[15] = m[(bhi + 0x30) / 4];
        ep[0] = 4;
        mhiBring(m, size < 0x20000 ? size : 0x20000);
    }
    release(bar);
}

// Stage 2; the step reached stays in native_wifi_stats[0], a negative value names the step that failed.
void bringUp(Window &gcc, Window &tlmm) {
    volatile uint32_t *g = gcc.va, *t = tlmm.va;
    pmicPrepare();
    const int rails = powerRails();
    IOSleep(20);
    if (gPmic.obs.va) {pmicSurvey(true); pmicFind1800(gPmic, native_wifi_ldo + 12);}
    if (rails) {native_wifi_stats[0] = -9; return;}
    IOSleep(5);
    // PERST# held low, CLKREQ# to its PCIe function, then WLAN_EN high and the WCN7850 power-up delay (pwrseq-qcom-wcn.c: 50 ms)
    setPin(t, kPerst, 1u | 1u << 9, 0);
    setPin(t, kClkReq, 3u | 1u << 2, -1);
    // the chip may still run what an earlier start left: WLAN_EN goes low first so that it starts from nothing
    setPin(t, kWlanEn, 1u | 3u << 6 | 1u << 9, 0);
    IOSleep(150);
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
    if (!mapWindow(parf, kParf, 0x3000) || !mapWindow(dbi, kDbi, 0x1000)) {release(parf); native_wifi_stats[0] = -13; return;}
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

    // qcom_pcie_config_sid_1_9_0: the controller tells the SMMU which stream a requester's DMA belongs to from a hash table of requester IDs
    // (0x2000, 256 words: BDF << 16 | stream offset << 8 | next) once its bypass bit (0x2c00 bit 0) is off. The iommu-map is requester 0 -> stream 0x1400 and
    // requester 0x100 (bus 1, the endpoint) -> 0x1401; the hash is a CRC-8 (polynomial 7, MSB first) of the requester ID's two bytes, high one first.
    {
        volatile uint32_t *f = parf.va;
        for (unsigned i = 0; i < 256; ++i) f[(0x2000 + 4 * i) / 4] = 0;
        f[0x2c00 / 4] &= ~1u;
        auto crc8 = [](uint16_t bdf) {
            uint8_t crc = 0;
            const uint8_t bytes[2] = {(uint8_t)(bdf >> 8), (uint8_t)bdf};
            for (uint8_t byte : bytes) {
                crc ^= byte;
                for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 7) : (uint8_t)(crc << 1);
            }
            return crc;
        };
        const uint8_t hash = crc8(0x100);
        f[(0x2000 + 4 * hash) / 4] = 0x100u << 16 | 1u << 8;   // requester 0 hashes to entry 0 with offset 0: the zeroed word already says that
        __asm__ volatile("dsb sy" ::: "memory");
        native_wifi_pon[10] = hash | f[0x2c00 / 4] << 8 | (f[(0x2000 + 4 * hash) / 4] != 0) << 31;   // hash, bypass register, entry written
    }

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
        if ((config.va[0] & 0xffff) == 0x17cb) endpointProbe(a, config.va, d);
    } else native_wifi_stats[0] = -17;
    release(config); release(atu); release(dbi); release(parf);
}

void wifiThread(void *arg, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng-wifi");
    publishMac();
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

// A file of the firmware container by name, for the board's own test hooks (not the driver).
extern "C" int sheng_wifi_file(const char *name, const uint8_t **data, size_t *size) {return pFwGet(name, data, size);}

extern "C" void sheng_wifi_probe(void) {
    unsigned stage = 0;
    if (!PE_parse_boot_argn("sheng-wifi", &stage, sizeof stage) || stage < 1 || native_wifi_stats[0]) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiThread, reinterpret_cast<void *>(uintptr_t(stage)), &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_wifi_stats[0] = -7;
}
