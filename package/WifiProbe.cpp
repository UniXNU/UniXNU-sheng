// SPDX-License-Identifier: BSD-2-Clause-Patent
// WiFi bring-up on the sheng board, stage 1 (boot argument sheng-wifi=1): a read-only look at what the boot loaders left of the WCN7850
// path, so the next stage knows what it has to power and train itself.
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

// [0] stage (1 mapped, 2 clocks read, 3 controller read; negative: the step that failed), [1] vote 0x52008, [2] vote 0x52000,
// [3] GDSC, [4] halt bits (bit i set: branch i of kBranches is off), [5] GPIO 80 | 82 << 8 | 94 << 16 (bit 0 input level, bit 1 output),
// [6] PARF LTSSM, [7] root port vendor/device, [8] link control/status, [9] PARF PHY control
extern "C" {volatile uint32_t native_wifi_stats[10];}

namespace {
constexpr uint64_t kGcc = 0x100000, kTlmm = 0xf100000, kParf = 0x1c00000, kDbi = 0x60000000;
constexpr uint32_t kVote8 = 0x52008, kVote0 = 0x52000, kGdsc = 0x6b004;
// slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a, cnoc_sf, aggre_noc, ddrss_sf, phy_rchng
constexpr uint32_t kBranches[] = {0x6b020, 0x6b02c, 0x6b038, 0x6b03c, 0x6b048, 0x6b01c, 0x10030, 0x1003c, 0x1004c, 0x6b054};
constexpr uint32_t kNeeded = 0xf;   // slv_axi, mstr_axi, cfg_ahb, aux
constexpr unsigned kGpios[] = {80, 82, 94};
constexpr uint32_t kParfLtssm = 0x1b0, kParfPhyCtrl = 0x40, kLinkControl = 0x80;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void release(Window &w) {if (w.map) w.map->release(); w = {};}

void wifiThread(void *, wait_result_t) {
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
    uint32_t gpios = 0;
    for (unsigned i = 0; i < 3; ++i) gpios |= (tlmm.va[(kGpios[i] * 0x1000 + 4) / 4] & 3u) << (8 * i);
    native_wifi_stats[5] = gpios;
    native_wifi_stats[0] = 2;
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
    if (kernel_thread_start(wifiThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_wifi_stats[0] = -7;
}
