// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off, staged read-only observation of the Adreno power and clock state. `sheng-gpu-power=<n>`:
//   1  read GCC GPU branches, GPUCC gdscr/branches, GPU SMMU global space 0 and RBBM_STATUS, each only when the previous stage
//      showed its prerequisite on (GpuPowerBoard.h); no register is written
//   2  as 1, plus the one write Linux's gcc-sm8550 probe makes: enable gcc_gpu_cfg_ahb_clk if its enable bit is clear
//   3  as 2, plus, if GPUCC shows the CX power domain off: the gdsc.c enable sequence for gpu_cc_cx_gdsc and the gpu_cc_ahb_clk and
//      gpu_cc_hlos1_vote_gpu_smmu_clk enables, so the SMMU global space can be read; GX and the SMMU are never programmed
//   4  as 3, then, if the SMMU global space read cleanly with CX on and the vote clock running and no SMR valid: map the
//      whole adreno SMMU window and build the GMU's own context through NativeAdreno (NativeAdrenoGmuContextBringup):
//      fixed a6xx_gmu.c payload addresses, native4K TTBR0 tables, context bank 1 / ASID 1, SMR+S2CR slot 2 for the GMU
//      stream 5 (iommus of gmu@3d6a000 at the pinned tree). Bank, ASID and slot are this probe's own choice: Linux gives
//      bank 0 to the GPU and takes the first free SMR. The GMU itself is not booted, so no transaction uses the context.
//      Rows 5 and 7 then show that result instead of the clock words and SMMU IDs mode 3 already recorded (int-005).
//   5  as 4, then, only if the GMU context reached Ready, read-only GMU cold-start prerequisites: the GMU clock tree
//      (gpb_read_gmu_clocks) in row 6; the AOP command DB (reserved-memory aop-cmd-db-region@81c60000, also reserved in
//      the sheng UEFI memory map, so outside the DarwinBoot window) copied with aligned reads, parsed by
//      ShengGmuCommandDbInputs and turned into the HFI vote table by ShengGmuBuildStartup (ACD and IFPC off), plus the
//      GMU reset/retention/RSC/HFI words when gpu_cc_cx_gmu_clk already runs, in row 7. Nothing is written. The version
//      passed to the parser is the word just read, so a pass shows the layout, not a qualified version.
//   6  as 5, then, between the clock snapshot and the GMU state read, the GMU clock bring-up of gpb_gmu_clocks_on (GPLL0
//      GPU votes, gpucc PLL0/PLL1 configuration, PLL1 enable, hub 200 MHz / gmu 220 MHz, the GMU branches). Row 5 shows its
//      result, row 6 the clock tree after it. The GMU itself stays in reset; no GMU or GPU register is written.
//   7  as 6, then, only if the clocks came up (s7 f0), the command DB gave a startup table (p0 s0) and the GMU reads
//      in reset and idle (1/0/0/0): the GMU cold start. a6xx_gmu_resume's interrupt clear/mask, a write-combine CPU view
//      of the five context payloads (the GMU maps them Normal non-cacheable), then ShengGmuColdStart with the embedded
//      gmu_gen70200.bin (hash-checked in full before any write) and the command-DB startup table: RSC/PDC setup,
//      firmware load and readback, CM3 release, FW and HFI ready, then the HFI startup messages with their ACKs. Row 6
//      shows the HFI protocol result, row 7 the load result, row 4 the GMU state before and after.
//   8  as 7, then, only after a complete cold start, the first GPU steps of a6xx_gmu_resume / hw_init: GMU2HOST interrupt
//      clear and mask, the HFI GX_BW_PERF_VOTE (a6xx_hfi_set_freq; perf index 1 = 220 MHz and bandwidth index 1, the
//      lowest real levels of the command-DB tables, where Linux starts at the top one) and the OOB GPU_SET request
//      (set bit 30, ack bit 31) that makes the GMU power GX. Row 5 shows the GX state before/after, and RBBM_STATUS /
//      GBIF_HALT only when GX reports on. Nothing in the GPU register space is written.
//   9  as 8, then, only when GX reports on, the rest of a6xx hw_init up to a7xx_cp_init, in Linux order: the GPU's own
//      native-4K SMMU context (bank 0, SIDs 0/1 on SMR 0/1, as arm-smmu-qcom gives the GPU bank 0) with the SQE, ring,
//      memptrs, power-up list and scratch buffers; the SQE copy (a740_sqe.fw minus its first dword); GBIF halt release,
//      SECVID TSB, hwcg (a740_hwcg), QoS, UCHE, AHB, hang detect, CP protect (a730_protect), APRIV, interrupt mask; SQE
//      base, ring, rptr shadows, SQE start; the power-up list; CP_THREAD_CONTROL + CP_ME_INIT with a timestamp fence;
//      then one CP_MEM_WRITE to the scratch page with a second fence. UBWC and the zap shader / secure-mode exit are
//      not done yet (sheng uses a zap shader, so RBBM_SECVID_TRUST_CNTL is never written). Row 6 shows the GPU
//      context, row 7 the CP result. From mode 8 on, row 4 becomes a heartbeat every two seconds once the run ends.
//      hw_init also programs UBWC (a6xx_set_ubwc_config with the sm8550 qcom_ubwc_cfg_data). After the scratch write:
//      the zap shader (zap_shader_load_mdt / qcom_mdt_load: PAS 13 init_image with the ELF header + hash segment,
//      mem_setup over gpu_micro_code_mem 0x9b09a000, segment copy, auth_and_reset, all as SIP SMC calls) and, only if TZ
//      accepted it, CP_SET_SECURE_MODE 0 with a fence (row 2); then the GPU interrupt line SPI 300 in the GIC
//      distributor before and after RBBM_INT_CLEAR_CMD, and the CP always-on counter rate (row 3).
// The result is drawn in diagnostic extension rows 4-7 (the GPU slots) so one photo carries the whole chain. It runs in a thread
// about ten seconds after the desktop is up so the page already exists if a read faults. Nothing here is validated on the device
// until a boot has shown it.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "GpuPowerBoard.h"
}

#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoVm.h"
#include "NativeAdrenoUserClient.h"
#include <IOKit/IOUserClient.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoPwrup.h"
#include "NativeAdrenoA740HwInit.h"
#include "AdrenoPM4.h"
// Weak: an image that does not link NativeAdreno reports "nolink" instead of failing to link.
extern "C" IOReturn NativeAdrenoGmuContextBringup(volatile uint8_t *smmu, size_t bytes, unsigned bank, unsigned asid,
    unsigned slot, unsigned sid, void (*progress)(uint32_t), NativeAdrenoGmuContextReport *report) __attribute__((weak));
#include "ShengGmuCommandDb.h"
#include "ShengGmu.h"
#include <libkern/crypto/sha2.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
extern "C" ShengGmuStatus ShengGmuColdStart(ShengGmuDevice *, const ShengGmuConfig *, const AdrenoHfiStartupConfig *,
    const ShengGmuStartupLimits *, ShengGmuStartupResult *) __attribute__((weak));
extern "C" ShengGmuStatus ShengGmuBindMmio(ShengGmuMmio *, ShengGmuIo *) __attribute__((weak));
extern "C" IOReturn NativeAdrenoGmuContextPayloads(NativeAdrenoGmuPayload out[5]) __attribute__((weak));
extern "C" IOReturn NativeAdrenoGpuContextBringup(volatile uint8_t *, size_t, unsigned, unsigned, unsigned, unsigned, unsigned,
    unsigned, void (*)(uint32_t), NativeAdrenoGmuContextReport *) __attribute__((weak));
extern "C" IOReturn NativeAdrenoGpuContextBuffers(NativeAdrenoGpuBuffer out[5]) __attribute__((weak));
extern "C" NativeAdrenoVm *NativeAdrenoGpuContextVm() __attribute__((weak));
extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
    const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t) __attribute__((weak));
extern "C" uint32_t native_adreno_publish_stats(uint32_t *) __attribute__((weak));
// The staged AGXAccelerator (drivers/Adreno740/NativeAdrenoAccelerator.cpp): its static initializer runs only after the
// original IOGPUFamily kext has loaded, because the parent metaclass does not exist before that.
extern "C" void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp") __attribute__((weak));
extern "C" IOService *native_adreno_accelerator_allocate(void) __attribute__((weak));
extern "C" unsigned native_adreno_accelerator_instance_size(void) __attribute__((weak));
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" volatile int native_gpu_addon_stage, native_gpu_addon_status, native_gpu_addon2_stage, native_gpu_addon2_status;
struct NativeAdrenoMetalDiagView {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];
    char reader[3][17];
    char name_reader[17];
    char starter[3][17];
    char report[64];
    char report2[64];
};
extern "C" NativeAdrenoMetalDiagView native_adreno_metal_diag;
struct NativeAdrenoOpenRecordView {char name[20]; uint32_t type, macf, res, attach, foreign;};
extern "C" NativeAdrenoOpenRecordView native_adreno_open_log[8];
extern "C" volatile uint32_t native_adreno_trace_stage;
extern "C" volatile uint32_t native_adreno_open_log_count;
extern "C" int proc_iterate(unsigned flags, int (*callout)(void *, void *), void *arg, int (*filter)(void *, void *), void *filterarg);
extern "C" const char *proc_name_address(void *p);
extern "C" volatile uint64_t armcpu_ipi_interrupts __attribute__((weak));
extern "C" volatile uint64_t armcpu_ipi_sent __attribute__((weak));
extern "C" volatile uint64_t armcpu_ipi_selftest_sent __attribute__((weak));
extern "C" void armcpu_ipi_selftest(uint64_t aff) __attribute__((weak));
extern "C" void armcpu_gic_diag(uint64_t aff, uint32_t *out) __attribute__((weak));
extern "C" volatile uint32_t armcpu_gic_diag_stage __attribute__((weak));
extern "C" void armcpu_sgi_selfprobe(uint64_t *out) __attribute__((weak));
extern "C" volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8], armcpu_gic_boot[8];
extern "C" IOService *native_adreno_published_service() __attribute__((weak));
extern "C" volatile uint32_t native_adreno_wc_enabled;
extern "C" volatile uint32_t native_adreno_pool_mib;
extern "C" volatile uint64_t native_adreno_fence_phys;
extern "C" const uint8_t native_adreno_sqe_firmware_image[] __attribute__((weak));
extern "C" const uint64_t native_adreno_sqe_firmware_image_size __attribute__((weak));
extern "C" const uint8_t native_adreno_zap_image[] __attribute__((weak));
extern "C" const uint64_t native_adreno_zap_image_size __attribute__((weak));
extern "C" const uint8_t native_adreno_firmware_image[] __attribute__((weak));
extern "C" const uint64_t native_adreno_firmware_image_size __attribute__((weak));
extern "C" ShengGmuDbStatus ShengGmuCommandDbInputs(const uint8_t *, size_t, uint32_t, ShengGmuVoteInputs *) __attribute__((weak));
extern "C" AdrenoHfiStartupStatus ShengGmuBuildStartup(const ShengGmuVoteInputs *, AdrenoHfiStartupConfig *) __attribute__((weak));
extern "C" unsigned ShengGmuPerfVote(unsigned, unsigned *) __attribute__((weak));

extern "C" {
extern char boot_progress_ext[8][48];
extern char boot_progress_ext_wide[2][112];
struct gpb_result native_gpu_power_result;
struct NativeAdrenoGmuContextReport native_gpu_gmu_context;
volatile uint32_t native_gpu_power_stage;
}

namespace {
unsigned mode() {
    static unsigned cached = ~0u;
    if (cached == ~0u) {unsigned m = 0; PE_parse_boot_argn("sheng-gpu-power", &m, sizeof m); cached = m > 9 ? 9 : m;}
    return cached;
}
void delayUs(void *, unsigned us) {IODelay(us);}
constexpr uint64_t kPhys[5] = {GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE};
constexpr uint64_t kLen[5] = {0x1000, 0x1000, GPB_GPUCC_SIZE, GPB_SMMU_GR0_SIZE, GPB_GPU_WINDOW};
OSSharedPtr<IOMemoryMap> gMaps[5];
uintptr_t gVa[5];

int rd(void *, unsigned w, uint32_t off, uint32_t *out) {
    if (w > 4 || !gVa[w] || off >= kLen[w] || (off & 3)) return -1;
    *out = *reinterpret_cast<volatile uint32_t *>(gVa[w] + off);
    return 0;
}
bool clockWrite(unsigned w, uint32_t off) {
    // Mode 6 only: the registers gpb_gmu_clocks_on names (GCC GPU votes/branches, gpucc PLL0/PLL1, the two RCGs, branches).
    if (w == GPB_WIN_GCC_VOTE) return off == GPB_GCC_PLL_VOTE;
    if (w == GPB_WIN_GCC_GPU) return off == GPB_GCC_DDRSS_GPU_AXI || off == GPB_GCC_GPU_MEMNOC_GFX;
    if (w != GPB_WIN_GPUCC) return false;
    return off < GPB_CC_PLL1 + 0x40 || off == GPB_CC_GMU_RCGR || off == GPB_CC_GMU_RCGR + 4 || off == GPB_CC_HUB_RCGR ||
           off == GPB_CC_HUB_RCGR + 4 || off == GPB_CC_CXO_AON_CLK || off == GPB_CC_DEMET_CLK || off == GPB_CC_CX_GMU_CLK ||
           off == GPB_CC_CXO_CLK || off == GPB_CC_HUB_CX_INT_CLK;
}
int wr(void *, unsigned w, uint32_t off, uint32_t value) {
    // Writes: the GCC cfg AHB enable (mode 2), the GPUCC CX gdsc / clock enables (mode 3), the GMU clocks (mode 6).
    const bool gcc = w == GPB_WIN_GCC_GPU && off == GPB_GCC_GPU_CFG_AHB;
    const bool cc = w == GPB_WIN_GPUCC && (off == GPB_CC_CX_GDSCR || off == GPB_CC_AHB_CLK || off == GPB_CC_SMMU_VOTE_CLK);
    if (w > 4 || !gVa[w] || off >= kLen[w] || (off & 3)) return -1;
    if (!((gcc && mode() >= 2) || (cc && mode() >= 3) || (mode() >= 6 && clockWrite(w, off)))) return -1;
    *reinterpret_cast<volatile uint32_t *>(gVa[w] + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
void setRow(unsigned slot, const char *text) {
    unsigned i = 0;
    for (; i < 47 && text[i]; ++i) boot_progress_ext[slot][i] = text[i];
    for (; i < 47; ++i) boot_progress_ext[slot][i] = ' ';
    boot_progress_ext[slot][47] = 0;
}
void publish(const gpb_result &r) {
    char row[64];
    snprintf(row, sizeof row, "GPU m%u gcc=%s cc=%s sm=%s gp=%s io=%d pw%u%u", r.mode, gpb_state_text(r.gcc), gpb_state_text(r.cc),
             gpb_state_text(r.smmu), gpb_state_text(r.gpu), r.io_error, r.powered_cx, r.powered_clocks);
    setRow(4, row);
    snprintf(row, sizeof row, "ahb=%08x>%08x mem=%08x dvm=%08x", r.cfg_ahb, r.cfg_ahb_after, r.memnoc, r.dvm);
    setRow(5, row);
    snprintf(row, sizeof row, "cx=%08x>%08x gx=%08x sv=%08x", r.cx_before, r.cx_gdscr, r.gx_gdscr, r.cc_smmu);
    setRow(6, row);
    snprintf(row, sizeof row, "SM %08x %08x %08x v%u R%08x", r.scr0, r.id0, r.id1, r.smr_valid, r.rbbm_status);
    setRow(7, row);
}
constexpr unsigned kGmuBank = 1, kGmuAsid = 1, kGmuSlot = 2, kGmuSid = 5;
constexpr uint64_t kSmmuWindow = 0x40000;   // adreno_smmu reg size at the pinned tree; ID1 says 32 x 4 KiB pages x 2
OSSharedPtr<IOMemoryMap> gSmmuMap;
void gmuProgress(uint32_t step) {
    // Before each step, so a hang or abort leaves the step on screen.
    char row[64];
    snprintf(row, sizeof row, "GMUCTX step %u running", step);
    setRow(5, row);
    native_gpu_power_stage = 0x40 + step;
}
void gmuContext(const gpb_result &r) {
    char row[64];
    const bool ready = r.smmu == GPB_DONE && !r.io_error && gpb_gdsc_on(r.cx_gdscr) && !r.smr_valid;
    if (!ready || !NativeAdrenoGmuContextBringup) {
        snprintf(row, sizeof row, "GMUCTX %s", !NativeAdrenoGmuContextBringup ? "nolink" : "skip prereq");
        setRow(5, row);
        return;
    }
    IODeviceMemory *memory = IODeviceMemory::withRange(GPB_SMMU_BASE, kSmmuWindow);
    if (memory) {
        gSmmuMap = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        memory->release();
    }
    if (!gSmmuMap) {setRow(5, "GMUCTX nomap"); return;}
    NativeAdrenoGmuContextReport &g = native_gpu_gmu_context;
    const IOReturn status = NativeAdrenoGmuContextBringup(reinterpret_cast<volatile uint8_t *>(gSmmuMap->getVirtualAddress()),
        kSmmuWindow, kGmuBank, kGmuAsid, kGmuSlot, kGmuSid, gmuProgress, &g);
    // B s<step> st<IOReturn> k<kit program status>/<kit attach status> i<ID2> p<phase>w<writes>r<readback>e<enabled>
    //   r<route phase>w<writes>v<verified>; spans are always 5 on success and left out for room
    snprintf(row, sizeof row, "B s%u st%x k%u/%u i%x p%uw%ur%ue%u r%uw%uv%u", g.step, static_cast<unsigned>(status),
             g.program_status, g.route_status, g.id2, g.program_phase, g.program_writes, g.program_readback_valid,
             g.program_enabled, g.route_phase, g.route_writes, g.routes_verified);
    setRow(5, row);
    if (g.step == 3 && g.program_phase >= 3) {
        // Program refused after its readback (int-007: kit status 7 with TTBR0 stored): show which context fields
        // differ (mask, NativeAdrenoGmuContextReport) and the read-back values; TTBR0 is in GPU4 of int-007.
        const ArmSmmu4KContextImage &v = g.image;
        snprintf(row, sizeof row, "M%03x a2%x m%x/%x T1%llx", g.mismatch, v.cba2r, v.mair0, v.mair1,
                 static_cast<unsigned long long>(v.ttbr1));
        setRow(6, row);
        snprintf(row, sizeof row, "cb%08x tc%08x t2%08x s%x", v.cbar, v.tcr, v.tcr2, v.sctlr);
        setRow(7, row);
    } else {
        snprintf(row, sizeof row, "T%016llx S%08x C%08x R%08x", static_cast<unsigned long long>(g.ttbr0), g.sctlr,
                 g.s2cr, g.smr);
        setRow(7, row);
    }
    native_gpu_power_stage = status ? 0x4f : 0x47;
}

// Mode 5 (read-only). Physical blocks from the pinned DT: gmu reg 0x3d6a000 size 0x35000, rscc 0x3d50000, cx_misc
// 0x3d9e000 (gpu reg-names "cx_mem"); register dword offsets from a6xx_gmu.xml / a6xx.xml. The gmu block starts at dword
// 0x1a800 of the GPU register space (SHENG_GMU_CORE_GPU_DELTA 0x6a000 bytes), so CM3_SYSRESET 0x1f800 is byte 0x14000.
constexpr uint64_t kCmdDbBase = 0x81c60000, kCmdDbBytes = 0x20000;
constexpr uint64_t kGmuPhys[3] = {0x03d6a000, 0x03d50000, 0x03d9e000};
constexpr uint64_t kGmuLen[3] = {0x35000, 0x10000, 0x1000};
constexpr uint32_t kGmuSysReset = 0x1f800 * 4 - 0x6a000, kGmuHfiStatus = 0x1f980 * 4 - 0x6a000;
constexpr uint32_t kRsccSeqBusy = 0x101 * 4, kCxMiscTcmRet = 0x39 * 4;
alignas(8) uint8_t gCmdDb[kCmdDbBytes];
ShengGmuVoteInputs gVoteInputs;
AdrenoHfiStartupConfig gStartup;
OSSharedPtr<IOMemoryMap> gCmdDbMap, gGmuMaps[3];

extern "C" {
struct gpb_gmu_clocks native_gpu_gmu_clocks;
struct gpb_gmu_clock_on native_gpu_gmu_clock_on;
uint32_t native_gpu_gmu_state[4];   // SYSRESET, TCM_RET_CNTL, RSCC SEQ_BUSY_DRV0, HFI_CTRL_STATUS
}

volatile uint32_t *mapWords(uint64_t phys, uint64_t len, OSSharedPtr<IOMemoryMap> &keep) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, len);
    if (!memory) return nullptr;
    keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly), OSNoRetain);
    memory->release();
    return keep ? reinterpret_cast<volatile uint32_t *>(keep->getVirtualAddress()) : nullptr;
}
// Mode 7. Windows in ShengGmu order (core, rscc, pdc, gpu, cx_misc): the pinned DT gmu reg-names gmu/rscc/gmu_pdc, gpu
// reg 0x3d00000 size 0x40000 and its cx_mem 0x3d9e000; ShengGmuBindMmio refuses any other address or size.
constexpr uint64_t kColdPhys[5] = {0x03d6a000, 0x03d50000, 0x0b280000, 0x03d00000, 0x03d9e000};
constexpr uint64_t kColdLen[5] = {0x35000, 0x10000, 0x10000, 0x40000, 0x1000};
constexpr uint32_t kAoIrqClr = 0x23b04 * 4 - 0x6a000, kAoIrqMask = 0x23b06 * 4 - 0x6a000;   // a6xx_gmu.xml
constexpr uint32_t kAoFatal = (1u << 0) | (1u << 3) | (1u << 5);   // A6XX_GMU_IRQ_MASK: WDOG_BITE, FENCE_ERR, AHB_BUS_ERROR
OSSharedPtr<IOMemoryMap> gColdMaps[5], gPayloadMaps[5];
ShengGmuMmio gColdMmio;
ShengGmuDevice gColdDevice;
ShengGmuConfig gColdConfig;
ShengGmuStartupResult gColdResult;
extern "C" {
ShengGmuStartupResult *native_gpu_gmu_cold = &gColdResult;
}
int coldSha256(void *, const void *data, size_t bytes, uint8_t digest[32]) {
    if (!data || !bytes || !digest) return 1;
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data, bytes);
    SHA256_Final(digest, &ctx);
    return 0;
}
int coldNow(void *, uint64_t *us) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    *us = ns / 1000;
    return 0;
}
int coldWait(void *, unsigned us) {IODelay(us); return 0;}
void readGmuState(uint32_t out[4]) {
    // The same four words gmuPrerequisites reads, through the cold-start windows.
    volatile uint8_t *core = gColdMmio.windows[0].cpu, *rscc = gColdMmio.windows[1].cpu, *misc = gColdMmio.windows[4].cpu;
    out[0] = *reinterpret_cast<volatile uint32_t *>(core + kGmuSysReset);
    out[1] = *reinterpret_cast<volatile uint32_t *>(misc + kCxMiscTcmRet);
    out[2] = *reinterpret_cast<volatile uint32_t *>(rscc + kRsccSeqBusy);
    out[3] = *reinterpret_cast<volatile uint32_t *>(core + kGmuHfiStatus);
}
// Mode 8. GMU dword offsets from a6xx_gmu.xml; GPU dword offsets from a6xx.xml (A6XX-A7XX variants).
constexpr uint32_t kGmuReg(uint32_t dword) {return dword * 4 - 0x6a000;}
constexpr uint32_t kG2hClr = kGmuReg(0x1f991), kG2hInfo = kGmuReg(0x1f992), kG2hMask = kGmuReg(0x1f993);
constexpr uint32_t kH2gSet = kGmuReg(0x1f994), kGxStatus = kGmuReg(0x1f8d0);   // GMU_SPTPRAC_PWR_CLK_STATUS (A7XX)
constexpr uint32_t kRbbmStatus = 0x0210 * 4, kGbifHalt = 0x3c45 * 4;
extern "C" {
uint32_t native_gpu_perf_vote[2];   // OPP and bandwidth index of the initial HFI vote
uint32_t native_gpu_gx[8];   // vote status, fw error, oob ack polls, gx before, gx after, gx gdscr, RBBM_STATUS, GBIF_HALT
}
int hfiWait(void *) {IODelay(100); return 0;}
// Mode 9. GPU register access through the cold-start GPU window (dword offsets), and the GMU core window.
uint32_t gpuRd(uint32_t reg) {
    uint32_t v = 0xdeadbeef;
    gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuGpu, size_t(reg) * 4, &v);
    return v;
}
void gpuWr(uint32_t reg, uint32_t value) {
    gColdDevice.io.write32(gColdDevice.io.cookie, ShengGmuGpu, size_t(reg) * 4, value);
}
void gpuWr64(uint32_t reg, uint64_t value) {gpuWr(reg, uint32_t(value)); gpuWr(reg + 1, uint32_t(value >> 32));}
void gmuWr(uint32_t dword, uint32_t value) {gColdDevice.io.write32(gColdDevice.io.cookie, ShengGmuCore, kGmuReg(dword), value);}
bool gxOn() {
    uint32_t v = 3;
    gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuCore, kGxStatus, &v);
    return !(v & 3);
}
int ringWrite(void *, uint32_t reg, uint32_t value) {
    return gColdDevice.io.write32(gColdDevice.io.cookie, ShengGmuGpu, size_t(reg) * 4, value);
}
int ringRead(void *, uint32_t reg, uint32_t *value) {
    return gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuGpu, size_t(reg) * 4, value);
}
int ringBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
// The ring, memptrs and lists are Normal non-cacheable on both sides (GPU PTE MAIR 0, CPU write-combine): no cache
// maintenance, only ordering.
int ringPublish(void *, const volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int ringAcquire(void *, volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
uint64_t ringNow(void *) {uint64_t us = 0; coldNow(nullptr, &us); return us;}
// The fence waits' pause. Once the GPU line is attached (irqAndTimer) a waiter that may block sleeps until the next GPU interrupt or
// 100 us, whichever is first; before that, or where it may not block, it spins 20 us. int-143's waiters spun through every frame's
// GPU time (35 ms a commit) on CPUs that were already the bottleneck.
volatile bool gIrqAttached;
volatile uint32_t gIrqCount;
extern "C" {volatile uint64_t native_gpu_idle_sleeps, native_gpu_idle_spins;}
extern "C" int get_preemption_level(void);
int ringIdle(void *) {
    if (gIrqAttached && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && get_preemption_level() == 0) {
        assert_wait_timeout(event_t(&gIrqCount), THREAD_UNINT, 100, NSEC_PER_USEC);
        thread_block(THREAD_CONTINUE_NULL);
        native_gpu_idle_sleeps = native_gpu_idle_sleeps + 1;
    } else {
        IODelay(20);
        native_gpu_idle_spins = native_gpu_idle_spins + 1;
    }
    return 0;
}
OSSharedPtr<IOMemoryMap> gGpuMaps[5];
volatile uint8_t *gGpuCpu[5];
NativeAdrenoGpuBuffer gGpuBuffers[5];
NativeAdrenoRing gRing;
NativeAdrenoCpInit gCpInit;
extern "C" {
NativeAdrenoGmuContextReport native_gpu_context;
// stage, copy, cp init status, fence1 result, fence1 seen, mem-write status, fence2 result, scratch word
uint32_t native_gpu_cp[8];
}
void gpuCtxProgress(uint32_t step) {
    char row[64];
    snprintf(row, sizeof row, "Q step %u running", step);
    setRow(6, row);
    native_gpu_power_stage = 0x90 + step;
}
// Context bank 0 ACTLR (MMU-500: bit 0 CMTLB, bit 1 CPRE, bits 9:8 prefetch depth) and sACR. The bootloader leaves ACTLR 0x3 (CPRE on,
// depth 0). Linux clears CPRE in every bank (arm_mmu500_reset, MMU-500 errata 841119/826419: the next-page prefetcher can keep stale
// translations after a TLB invalidation), and the Linux reference session on this tablet read the Adreno bank as 0x301 (CMTLB |
// PREFETCH_DEEP, CPRE off) with Linux's own driver running. The GPU bank is set to that value before any GPU buffer is mapped, then
// its TLB is invalidated (TLBIALL +0x618, TLBSYNC +0x7f0, TLBSTATUS +0x7f4). gActlr keeps the value found, gActlrSet the readback.
uint32_t gActlr = 0xdead, gActlrSet = 0xdead, gSacr = 0xdead;
void gpuSetActlr() {
    const uint32_t id1 = native_gpu_context.id1;
    if (!gSmmuMap || !id1) return;
    const size_t page = (id1 >> 31) ? 0x10000 : 0x1000, bank0 = page << (((id1 >> 28) & 7) + 1);
    if (bank0 + 0x800 > kSmmuWindow) return;
    volatile uint32_t *base = reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress());
    gActlr = base[(bank0 + 0x4) / 4];
    gSacr = base[0x10 / 4];
    base[(bank0 + 0x4) / 4] = 0x301;
    __asm__ volatile("dsb sy" ::: "memory");
    base[(bank0 + 0x618) / 4] = 0;
    base[(bank0 + 0x7f0) / 4] = 0;
    for (unsigned i = 0; i < 1000 && (base[(bank0 + 0x7f4) / 4] & 1); ++i) IODelay(1);
    gActlrSet = base[(bank0 + 0x4) / 4];
}
// Reads one 64-bit word of physical memory through a fresh uncached mapping (page tables and buffer contents as DRAM holds them).
// A throwaway uncached kernel mapping (io_map with VM_WIMG_IO). Not IOMemoryDescriptor::map: its kernel path (IOMapPages) sets the page's
// own cache attribute to the requested mode, which changed every mapping of the page, a client's view of its BO included (int-098).
extern "C" vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern "C" void ml_io_unmap(vm_offset_t addr, vm_size_t size);
bool physRead64(uint64_t pa, uint64_t *out) {
    const vm_offset_t va = ml_io_map_unmappable(vm_offset_t(pa & ~uint64_t(0xfff)), 0x1000, 0x7 /* VM_WIMG_IO */);
    if (!va) return false;
    *out = *reinterpret_cast<volatile uint64_t *>(va + (pa & 0xff8));
    ml_io_unmap(va, 0x1000);
    return true;
}
// NativeAdrenoVm::sampleWord's cached reader: one aligned word through the kernel's cacheable physical aperture, so a line a client's
// cacheable view left dirty is seen whatever the cache mode of the BO's own mappings.
extern "C" vm_offset_t phystokv(uint64_t pa);
extern "C" uint64_t gPhysBase, gPhysSize;
int cachedRead32(uint64_t pa, uint32_t *value) {
    if ((pa & 3) || pa < gPhysBase || pa - gPhysBase >= gPhysSize) return 1;
    const vm_offset_t va = phystokv(pa);
    if (!va) return 1;
    *value = *reinterpret_cast<volatile uint32_t *>(va);
    // Drop the line this read brought in: a stale clean copy would later differ from DRAM and look like a dirty one.
    __asm__ volatile("dsb sy\n\tdc civac, %0\n\tdsb sy" :: "r"(va) : "memory");
    return 0;
}
// NativeAdrenoVm::auditRange's line comparer (int-100): whole lines of one page as the cacheable aperture and DRAM (a fresh uncached
// mapping) read them. A differing line is cleaned and invalidated, then DRAM is read again: holding the cached copy now, the line was
// dirty; unchanged, it was a stale clean copy (now dropped). int-099's single-word check wrote its line back before reading DRAM.
// The time spent and the attribute XNU keeps for the first page that held a dirty line go to the banner (GPUW).
extern "C" unsigned int pmap_cache_attributes(unsigned int pn);
extern "C" {volatile uint64_t native_adreno_audit_ticks; volatile uint32_t native_adreno_audit_attr;}
int lineAudit(uint64_t pa, size_t bytes, uint32_t *dirty, uint32_t *stale) {
    if (!bytes || ((pa | bytes) & 63) || (pa & 0xfff) + bytes > 0x1000 || pa < gPhysBase || pa - gPhysBase >= gPhysSize) return 1;
    const uint64_t began = mach_absolute_time();
    const vm_offset_t page = ml_io_map_unmappable(vm_offset_t(pa & ~uint64_t(0xfff)), 0x1000, 0x7 /* VM_WIMG_IO */);
    const vm_offset_t cached = phystokv(pa);
    if (!page || !cached) {if (page) ml_io_unmap(page, 0x1000); return 1;}
    const vm_offset_t dram = page + vm_offset_t(pa & 0xfff);
    for (size_t at = 0; at < bytes; at += 64) {
        const volatile uint64_t *c = reinterpret_cast<const volatile uint64_t *>(cached + at);
        const volatile uint64_t *d = reinterpret_cast<const volatile uint64_t *>(dram + at);
        uint64_t copy[8];
        bool differs = false;
        for (unsigned i = 0; i < 8; ++i) {copy[i] = c[i]; if (copy[i] != d[i]) differs = true;}
        if (!differs) continue;
        __asm__ volatile("dsb sy\n\tdc civac, %0\n\tdsb sy" :: "r"(cached + at) : "memory");
        bool written = true;
        for (unsigned i = 0; i < 8; ++i) if (d[i] != copy[i]) written = false;
        if (written) {
            if (!*dirty && !native_adreno_audit_attr) native_adreno_audit_attr = 0x100 | (pmap_cache_attributes(unsigned(pa >> 12)) & 0xff);
            ++*dirty;
        } else {
            ++*stale;
        }
    }
    ml_io_unmap(page, 0x1000);
    native_adreno_audit_ticks += mach_absolute_time() - began;
    return 0;
}
// NativeAdrenoVm::sampleWord's DRAM reader (int-070): one aligned word through a fresh uncached mapping.
int dramRead32(uint64_t pa, uint32_t *value) {
    uint64_t w = 0;
    if (!physRead64(pa & ~uint64_t(7), &w)) return 1;
    *value = uint32_t(w >> ((pa & 4) * 8));
    return 0;
}
// Software walk of the GPU context's TTBR0 table (bank 0 TTBR0 +0x20, TCR +0x30: T0SZ bits 5:0, 4 KiB granule) for one GPU address.
// The SMMU here has no address translation operations (IDR0.ATOSNS), so this shows what the table says, not what the TLB holds.
// Returns the leaf descriptor (0 when absent) and the physical address it maps the address to.
[[maybe_unused]] uint64_t gpuWalk(uint64_t va, uint64_t *pa) {
    *pa = 0;
    const uint32_t id1 = native_gpu_context.id1;
    if (!gSmmuMap || !id1) return 0;
    const size_t page = (id1 >> 31) ? 0x10000 : 0x1000, bank0 = page << (((id1 >> 28) & 7) + 1);
    const volatile uint32_t *base = reinterpret_cast<const volatile uint32_t *>(gSmmuMap->getVirtualAddress());
    const uint64_t ttbr = (uint64_t(base[(bank0 + 0x24) / 4]) << 32) | base[(bank0 + 0x20) / 4];
    const unsigned bits = 64 - (base[(bank0 + 0x30) / 4] & 0x3f);
    if (bits < 25 || bits > 48) return 0;
    int level = 4 - int((bits - 12 + 8) / 9);
    uint64_t table = ttbr & 0x0000fffffffff000ull, desc = 0;
    for (; level <= 3; ++level) {
        const unsigned shift = 12 + 9 * unsigned(3 - level);
        if (!physRead64(table + ((va >> shift) & 0x1ff) * 8, &desc) || !(desc & 1)) return 0;
        if (level == 3 || !(desc & 2)) {*pa = (desc & 0x0000fffffffff000ull & ~((uint64_t(1) << shift) - 1)) | (va & ((uint64_t(1) << shift) - 1)); return desc;}
        table = desc & 0x0000fffffffff000ull;
    }
    return 0;
}
uint32_t gpuFsr() {
    // Context bank 0 FSR (ARM SMMUv2: banks follow 2^(NUMPAGENDXB+1) global pages; FSR at +0x58).
    const uint32_t id1 = native_gpu_context.id1;
    if (!gSmmuMap || !id1) return 0xdead;
    const size_t page = (id1 >> 31) ? 0x10000 : 0x1000, bank0 = page << (((id1 >> 28) & 7) + 1);
    if (bank0 + 0x60 > kSmmuWindow) return 0xdead;
    return *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + bank0 + 0x58);
}
void cpRow(const char *what) {
    // C<stage><what> R<RBBM_STATUS> I<INT_0_STATUS> F<CP_HW_FAULT> r<RPTR>/<WPTR> m<shadow rptr>/<fence> s<CB0 FSR>
    char row[64];
    const volatile uint32_t *mem = reinterpret_cast<const volatile uint32_t *>(gGpuCpu[2]);
    snprintf(row, sizeof row, "C%u%s R%x I%x F%x r%x/%x m%x/%x s%x", native_gpu_cp[0], what, gpuRd(A740_RBBM_STATUS),
             gpuRd(A740_RBBM_INT_0_STATUS), gpuRd(A740_CP_HW_FAULT), gpuRd(A740_CP_RB_RPTR), gpuRd(A740_CP_RB_WPTR),
             mem ? mem[0] : 0, mem ? mem[1] : 0, gpuFsr());
    setRow(7, row);
}
// SIP SMC (qcom_scm-smc.c): x0 function, x1 arginfo, x2.. arguments. QCOM_SCM_INTERRUPTED (1) resumes with x0 = 1 and
// the returned x6 (ARM_SMCCC_QUIRK_QCOM_A6); QCOM_SCM_V2_EBUSY (-12) retries after 30 ms, at most 20 times. WAITQ_SLEEP
// (2) needs the SCM wait-queue interrupt, which is not wired here, so it is returned to the caller as is.
struct ScmRes {int64_t a0; uint64_t a1; uint32_t ms, resumes;};
ScmRes scmCall(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2) {
    uint64_t first = fn, a6 = 0;
    const uint64_t start = ringNow(nullptr);
    auto done = [&](int64_t a0, uint64_t a1, unsigned resumes) {
        return ScmRes{a0, a1, uint32_t((ringNow(nullptr) - start) / 1000), resumes};
    };
    for (unsigned resume = 0, busy = 0; resume < 10000;) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = arginfo, x2 asm("x2") = p0, x3 asm("x3") = p1;
        register uint64_t x4 asm("x4") = p2, x5 asm("x5") = 0, x6 asm("x6") = a6, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {first = 1; a6 = x6; ++resume; continue;}
        if (int64_t(x0) == -12 && busy++ < 20) {first = fn; a6 = 0; IOSleep(30); continue;}
        return done(int64_t(x0), x1, resume);
    }
    return done(-1000, 0, 10000);
}
// ARM_SMCCC_CALL_VAL(STD, SMC_64, OWNER_SIP, svc << 8 | cmd).
constexpr uint64_t kScmPil(uint32_t cmd) {return 0x42000000u | (0x02u << 8) | cmd;}
constexpr uint32_t kGpuPasId = 13;                                     // a6xx_gpu.h GPU_PAS_ID
constexpr uint64_t kZapPhys = 0x9b09a000, kZapBytes = 0x2000;          // sm8550.dtsi gpu_micro_code_mem
NativeAdrenoMemory gZapMetadata;
extern "C" {
// stage, init a0/result, mem_setup a0/result, auth a0/result, secure-mode fence result
int32_t native_gpu_zap[8];
// per SMC (init, mem_setup, auth): milliseconds and INTERRUPTED resumes; free pages before the zap and 2 s after its last step
uint32_t native_gpu_zap_cost[6], native_gpu_zap_free[2];
extern unsigned int vm_page_free_count __attribute__((weak));
}
uint32_t rd32le(const uint8_t *p) {return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;}
uint16_t rd16le(const uint8_t *p) {return uint16_t(p[0] | p[1] << 8);}
unsigned zapLimit() {
    // sheng-gpu-zap: 0 off, 1 through init_image, 2 through mem_setup and the segment copy, 3 through auth_and_reset,
    // 4 (default) also CP_SET_SECURE_MODE 0. int-019 (0) reached the desktop and int-017 (4) did not.
    unsigned zap = 4;
    PE_parse_boot_argn("sheng-gpu-zap", &zap, sizeof zap);
    return zap;
}
void zapRow() {
    // Z<stage>/<limit> i<init a0>.<ms>.<resumes> m<mem_setup ...> a<auth ...> s<secure fence> f<free pages delta>
    char row[64];
    const int32_t *z = native_gpu_zap;
    const uint32_t *c = native_gpu_zap_cost;
    snprintf(row, sizeof row, "Z%d/%u i%d.%u.%u m%d.%u.%u a%d.%u.%u s%d f%d", z[0], zapLimit(), z[1], c[0] & 0xffff,
             c[0] >> 16, z[3], c[1] & 0xffff, c[1] >> 16, z[5], c[2] & 0xffff, c[2] >> 16, z[7],
             int32_t(native_gpu_zap_free[1] - native_gpu_zap_free[0]));
    setRow(2, row);
}
void zapEnd() {
    IOSleep(2000);
    native_gpu_zap_free[1] = &vm_page_free_count ? vm_page_free_count : 0;
    zapRow();
}
void zapAndSecure() {
    int32_t *z = native_gpu_zap;
    native_gpu_zap_free[0] = native_gpu_zap_free[1] = &vm_page_free_count ? vm_page_free_count : 0;
    z[0] = 1; zapRow();
    const uint8_t *fw = &native_adreno_zap_image_size ? native_adreno_zap_image : nullptr;
    const uint64_t size = fw ? native_adreno_zap_image_size : 0;
    // mdt_header_valid: ELF32 magic, 32-byte program headers inside the file; qcom_mdt_read_metadata: >= 2 headers, the
    // first not PT_LOAD, a hash segment (p_flags type 2 in bits 24..26).
    if (size < 52 || fw[0] != 0x7f || fw[1] != 'E' || fw[2] != 'L' || fw[3] != 'F' || fw[4] != 1) {z[0] = -1; zapRow(); return;}
    const uint32_t phoff = rd32le(fw + 28);
    const unsigned phent = rd16le(fw + 42), phnum = rd16le(fw + 44);
    if (phent != 32 || phnum < 2 || phnum > 16 || phoff + uint64_t(phnum) * 32 > size) {z[0] = -2; zapRow(); return;}
    auto ph = [&](unsigned i, unsigned field) {return rd32le(fw + phoff + i * 32 + field * 4);};   // type off va pa filesz memsz flags
    if (ph(0, 0) == 1) {z[0] = -3; zapRow(); return;}
    unsigned hash = 0;
    for (unsigned i = 1; i < phnum && !hash; ++i) if (((ph(i, 6) >> 24) & 7) == 2) hash = i;
    if (!hash) {z[0] = -4; zapRow(); return;}
    uint64_t lo = ~0ull, hi = 0;
    bool relocate = false;
    for (unsigned i = 0; i < phnum; ++i) {
        if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;   // mdt_phdr_loadable
        if (ph(i, 6) & (1u << 27)) relocate = true;
        lo = lo < ph(i, 3) ? lo : ph(i, 3);
        const uint64_t end = (uint64_t(ph(i, 3)) + ph(i, 5) + 0xfff) & ~0xfffull;
        hi = hi > end ? hi : end;
    }
    if (lo >= hi || hi - lo > kZapBytes) {z[0] = -5; zapRow(); return;}
    const uint64_t ehdr = ph(0, 4), hashBytes = ph(hash, 4), hashOff = ph(hash, 1);
    if (ehdr > size || hashOff + hashBytes > size || ehdr + hashBytes > 0x4000) {z[0] = -6; zapRow(); return;}
    // Metadata: physically contiguous, 4K aligned, pushed to the point of coherency (Linux: dma_alloc_coherent).
    z[0] = 2; zapRow();
    const size_t metaBytes = size_t((ehdr + hashBytes + 0xfff) & ~0xfffull);
    if (gZapMetadata.allocate(metaBytes, 40, true, metaBytes) || gZapMetadata.spanCount() != 1) {z[0] = -7; zapRow(); return;}
    uint8_t *meta = static_cast<uint8_t *>(gZapMetadata.mapping());
    for (uint64_t i = 0; i < ehdr; ++i) meta[i] = fw[i];
    for (uint64_t i = 0; i < hashBytes; ++i) meta[ehdr + i] = fw[hashOff + i];
    if (gZapMetadata.publishForDevice(0, metaBytes, true)) {z[0] = -8; zapRow(); return;}
    // qcom_scm_pas_init_image: ARGS(2, VAL, RW).
    z[0] = 3; zapRow();
    ScmRes r = scmCall(kScmPil(0x01), 2 | (2u << 6), kGpuPasId, gZapMetadata.spans()[0].physical, 0);
    z[1] = int32_t(r.a0 ? r.a0 : int64_t(r.a1)); native_gpu_zap_cost[0] = (r.ms & 0xffff) | (r.resumes << 16);
    if (r.a0 || r.a1 || zapLimit() < 2) {zapEnd(); return;}
    if (!relocate) z[3] = -99;   // m-99: no relocatable segment, so mem_setup was not called
    if (relocate) {
        // qcom_scm_pas_mem_setup: ARGS(3).
        z[0] = 4; zapRow();
        r = scmCall(kScmPil(0x02), 3, kGpuPasId, kZapPhys, hi - lo);
        z[3] = int32_t(r.a0 ? r.a0 : int64_t(r.a1)); native_gpu_zap_cost[1] = (r.ms & 0xffff) | (r.resumes << 16);
        if (r.a0 || r.a1) {zapEnd(); return;}
    }
    // qcom_mdt_load_no_init into the reserved region (relocatable: offsets from the lowest segment address).
    z[0] = 5; zapRow();
    {
        IODeviceMemory *m = IODeviceMemory::withRange(kZapPhys, kZapBytes);
        OSSharedPtr<IOMemoryMap> map;
        if (m) {map = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain); m->release();}
        if (!map) {z[0] = -9; zapRow(); return;}
        volatile uint8_t *region = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
        const uint64_t base = relocate ? lo : kZapPhys;
        for (unsigned i = 0; i < phnum; ++i) {
            if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;
            const uint64_t at = uint64_t(ph(i, 3)) - base, file = ph(i, 4), mem = ph(i, 5), off = ph(i, 1);
            if (ph(i, 3) < base || at + mem > kZapBytes || file > mem || off + file > size) {z[0] = -10; zapRow(); return;}
            for (uint64_t b = 0; b < file; ++b) region[at + b] = fw[off + b];
            for (uint64_t b = file; b < mem; ++b) region[at + b] = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }   // the CPU view is gone before TZ takes the region
    if (zapLimit() < 3) {zapEnd(); return;}
    // qcom_scm_pas_auth_and_reset: ARGS(1).
    z[0] = 6; zapRow();
    r = scmCall(kScmPil(0x05), 1, kGpuPasId, 0, 0);
    z[5] = int32_t(r.a0 ? r.a0 : int64_t(r.a1)); native_gpu_zap_cost[2] = (r.ms & 0xffff) | (r.resumes << 16);
    if (r.a0 || r.a1 || zapLimit() < 4) {zapEnd(); return;}
    // hw_init after a successful zap load: CP_SET_SECURE_MODE (0x66) 0, then idle.
    z[0] = 7; zapRow();
    uint32_t words[2] = {};
    uint32_t fence = 0, seen = 0;
    if (AdrenoPM4Type7(0x66, 1, &words[0]) != AdrenoPM4Success || gRing.submit(words, 2, &fence)) {z[0] = -11; zapRow(); return;}
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    z[7] = int32_t(gRing.wait(fence, &clock, 200000, &seen));
    z[0] = z[7] ? 8 : 9;   // 9: TZ authenticated the zap shader and the CP left secure mode
    zapEnd();
}
extern "C" {
// GPU interrupt (row 3): attach status, GICD readback, handled interrupts, OR of the INT_0_STATUS words they cleared
uint32_t native_gpu_irq[6];
IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback,
                           uint64_t *router) __attribute__((weak));
extern volatile uint64_t armcpu_spi_unhandled __attribute__((weak));
}
volatile uint32_t gIrqLast, gIrqSeen, gIrqEmpty;
void gpuIrq(void *) {
    // a6xx_irq: read RBBM_INT_0_STATUS and clear it through RBBM_INT_CLEAR_CMD before anything else, so the level line drops
    // before the EOI. A storm (a line with no source to clear) masks the GPU interrupts instead of hanging the boot CPU; the
    // fence interrupts of a running desktop are not one (the waiters then fall back to their 100 us timeout).
    const uint32_t status = gpuRd(A740_RBBM_INT_0_STATUS);
    if (status) gpuWr(A740_RBBM_INT_CLEAR_CMD, status);
    gIrqLast = status;
    gIrqSeen = gIrqSeen | status;
    gIrqCount = gIrqCount + 1;
    gIrqEmpty = status ? 0 : gIrqEmpty + 1;
    if (gIrqEmpty > 100000) gpuWr(A740_RBBM_INT_0_MASK, 0);
    thread_wakeup(event_t(&gIrqCount));
}
bool cpFence(const uint32_t *words, size_t count) {
    uint32_t fence = 0, seen = 0;
    if (gRing.submit(words, count, &fence)) return false;
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    return !gRing.wait(fence, &clock, 200000, &seen);
}
void irqAndTimer() {
    // int-017..022 qualified the always-on counter (19.2 MHz against CNTVCT) and found SPI 300 disabled in GICD: ArmCpu's
    // distributor init disables every SPI. Now the line is attached for real.
    char row[64];
    uint32_t *q = native_gpu_irq;
    // GPU bulk copy: CP_MEMCPY (0x75: dwords, src lo/hi, dst lo/hi) of 1023 dwords of the SQE buffer to scratch+4, then
    // CP_WAIT_MEM_WRITES (0x12) and the fence; the CPU compares every dword. M<matching dwords> or M-1 when the fence failed.
    uint32_t words[8] = {};
    const uint64_t src = gGpuBuffers[0].iova, dst = gGpuBuffers[4].iova + 4;
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gGpuCpu[4]);
    for (unsigned i = 1; i < 1024; ++i) scratch[i] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    int matched = -100;
    if (AdrenoPM4Type7(0x75, 5, &words[0]) == AdrenoPM4Success && AdrenoPM4Type7(0x12, 0, &words[6]) == AdrenoPM4Success) {
        words[1] = 1023; words[2] = uint32_t(src); words[3] = uint32_t(src >> 32); words[4] = uint32_t(dst);
        words[5] = uint32_t(dst >> 32);
        if (!cpFence(words, 7)) matched = -1;
        else {
            const volatile uint32_t *sqe = reinterpret_cast<const volatile uint32_t *>(gGpuCpu[0]);
            matched = 0;
            for (unsigned i = 0; i < 1023; ++i) matched += scratch[1 + i] == sqe[i];
        }
    }
    // SPI 300 (sm8550.dtsi gpu interrupts, IRQ_TYPE_LEVEL_HIGH) = INTID 332. The memcpy fence left CP_CACHE_FLUSH_TS (bit 20)
    // set, so a working line fires as soon as it is enabled; one more fence (CP_NOP) must fire it again.
    uint32_t readback = 0;
    uint64_t router = ~0ull;
    q[0] = armcpu_spi_attach ? uint32_t(armcpu_spi_attach(332, false, gpuIrq, nullptr, &readback, &router)) : 0xdead;
    gIrqAttached = q[0] == 0;
    q[1] = readback;
    IOSleep(20);
    q[2] = gIrqCount;
    uint32_t nop = 0;
    const bool fenced = AdrenoPM4Type7(0x10, 0, &nop) == AdrenoPM4Success && cpFence(&nop, 1);
    IOSleep(20);
    q[3] = gIrqCount;
    q[4] = gIrqSeen;
    q[5] = gpuRd(A740_RBBM_INT_0_STATUS);
    // I a<attach IOReturn> b<group|enabled<<1|edge<<2|prio<<8> r<IROUTER> n<after attach>/<after a fence><fence ok>
    //   l<seen status> s<status now> M<copied dwords>
    snprintf(row, sizeof row, "I a%x b%x r%llx n%u/%u%c l%x s%x M%d", q[0], q[1], (unsigned long long)router, q[2], q[3],
             fenced ? '+' : '-', q[4], q[5], matched);
    setRow(3, row);
}

NativeAdrenoSubmit gSubmit;
NativeAdrenoUserClient gClient;
extern "C" {
// open, query, then per round: alloc/map status, submit, wait, verified words (of 4), cpu map/unmap, frees, GFSR
uint32_t native_gpu_uc[16];
}
volatile uint32_t gUcCalls, gWCalls;
IOReturn ucCall(uint32_t method, const void *in, void *out) {
    // Position marker before each call, so a stall leaves the call on screen: U m<method> n<call number>
    char mark[64];
    gUcCalls = gUcCalls + 1;
    snprintf(mark, sizeof mark, "U run m%u n%u", method, gUcCalls);
    setRow(6, mark);
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    const size_t os = NativeAdrenoUserClient::outputSize(method);
    return gClient.dispatch(method, in, NativeAdrenoUserClient::inputSize(method), os ? out : nullptr, os, &clock);
}
// One round through the user-client dispatch only: two buffer objects (an indirect buffer, a target), mapped at fresh GPU
// addresses, the IB writing three known dwords into the target with CP_MEM_WRITE, submitted, waited on by the CP timestamp
// fence, read back by the CPU, cpu-mapped/unmapped, then unmapped and freed (live SMMU unmap + TLB invalidate).
// Returns the matching dwords (of 4: three written, one untouched) or -1.
int ucRound(unsigned round, uint64_t ibIova, uint64_t tgtIova, uint32_t pattern, IOReturn &err, int &errAt, int &ok, int &n) {
    auto step = [&](IOReturn s) {
        const int idx = n++;
        if (!s) ++ok;
        else if (!err) {err = s; errAt = idx;}
        return !s;
    };
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib{}, tgt{};
    if (!step(ucCall(NativeAdrenoMethodBoAlloc, &alloc, &ib)) || !step(ucCall(NativeAdrenoMethodBoAlloc, &alloc, &tgt))) return -1;
    NativeAdrenoAbiBoMap mapIb{ib.handle, ibIova, 1, 0, 2, 0}, mapTgt{tgt.handle, tgtIova, 3, 0, 2, 0};
    NativeAdrenoAbiBoMapReply r1{}, r2{};
    if (!step(ucCall(NativeAdrenoMethodBoMap, &mapIb, &r1)) || !step(ucCall(NativeAdrenoMethodBoMap, &mapTgt, &r2))) return -1;
    NativeAdrenoMemory *ibMem = gClient.memoryOf(ib.handle), *tgtMem = gClient.memoryOf(tgt.handle);
    if (!ibMem || !tgtMem) {step(kIOReturnNotFound); return -1;}
    volatile uint32_t *iw = static_cast<volatile uint32_t *>(ibMem->mapping()), *tw = static_cast<volatile uint32_t *>(tgtMem->mapping());
    for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
    const uint32_t values[3] = {pattern, ~pattern, 0x600d0000u | round};
    uint32_t words[12];
    for (unsigned i = 0; i < 3; ++i) {
        if (AdrenoPM4Type7(0x3d, 3, &words[i * 4]) != AdrenoPM4Success) {step(kIOReturnInternalError); return -1;}
        words[i * 4 + 1] = uint32_t(tgtIova + i * 4); words[i * 4 + 2] = uint32_t(tgtIova >> 32); words[i * 4 + 3] = values[i];
    }
    for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
    if (!step(ibMem->publishForDevice(0, 4096, false)) || !step(tgtMem->publishForDevice(0, 4096, true))) return -1;
    NativeAdrenoAbiSubmit submit{};
    submit.buffer_count = 1;
    submit.buffers[0] = {ib.handle, ibIova, sizeof words};
    NativeAdrenoAbiSubmitReply sr{};
    int matched = -1;
    if (step(ucCall(NativeAdrenoMethodSubmit, &submit, &sr))) {
        NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
        if (step(ucCall(NativeAdrenoMethodWaitFence, &wait, nullptr)) && step(tgtMem->acquireDeviceWrites(0, 4096))) {
            matched = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
        }
    }
    NativeAdrenoAbiBoCpuMap cm{tgt.handle};
    NativeAdrenoAbiBoCpuMapReply cr{};
    if (step(ucCall(NativeAdrenoMethodBoCpuMap, &cm, &cr))) {
        NativeAdrenoAbiBoCpuUnmap cu{tgt.handle, cr.token, 0};
        step(ucCall(NativeAdrenoMethodBoCpuUnmap, &cu, nullptr));
    }
    NativeAdrenoAbiBoFree f1{ib.handle}, f2{tgt.handle};
    step(ucCall(NativeAdrenoMethodBoFree, &f1, nullptr));
    step(ucCall(NativeAdrenoMethodBoFree, &f2, nullptr));
    return matched;
}
// Limits and paths the Vulkan driver depends on, driven through the same dispatch entry: many live objects (the table was 64),
// one big buffer spread over many physical spans, a burst of submissions with no waits (the kernel has eight job slots and
// must retire finished ones itself; the userspace backend waits on the oldest fence when it sees Busy), and a wait on a fence
// whose job was already retired. Row 7 (48 bytes): "K a<mapped of 200> f<freed> L<large alloc status> s<spans of the 16 MiB BO>
// v<big words ok of 2> J<jobs of 20> y<busy retries> m<burst result matches> r<retired-fence wait status> t<big map ms>".
void ucStress() {
    char row[64];
    constexpr unsigned kMany = 200;
    static uint64_t handles[kMany];
    unsigned mapped = 0, freed = 0;
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    for (unsigned i = 0; i < kMany; ++i) {
        NativeAdrenoAbiBoAllocReply reply{};
        if (ucCall(NativeAdrenoMethodBoAlloc, &alloc, &reply) || !reply.handle) break;
        handles[i] = reply.handle;
        NativeAdrenoAbiBoMap map{reply.handle, 0x30000000ull + uint64_t(i) * 0x2000, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply mr{};
        if (ucCall(NativeAdrenoMethodBoMap, &map, &mr)) {
            NativeAdrenoAbiBoFree f{reply.handle};
            ucCall(NativeAdrenoMethodBoFree, &f, nullptr);
            break;
        }
        ++mapped;
    }
    for (unsigned i = 0; i < mapped; ++i) {
        NativeAdrenoAbiBoFree f{handles[i]};
        if (!ucCall(NativeAdrenoMethodBoFree, &f, nullptr)) ++freed;
    }
    // One 16 MiB buffer: IB writes the first and last dword through the GPU's own translation of a many-span mapping.
    NativeAdrenoAbiBoAlloc bigAlloc{16ull << 20, 12, 0}, small{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply big{}, ib{};
    IOReturn bigStatus = ucCall(NativeAdrenoMethodBoAlloc, &bigAlloc, &big);
    unsigned spans = 0, mapMs = 0;
    int bigWords = -1;
    constexpr uint64_t kBigIova = 0x40000000ull, kIbIova = 0x10010000ull;
    if (!bigStatus && !ucCall(NativeAdrenoMethodBoAlloc, &small, &ib)) {
        NativeAdrenoAbiBoMap mb{big.handle, kBigIova, 3, 0, 2, 0}, mi{ib.handle, kIbIova, 1, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        NativeAdrenoMemory *bm = gClient.memoryOf(big.handle), *im = gClient.memoryOf(ib.handle);
        if (bm) spans = unsigned(bm->spanCount());
        const uint64_t mapStart = ringNow(nullptr);
        const IOReturn bigMap = bm && im ? ucCall(NativeAdrenoMethodBoMap, &mb, &r) : kIOReturnNotReady;
        mapMs = unsigned((ringNow(nullptr) - mapStart) / 1000);
        if (bm && im && !bigMap && !ucCall(NativeAdrenoMethodBoMap, &mi, &r)) {
            volatile uint32_t *bw = static_cast<volatile uint32_t *>(bm->mapping());
            volatile uint32_t *iw = static_cast<volatile uint32_t *>(im->mapping());
            const size_t last = ((16u << 20) / 4) - 1;
            bw[0] = 0xdeadbeef;
            bw[last] = 0xdeadbeef;
            const uint64_t addrs[2] = {kBigIova, kBigIova + (16ull << 20) - 4};
            const uint32_t vals[2] = {0x11112222u, 0x33334444u};
            uint32_t words[8];
            bool built = true;
            for (unsigned i = 0; i < 2; ++i) {
                built = built && AdrenoPM4Type7(0x3d, 3, &words[i * 4]) == AdrenoPM4Success;
                words[i * 4 + 1] = uint32_t(addrs[i]); words[i * 4 + 2] = uint32_t(addrs[i] >> 32); words[i * 4 + 3] = vals[i];
            }
            for (unsigned i = 0; i < 8; ++i) iw[i] = words[i];
            if (built && !im->publishForDevice(0, 4096, false) && !bm->publishForDevice(0, 16u << 20, true)) {
                NativeAdrenoAbiSubmit submit{};
                submit.buffer_count = 1;
                submit.buffers[0] = {ib.handle, kIbIova, sizeof words};
                NativeAdrenoAbiSubmitReply sr{};
                if (!ucCall(NativeAdrenoMethodSubmit, &submit, &sr)) {
                    NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
                    if (!ucCall(NativeAdrenoMethodWaitFence, &wait, nullptr) && !bm->acquireDeviceWrites(0, 16u << 20))
                        bigWords = (bw[0] == vals[0]) + (bw[last] == vals[1]);
                }
            }
        }
    }
    // Burst: n submissions of the same IB with no wait calls in between.
    constexpr unsigned kJobs = 20;
    unsigned submitted = 0, busy = 0;
    int burstMatch = -1;
    IOReturn retiredWait = kIOReturnNotReady;
    NativeAdrenoAbiBoAllocReply tgt{};
    if (!ucCall(NativeAdrenoMethodBoAlloc, &small, &tgt)) {
        constexpr uint64_t kTgtIova = 0x10020000ull;
        NativeAdrenoAbiBoMap mt{tgt.handle, kTgtIova, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        NativeAdrenoMemory *tm = gClient.memoryOf(tgt.handle), *im = gClient.memoryOf(ib.handle);
        if (tm && im && !ucCall(NativeAdrenoMethodBoMap, &mt, &r)) {
            volatile uint32_t *tw = static_cast<volatile uint32_t *>(tm->mapping());
            volatile uint32_t *iw = static_cast<volatile uint32_t *>(im->mapping());
            tw[0] = 0;
            // the IB (still mapped at kIbIova) now writes 0xb0057000 to the burst target; its content is rewritten once
            uint32_t words[4];
            if (AdrenoPM4Type7(0x3d, 3, &words[0]) == AdrenoPM4Success) {
                words[1] = uint32_t(kTgtIova); words[2] = uint32_t(kTgtIova >> 32); words[3] = 0xb0057000u;
                for (unsigned i = 0; i < 4; ++i) iw[i] = words[i];
                if (!im->publishForDevice(0, 4096, false) && !tm->publishForDevice(0, 4096, true)) {
                    NativeAdrenoAbiSubmit submit{};
                    submit.buffer_count = 1;
                    submit.buffers[0] = {ib.handle, kIbIova, sizeof words};
                    uint64_t fences[kJobs] = {};
                    for (unsigned j = 0; j < kJobs; ++j) {
                        NativeAdrenoAbiSubmitReply sr{};
                        IOReturn st = ucCall(NativeAdrenoMethodSubmit, &submit, &sr);
                        for (unsigned tries = 0; st == kIOReturnBusy && tries < 64; ++tries) {
                            ++busy;
                            // the userspace backend's answer to Busy: wait for the oldest fence it still knows
                            NativeAdrenoAbiWaitFence w{fences[j > 8 ? j - 8 : 0], 200000000ull};
                            ucCall(NativeAdrenoMethodWaitFence, &w, nullptr);
                            st = ucCall(NativeAdrenoMethodSubmit, &submit, &sr);
                        }
                        if (st) break;
                        fences[j] = sr.fence;
                        ++submitted;
                    }
                    if (submitted) {
                        NativeAdrenoAbiWaitFence last{fences[submitted - 1], 200000000ull};
                        if (!ucCall(NativeAdrenoMethodWaitFence, &last, nullptr) && !tm->acquireDeviceWrites(0, 4096))
                            burstMatch = tw[0] == 0xb0057000u;
                        // an early fence whose job the kernel retired long ago must still read as complete
                        NativeAdrenoAbiWaitFence early{fences[0], 0};
                        retiredWait = ucCall(NativeAdrenoMethodWaitFence, &early, nullptr);
                    }
                }
            }
        }
    }
    // Drain and free what the stress made (the caller closes the client afterwards).
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    gSubmit.waitAll(&clock, 200000);
    const uint64_t leftovers[3] = {big.handle, ib.handle, tgt.handle};
    for (const uint64_t h : leftovers) {
        if (!h) continue;
        NativeAdrenoAbiBoFree f{h};
        ucCall(NativeAdrenoMethodBoFree, &f, nullptr);
    }
    snprintf(row, sizeof row, "K a%u f%u L%x s%u v%d J%u y%u m%d r%x t%u", mapped, freed, unsigned(bigStatus) & 0xfff, spans,
             bigWords, submitted, busy, burstMatch, unsigned(retiredWait) & 0xfff, mapMs);
    setRow(7, row);
}
void userClientTest() {
    uint32_t *u = native_gpu_uc;
    char row[64];
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm ? NativeAdrenoGpuContextVm() : nullptr;
    if (!vm) {setRow(6, "U novm"); return;}
    IOReturn err = 0;
    int errAt = -1, ok = 0, n = 0;
    const IOReturn attached = gSubmit.attach(vm, &gRing);
    // input_bits 32: the 4 GiB TTBR0 aperture; output_bits 40 for the BO allocations
    const IOReturn opened = attached ? attached : gClient.open(NativeAdrenoAbiVersion, vm, &gSubmit, &gRing, 32, 40);
    u[0] = uint32_t(attached);
    u[1] = uint32_t(opened);
    if (opened) {snprintf(row, sizeof row, "U open a%x o%x", u[0], u[1]); setRow(6, row); return;}
    NativeAdrenoAbiParams params{};
    if (!ucCall(NativeAdrenoMethodQuery, nullptr, &params)) ok += params.input_bits == 32 && params.output_bits == 40;
    ++n;
    const uint32_t irq0 = gIrqCount;
    // Round 2 reuses the IB address with a new object (a stale TLB entry would run the old IB, which writes to the now
    // unmapped round-1 target and faults) and maps its target at a fresh address, so a stale entry cannot write freed memory.
    const int v1 = ucRound(1, 0x10000000, 0x10001000, 0x5e9a7400u, err, errAt, ok, n);
    const int v2 = ucRound(2, 0x10000000, 0x10002000, 0xa740b0b0u, err, errAt, ok, n);
    u[2] = uint32_t(v1);
    u[3] = uint32_t(v2);
    u[4] = uint32_t(err);
    u[5] = uint32_t(errAt);
    u[6] = gIrqCount - irq0;
    u[7] = gSmmuMap ? *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + 0x48) : 0xdead;
    setRow(6, "U ok.. (stress running)");
    ucStress();
    const IOReturn closed = gClient.close();
    u[8] = uint32_t(closed);
    u[9] = uint32_t(gSubmit.stats().completed);
    // U ok<steps ok>/<steps> e<first error>@<step> v<round1>/<round2 dwords of 4> q<GPU irqs> c<close> s<CB0 FSR> G<GFSR>
    snprintf(row, sizeof row, "U ok%d/%d e%x@%d v%d/%d q%u c%x s%x G%x", ok, n, unsigned(err), errAt, v1, v2, u[6],
             unsigned(closed), gpuFsr(), u[7]);
    setRow(6, row);
}
NativeAdrenoTimer gTimer;
// Constant facts for the Vulkan driver, each from a pinned source: chip id and gmem from a6xx_catalog.c (a740, "C510v2"),
// UBWC 4.0 / swizzle levels 2+3 / bank bit 16 / macrotile from ubwc_config.c sm8550_data, the GMEM window and trap base from the
// values hw_init programs above. The VA window keeps clear of the kernel buffers (below 0x302000+) and the GMEM window
// (0x1000000..0x12fffff) and ends at the 4 GiB aperture.
const NativeAdrenoAbiDeviceInfo gDeviceInfo = {0x43050a01u, 3u << 20, 0x1000000ull, 0x1fffffffff000ull, 0x20000000ull,
                                               0xE0000000ull, 16, 6, 1, 4, NativeAdrenoDeviceClientIova, 0};
int timerRead(void *, uint32_t reg, uint32_t *value) {*value = gpuRd(reg); return 0;}
const NativeAdrenoTimeIo gTimerIo{nullptr, timerRead};
IOReturn wcall(IOUserClient *uc, uint32_t method, const void *in, void *out) {
    char mark[64];
    gWCalls = gWCalls + 1;
    snprintf(mark, sizeof mark, "P run m%u n%u", method, gWCalls);
    setRow(5, mark);
    IOExternalMethodArguments a{};
    a.version = kIOExternalMethodArgumentsCurrentVersion;
    a.selector = method;
    const size_t is = NativeAdrenoUserClient::inputSize(method), os = NativeAdrenoUserClient::outputSize(method);
    a.structureInput = is ? in : nullptr;
    a.structureInputSize = uint32_t(is);
    a.structureOutput = os ? out : nullptr;
    a.structureOutputSize = uint32_t(os);
    return uc->externalMethod(method, &a, nullptr, nullptr, nullptr);
}
// Exactly the path a userspace client takes, minus the Mach trap: newUserClient, the external-method table and its lock,
// BoCpuMap -> clientMemoryForType -> a write-combined mapping (the GPU's PTEs are non-cacheable, so no cache maintenance),
// the IB written and the result read through that mapping only, then clientClose (drain, unmap, free) and a second
// connection to prove the slot recycles. Returns the matching dwords (of 4) or -1.
int wrapperRound(IOService *svc, int &ok, int &n, IOReturn &err, int &errAt, uint32_t &memStatus) {
    auto step = [&](IOReturn s) {
        const int idx = n++;
        if (!s) ++ok;
        else if (!err) {err = s; errAt = idx;}
        return !s;
    };
    IOUserClient *uc = nullptr;
    if (!step(svc->newUserClient(kernel_task, nullptr, 0, nullptr, &uc)) || !uc) return -1;
    IOUserClient *second = nullptr;
    step(svc->newUserClient(kernel_task, nullptr, 0, nullptr, &second) == kIOReturnBusy ? 0 : kIOReturnError);
    if (second) second->release();
    NativeAdrenoAbiParams params{};
    step(wcall(uc, NativeAdrenoMethodQuery, nullptr, &params));
    NativeAdrenoAbiDeviceInfo info{};
    step(wcall(uc, NativeAdrenoMethodGetDeviceInfo, nullptr, &info));
    step(info.chip_id == 0x43050a01u && info.gmem_bytes == (3u << 20) ? 0 : kIOReturnError);
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib{}, tgt{};
    int matched = -1;
    IOMemoryDescriptor *desc[2] = {};
    IOMemoryMap *maps[2] = {};
    if (step(wcall(uc, NativeAdrenoMethodBoAlloc, &alloc, &ib)) && step(wcall(uc, NativeAdrenoMethodBoAlloc, &alloc, &tgt))) {
        const uint64_t ibIova = 0x10004000, tgtIova = 0x10005000;
        NativeAdrenoAbiBoMap mapIb{ib.handle, ibIova, 1, 0, 2, 0}, mapTgt{tgt.handle, tgtIova, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        NativeAdrenoAbiBoCpuMap cmIb{ib.handle}, cmTgt{tgt.handle};
        NativeAdrenoAbiBoCpuMapReply crIb{}, crTgt{};
        if (step(wcall(uc, NativeAdrenoMethodBoMap, &mapIb, &r)) && step(wcall(uc, NativeAdrenoMethodBoMap, &mapTgt, &r)) &&
            step(wcall(uc, NativeAdrenoMethodBoCpuMap, &cmIb, &crIb)) && step(wcall(uc, NativeAdrenoMethodBoCpuMap, &cmTgt, &crTgt))) {
            IOOptionBits opt[2] = {};
            const uint32_t tokens[2] = {crIb.token, crTgt.token};
            bool mapped = true;
            for (int i = 0; i < 2; ++i) {
                const IOReturn s = uc->clientMemoryForType(tokens[i], &opt[i], &desc[i]);
                if (!memStatus) memStatus = uint32_t(s);
                if (s || !desc[i]) {mapped = false; continue;}
                maps[i] = desc[i]->createMappingInTask(kernel_task, 0, opt[i]);
                if (!maps[i]) {mapped = false; if (!memStatus) memStatus = 0xbad;}
            }
            // type 0 and a stale token must be refused
            IOMemoryDescriptor *none = nullptr;
            IOOptionBits o = 0;
            step(uc->clientMemoryForType(0, &o, &none) != kIOReturnSuccess && !none ? 0 : kIOReturnError);
            step(mapped ? 0 : kIOReturnNoMemory);
            if (mapped) {
                volatile uint32_t *iw = reinterpret_cast<volatile uint32_t *>(maps[0]->getVirtualAddress());
                volatile uint32_t *tw = reinterpret_cast<volatile uint32_t *>(maps[1]->getVirtualAddress());
                for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
                const uint32_t values[3] = {0x0badc0deu, 0xfeed0001u, 0x600d0003u};
                uint32_t words[12];
                bool built = true;
                for (unsigned i = 0; i < 3; ++i) {
                    built = built && AdrenoPM4Type7(0x3d, 3, &words[i * 4]) == AdrenoPM4Success;
                    words[i * 4 + 1] = uint32_t(tgtIova + i * 4); words[i * 4 + 2] = uint32_t(tgtIova >> 32);
                    words[i * 4 + 3] = values[i];
                }
                for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
                __asm__ volatile("dsb sy" ::: "memory");
                NativeAdrenoAbiSubmit submit{};
                submit.buffer_count = 1;
                submit.buffers[0] = {ib.handle, ibIova, sizeof words};
                NativeAdrenoAbiSubmitReply sr{};
                if (step(built ? 0 : kIOReturnInternalError) && step(wcall(uc, NativeAdrenoMethodSubmit, &submit, &sr))) {
                    NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
                    if (step(wcall(uc, NativeAdrenoMethodWaitFence, &wait, nullptr))) {
                        __asm__ volatile("dsb sy" ::: "memory");
                        matched = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
                    }
                }
                NativeAdrenoAbiTimestamp ts{};
                step(wcall(uc, NativeAdrenoMethodGetTimestamp, nullptr, &ts));
            }
        }
    }
    for (auto *m : maps) if (m) m->release();
    for (auto *d : desc) if (d) d->release();
    // clientClose drains the CP, unmaps and frees everything (including the leases still held), then terminates.
    step(uc->clientClose());
    uc->release();
    return matched;
}
// IO coherence (sheng-gpu-coherent, default 1), before any client: the A740 is ADRENO_QUIRK_HAS_CACHED_COHERENT behind the
// dma-coherent GPU SMMU (a6xx_catalog.c, sm8550.dtsi), and Linux maps its cached BOs with IOMMU_CACHE (write-back, inner shareable).
// A page of kernel memory is imported and mapped that way, and both directions are proved against DRAM:
//   read:  the CPU stores new values over lines DRAM still holds old (counted dirty through an uncached mapping), the GPU copies the
//          page, and the copy DRAM holds must be the new values, which only the CPU's caches had;
//   write: the CPU keeps clean lines of a page it read, the GPU copies other values over it, and plain cached loads (no
//          invalidate) must return the GPU's values.
// Only both proofs turn native_adreno_io_coherent on; imported user memory is then mapped write-back inner-shareable and no longer
// cleaned and invalidated around every submission (int-143: about 12 ms each, every imported IOSurface page every time).
// Row: H d<dirty lines of 64> r<GPU read new>/<old> w<CPU saw GPU>/<DRAM> s<status> -> on/off/skip
extern "C" volatile uint32_t native_adreno_io_coherent __attribute__((weak));
extern "C" {char native_gpu_coherence_row[120]; uint32_t native_gpu_coherence[8];}
bool dramPage(uint64_t pa, uint32_t *out) {
    const vm_offset_t page = ml_io_map_unmappable(vm_offset_t(pa), 0x1000, 0x7 /* VM_WIMG_IO */);
    if (!page) return false;
    for (unsigned i = 0; i < 1024; ++i) out[i] = reinterpret_cast<const volatile uint32_t *>(page)[i];
    ml_io_unmap(page, 0x1000);
    return true;
}
bool gpuCopyPage(uint64_t src, uint64_t dst) {
    uint32_t words[8] = {};
    if (AdrenoPM4Type7(0x75, 5, &words[0]) != AdrenoPM4Success || AdrenoPM4Type7(0x12, 0, &words[6]) != AdrenoPM4Success) return false;
    words[1] = 1024; words[2] = uint32_t(src); words[3] = uint32_t(src >> 32); words[4] = uint32_t(dst); words[5] = uint32_t(dst >> 32);
    return cpFence(words, 7);
}
unsigned gCoherenceAttempts;
void coherenceProbe(NativeAdrenoVm *vm) {
    uint32_t *k = native_gpu_coherence;
    unsigned wanted = 1;
    PE_parse_boot_argn("sheng-gpu-coherent", &wanted, sizeof wanted);
    if (!wanted || !vm || !&native_adreno_io_coherent) {
        snprintf(native_gpu_coherence_row, sizeof native_gpu_coherence_row, "H skip (arg %u vm %d)", wanted, vm != nullptr);
        return;
    }
    constexpr uint64_t kIova = 0x18000000;
    constexpr size_t kBytes = 4 * 4096;
    uint8_t *buf = static_cast<uint8_t *>(IOMallocAligned(kBytes, 4096));
    NativeAdrenoMemory mem;
    uint64_t handle = 0;
    IOReturn st = buf ? mem.importTask(kernel_task, uint64_t(uintptr_t(buf)), kBytes, 40, kBytes) : kIOReturnNoMemory;
    k[0] = 1;
    if (!st) {k[0] = 2; st = vm->map(&mem, 0, kIova, 3, 1, 3, &handle);}
    uint64_t phys[4] = {};
    for (size_t j = 0; !st && j < mem.spanCount(); ++j)
        for (size_t at = 0; at < mem.spans()[j].bytes; at += 4096) phys[(mem.spans()[j].offset + at) / 4096] = mem.spans()[j].physical + at;
    static uint32_t words[1024];
    bool pass = false;
    if (!st && phys[0] && phys[1] && phys[2] && phys[3]) {
        k[0] = 3;
        volatile uint32_t *S = reinterpret_cast<volatile uint32_t *>(buf), *D = S + 1024, *R = S + 2048, *T = S + 3072;
        // read direction. The proof needs S's new words still dirty in a CPU cache (DRAM still holding the old ones, k[1] >= 48 of 64
        // lines) when the GPU copies S. int-150 found every line already written back once (d0/64) while both directions were right
        // (r1024/0 w1024/1024): the precondition, not the snoop, had failed, and the whole boot ran with per-submission maintenance.
        // So the setup repeats until the lines are seen dirty, up to eight times (attempts in the row), and only then does the GPU copy.
        bool dirty = false;
        for (unsigned attempt = 0; attempt < 8 && !st && !dirty; ++attempt) {
            gCoherenceAttempts = attempt + 1;
            for (unsigned i = 0; i < 1024; ++i) {S[i] = 0x11110000u | i; D[i] = 0; R[i] = 0x33330000u | i; T[i] = 0x44440000u | i;}
            st = NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
            for (unsigned i = 0; i < 1024; ++i) S[i] = 0x22220000u | i;
            __asm__ volatile("dsb sy" ::: "memory");
            k[1] = 0;
            if (!st && dramPage(phys[0], words)) for (unsigned i = 0; i < 1024; i += 16) k[1] += words[i] == (0x11110000u | i);
            dirty = k[1] >= 48;
        }
        if (!st && dirty) {
            k[0] = 4;
            if (gpuCopyPage(kIova, kIova + 4096) && dramPage(phys[1], words)) {
                k[0] = 5;
                for (unsigned i = 0; i < 1024; ++i) {k[2] += words[i] == (0x22220000u | i); k[3] += words[i] == (0x11110000u | i);}
            }
        }
        // write direction: T written back and kept (clean, resident), then read so its lines are present when the GPU writes
        if (!st && k[0] == 5) st = NativeAdrenoMaintainOwnedPages(const_cast<uint32_t *>(T), 4096, NativeAdrenoClean);
        uint32_t touch = 0;
        for (unsigned i = 0; i < 1024; ++i) touch += T[i];
        __asm__ volatile("dsb sy" ::: "memory");
        if (!st && k[0] == 5 && gpuCopyPage(kIova + 8192, kIova + 12288)) {
            k[0] = 6;
            for (unsigned i = 0; i < 1024; ++i) k[4] += T[i] == (0x33330000u | i);
            if (dramPage(phys[3], words)) for (unsigned i = 0; i < 1024; ++i) k[5] += words[i] == (0x33330000u | i);
        }
        (void)touch;   /* the loads are volatile; the sum only keeps them one expression */
        pass = k[0] == 6 && k[1] >= 48 && k[2] == 1024 && k[4] == 1024 && k[5] == 1024;
    }
    k[6] = uint32_t(st);
    if (handle) (void)vm->unmap(handle);
    // Whatever the result, no line of these pages stays in a CPU cache (a stale clean copy would outlive the free).
    if (buf) (void)NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
    (void)mem.release();
    if (buf) IOFreeAligned(buf, kBytes);
    k[7] = pass;
    native_adreno_io_coherent = pass ? 1 : 0;
    snprintf(native_gpu_coherence_row, sizeof native_gpu_coherence_row, "H d%u/64 a%u r%u/%u w%u/%u s%x k%u -> %s", k[1], gCoherenceAttempts,
             k[2], k[3], k[4], k[5], k[6], k[0], pass ? "on" : "off");
}
void publishAndWrapperTest(bool rounds) {
    char row[64];
    if (!native_adreno_publish || !native_adreno_published_service || !native_adreno_publish_stats) {setRow(5, "P nolink"); return;}
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm ? NativeAdrenoGpuContextVm() : nullptr;
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    // userClientTest attaches the submit object; with the self-tests skipped it still has to be attached before a client opens.
    if (vm) gSubmit.attach(vm, &gRing);
    native_adreno_dram_read32 = dramRead32;
    native_adreno_cached_read32 = cachedRead32;
    native_adreno_line_audit = lineAudit;
    coherenceProbe(vm);
    const IOReturn timer = gTimer.attach(&gTimerIo, NativeAdrenoTimeCp);
    {unsigned wc = 1; PE_parse_boot_argn("sheng-gpu-wc", &wc, sizeof wc); native_adreno_wc_enabled = wc ? 1 : 0;}
    const IOReturn published = native_adreno_publish(vm, &gSubmit, &gRing, &clock, timer ? nullptr : &gTimer, &gDeviceInfo, 32, 40);
    IOService *svc = published ? nullptr : native_adreno_published_service();
    int ok = 0, n = 0, errAt = -1, v1 = -1, v2 = -1;
    IOReturn err = 0;
    uint32_t memStatus = 0;
    if (svc && rounds) {
        v1 = wrapperRound(svc, ok, n, err, errAt, memStatus);
        v2 = wrapperRound(svc, ok, n, err, errAt, memStatus);   // the slot must recycle; the IOVAs were unmapped by the close
    }
    uint32_t closed = 0;
    const uint32_t opened = native_adreno_publish_stats(&closed);
    // P s<publish IOReturn> w<steps ok>/<steps> e<first error>@<step> v<r1>/<r2 dwords of 4> m<clientMemoryForType> o<opened>/<closed>
    snprintf(row, sizeof row, "P s%x w%d/%d e%x@%d v%d/%d m%x o%u/%u s%x", unsigned(published), ok, n, unsigned(err), errAt, v1, v2,
             memStatus, opened, closed, gpuFsr());
    setRow(5, rounds ? row : "P published (rounds skipped)");
}
// sheng-gpu-metal=1 publishes the Adreno accelerator for the original Metal discovery (the plugin bundle itself arrives through the
// boot-time addon mount, sheng-gpu-addon=1, and the trust cache). sheng-gpu-selftest=1 runs the kernel-driven user-client tests that
// int-024..031 already qualified, which frees the diagnostic rows for the plugin's own result.
unsigned bootArg(const char *name, unsigned fallback) {
    unsigned value = fallback;
    PE_parse_boot_argn(name, &value, sizeof value);
    return value;
}
volatile uint32_t gMetalStage = 0, gMetalRegistered = 0;
volatile int gMetalKext = -99, gMetalStatus = 0;
// Stages: 1 kext load attempted, 2 parent metaclass found, 3 own class registered, 4 object created and initialised, 5 published.
void publishMetal() {
    gMetalStage = 1;
    if (!native_adreno_accelerator_static_init || !native_adreno_accelerator_allocate || !native_adreno_accelerator_instance_size) {
        gMetalStatus = kIOReturnUnsupported;
        return;
    }
    gMetalKext = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (gMetalKext) {gMetalStatus = gMetalKext; return;}
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) {gMetalStatus = kIOReturnNotFound; return;}
    gMetalStage = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {gMetalStatus = kIOReturnNoResources; return;}
    native_adreno_accelerator_static_init();
    gMetalStatus = OSMetaClass::postModLoad(handle);
    if (gMetalStatus) return;
    if (native_adreno_accelerator_instance_size() != 136) {gMetalStatus = kIOReturnInvalid; return;}
    gMetalStage = 3;
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator) {gMetalStatus = kIOReturnNoMemory; return;}
    if (!accelerator->init()) {accelerator->release(); gMetalStatus = kIOReturnError; return;}
    // The two keys the original _MTLRegisterDevices helper reads. The plugin name is ours: the bundle exists only through the
    // addon mount, and no Apple device identity (gpu,t8011 and the like) is claimed.
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    // The one process (the render server) whose open of this accelerator and its client the kernel's open path grants in place of the sandbox verdict
    // (patches/xnu/zzw-native-adreno-sandbox-grant.patch): the original profiles name no class of this driver.
    accelerator->setProperty("NativeSandboxGrantProcess", "backboardd,iconservicesagent,WidgetRenderer_Default,PosterBoard");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    gMetalStage = 4;
    if (!accelerator->attach(IOService::getPlatform())) {accelerator->release(); gMetalStatus = kIOReturnNotAttached; return;}
    accelerator->registerService();
    gMetalRegistered = 1;
    gMetalStage = 5;
}
// Rows refreshed by the heartbeat thread: 6 = the stage chain, 7 = who reached the driver, 5 = the plugin's own report.
// M a<addon stage>.<status> k<kext load> s<publish stage>/<status> r<registered>
// D o<opened>/<busy>/<closed> p<property reads> <latest reader>><latest opener>   R<plugin report>
// Is the launchd job's process alive? proc_iterate over every process, looking for the daemon by name (12 characters, "adrenoprobed").
volatile uint32_t gProcTotal, gDaemonSeen, gDaemonAlive, gDaemonPid;
int scanProcess(void *proc, void *) {
    gProcTotal = gProcTotal + 1;
    const char *name = proc_name_address(proc);
    if (name && !strncmp(name, "adrenoprobed", 12)) {gDaemonAlive = 1; gDaemonSeen = 1; gDaemonPid = gDaemonPid + 1;}   // pid not read: the header's proc_pid prototype differs
    return 0;   // PROC_RETURNED: keep iterating
}
void metalRows() {
    if (!gMetalStage && !native_gpu_addon_stage) return;
    char row[64];
    const NativeAdrenoMetalDiagView &d = native_adreno_metal_diag;
    gProcTotal = 0;
    gDaemonAlive = 0;
    proc_iterate(1 /* PROC_ALLPROCLIST */, scanProcess, nullptr, nullptr, nullptr);
    // Row 6: both overlay mounts (stage.status; 5 = mounted and our own file visible), IOGPUFamily load, publication stage, plugin opens
    // granted/refused/closed, MetalPluginName reads, and the daemon process: d<ever seen><alive now>:<pid> of <process count>.
    snprintf(row, sizeof row, "M a%d.%x/%d.%x k%x s%u/%x r%u o%u/%u/%u n%u d%u%u:%u/%u", native_gpu_addon_stage,
             unsigned(native_gpu_addon_status), native_gpu_addon2_stage, unsigned(native_gpu_addon2_status), unsigned(gMetalKext),
             gMetalStage, unsigned(gMetalStatus), gMetalRegistered, d.opens, d.busy, d.closes, d.name_reads, gDaemonSeen, gDaemonAlive,
             gDaemonPid, gProcTotal);
    setRow(6, row);
    // Row 7: the plugin's progress markers (open types 0x4147xxxx), oldest first: 1 started, 2 symbols, 3 opened, 1xxx probe
    // (xxx admitted selectors), 2000/2001 mailbox failed/ok, 3xxx step xxx done, 4xxx step failed (xxx = status), 5xxx reported;
    // after 4F01 the two halves of the failed open's kern_return_t.
    char tail[48] = {};
    const unsigned total = d.marks_total, shown = total < 7 ? total : 7;
    size_t at = 0;
    for (unsigned i = 0; i < shown && at < sizeof tail - 6; ++i)
        at += snprintf(tail + at, sizeof tail - at, " %x", d.marks[(total - shown + i) & 15]);
    snprintf(row, sizeof row, "K%u%s", total, tail);
    setRow(7, row);
    // GPU7/GPU8 (wide): the raw test's report line and the Turnip phase's line (T result or W watchdog sample).
    if (d.report[0]) snprintf(boot_progress_ext_wide[0], sizeof boot_progress_ext_wide[0], "R %.63s", d.report);
    if (d.report2[0]) snprintf(boot_progress_ext_wide[1], sizeof boot_progress_ext_wide[1], "%.63s", d.report2);
    {
        // Row 5: the newest four IOServiceOpen attempts as <process><P plugin type|F other type><MACF result of the sandbox>; 0 = admitted.
        char list[48] = {};
        const unsigned count = native_adreno_open_log_count, show = count < 1 ? count : 1;   // one, so the call counters fit the 47 columns
        size_t used = 0;
        for (unsigned i = 0; i < show && used < sizeof list - 12; ++i) {
            const NativeAdrenoOpenRecordView &r = native_adreno_open_log[(count - 1 - i) & 7];
            used += snprintf(list + used, sizeof list - used, " %.6s%c%x", r.name, r.foreign ? 'F' : 'P', r.macf);
        }
        // ... a<attach> x<selectors that reached the driver> c<calls done> f<method running now + 1, 0 = none> l<last method>/<its status>
        // ... c<calls done> f<method running now + 1> l<last method>/<status> g<stage reached in the free/unmap path: 1-4 freeBo, 10-14 VM unmap, 30-34 release>
        snprintf(row, sizeof row, "S%s x%u c%u f%u l%u/%x g%u u%u", list, d.ext_entries, d.calls, d.inflight, d.last_selector,
                 d.last_status & 0xfff, native_adreno_trace_stage, d.forced_unmaps);
        setRow(5, row);
    }
}
// Exec observer: every successful exec is announced to kauth fileop listeners after the image was activated (code signature accepted).
// This shows whether launchd ever started the owner job /private/var/ngpu/bin/adrenoprobed, and the last few programs it started.
extern "C" void *kauth_listen_scope(const char *identifier,
    int (*callback)(void *cred, void *idata, unsigned action, uintptr_t arg0, uintptr_t arg1, uintptr_t arg2, uintptr_t arg3), void *idata);
extern "C" const char *vnode_getname(void *vp);
extern "C" void vnode_putname(const char *name);
volatile uint32_t gExecTotal, gExecDaemon;
char gExecNames[6][9];
volatile void *gExecListener;
int execObserver(void *, void *, unsigned action, uintptr_t vnode, uintptr_t, uintptr_t, uintptr_t) {
    if (action != 6 /* KAUTH_FILEOP_EXEC */ || !vnode) return 3;   // KAUTH_RESULT_DEFER
    const char *name = vnode_getname(reinterpret_cast<void *>(vnode));
    if (name) {
        if (!strncmp(name, "adrenoprobed", 12)) gExecDaemon = gExecDaemon + 1;
        const uint32_t slot = gExecTotal % 6;
        strlcpy(gExecNames[slot], name, sizeof gExecNames[slot]);
        vnode_putname(name);
    }
    gExecTotal = gExecTotal + 1;
    return 3;
}
// Vnode authorization observer, from the first moment of the probe thread: names the files the boot chain touches before any process can report.
// e: execute authorizations of adrenoprobed (an exec attempt, before the code signature check), p: any access to our job plist / the shadowed bluetoothd /
// iomfb_fdr_loader plists (did launchd read them), b: accesses to the plugin binary NativeAdrenoMetal / the original AGXMetalA10 binary, with the
// name of the last process that touched the plugin binary.
extern "C" int proc_selfname(char *buf, int size);
volatile uint32_t gVnE, gVnAny, gVnPlist[3], gVnPlugin, gVnOriginal, gVnBundle;
char gVnPluginProc[17], gVnPlistProc[17];
volatile void *gVnodeListener;
int vnodeObserver(void *, void *, unsigned action, uintptr_t, uintptr_t vnode, uintptr_t, uintptr_t) {
    if (!vnode) return 3;
    const char *name = vnode_getname(reinterpret_cast<void *>(vnode));
    if (!name) return 3;
    switch (name[0]) {
    case 'a':
        if (!strcmp(name, "adrenoprobed")) {
            gVnAny = gVnAny + 1;
            if (action & 4 /* KAUTH_VNODE_EXECUTE */) gVnE = gVnE + 1;
        } else if (!strcmp(name, "AGXMetalA10")) gVnOriginal = gVnOriginal + 1;
        break;
    case 'A':
        if (!strcmp(name, "AGXMetalA10")) gVnOriginal = gVnOriginal + 1;
        break;
    case 'o':
        if (!strcmp(name, "org.blu-sharky.adrenoprobed.plist")) {gVnPlist[0] = gVnPlist[0] + 1; proc_selfname(gVnPlistProc, sizeof gVnPlistProc - 1);}
        break;
    case 'c':
        if (!strcmp(name, "com.apple.bluetoothd.plist")) {gVnPlist[1] = gVnPlist[1] + 1; proc_selfname(gVnPlistProc, sizeof gVnPlistProc - 1);}
        else if (!strcmp(name, "com.apple.iomfb_fdr_loader.plist")) {gVnPlist[2] = gVnPlist[2] + 1; proc_selfname(gVnPlistProc, sizeof gVnPlistProc - 1);}
        break;
    case 'N':
        if (!strcmp(name, "NativeAdrenoMetal")) {
            gVnPlugin = gVnPlugin + 1;
            proc_selfname(gVnPluginProc, sizeof gVnPluginProc - 1);
        } else if (!strcmp(name, "NativeAdrenoMetal.bundle")) gVnBundle = gVnBundle + 1;
        break;
    default:
        break;
    }
    vnode_putname(name);
    return 3;
}
void registerExecObserver() {
    if (!gExecListener) gExecListener = kauth_listen_scope("com.apple.kauth.fileop", execObserver, nullptr);
    if (!gVnodeListener) gVnodeListener = kauth_listen_scope("com.apple.kauth.vnode", vnodeObserver, nullptr);
}
// Does the interrupt controller deliver SGI 0 between cores? The kernel sends IPIs through ArmCpu::signalCPU, but every boot page has shown
// ipi=0 received, and the restartable-ranges panic (a thread waiting for an IPI acknowledgement) says delivery may be broken. This sends
// one SGI from this core to each other core with the same ICC_SGI1R_EL1 encoding signalCPU uses and counts what arrives. Row 2:
// IPI k<sent by the kernel> t<self-test sent> r<received in total> m<cores that answered, bit = core index>
extern "C" unsigned int processor_avail_count;
volatile unsigned gIpiRuns;
// Self-SGI probe (system registers only): row 2 ends with s<cores that ran a probe> p<INTIDs 0..15 that became pending as Group 1>/<as Group 0>,
// accumulated over every IPI test run. Row 3 is the boot core's GIC readback after enabling: g<IGROUPR0> e<ISENABLER0> c<GICD_CTLR> t<GICD_TYPER>
// m<IGRPMODR0> and the ICC registers of the last probing core P<PMR> I<IGRPEN1> S<SRE> C<CTLR> R<RPR>.
unsigned gSelfTried, gSelfPend1, gSelfPend0;
uint64_t gSelfLast[10];
void selfProbe() {
    if (!armcpu_sgi_selfprobe) return;
    for (unsigned i = 0; i < 8; ++i) {
        uint64_t out[10] = {};
        armcpu_sgi_selfprobe(out);
        gSelfTried |= 1u << unsigned((out[0] >> 8) & 7);
        gSelfPend1 |= unsigned(out[1]); gSelfPend0 |= unsigned(out[2]);
        for (unsigned k = 0; k < 10; ++k) gSelfLast[k] = out[k];
        IOSleep(25);
    }
}
void gicRow() {
    char row[64];
    // L e<execute authorizations>/<any access of adrenoprobed> p<our plist>/<bluetoothd plist>/<fdr plist reads> b<plugin binary>/<original AGXMetalA10> B<bundle dir> <last plugin toucher>
    snprintf(row, sizeof row, "L e%u/%u p%u/%u/%u %.6s b%u/%u B%u %.6s", gVnE, gVnAny, gVnPlist[0], gVnPlist[1], gVnPlist[2], gVnPlistProc,
             gVnPlugin, gVnOriginal, gVnBundle, gVnPluginProc);
    setRow(3, row);
}
void ipiSelfTest() {
    if (!armcpu_ipi_selftest || !&armcpu_ipi_interrupts) {setRow(2, "IPI nolink"); return;}
    unsigned mask = 0;
    for (unsigned cpu = 0; cpu < 8; ++cpu) {
        const uint64_t received = armcpu_ipi_interrupts;
        armcpu_ipi_selftest(uint64_t(cpu) << 8);
        for (unsigned wait = 0; wait < 20 && armcpu_ipi_interrupts == received; ++wait) IOSleep(1);
        if (armcpu_ipi_interrupts != received) mask |= 1u << cpu;
    }
    char row[64];
    gIpiRuns = gIpiRuns + 1;
    // Row 2: IPI r<received> m<cores that answered> #<round> X<execs> a<adrenoprobed execs> s<tried> p<pending G1>/<pending G0>
    snprintf(row, sizeof row, "IPI k%llu r%llu m%x #%u X%u a%u", (unsigned long long)armcpu_ipi_sent, (unsigned long long)armcpu_ipi_interrupts, mask,
             gIpiRuns, gExecTotal, gExecDaemon);
    setRow(2, row);
    selfProbe();
    snprintf(row, sizeof row, "IPI k%llu r%llu m%x #%u X%u a%u p%x/%x", (unsigned long long)armcpu_ipi_sent, (unsigned long long)armcpu_ipi_interrupts,
             mask, gIpiRuns, gExecTotal, gExecDaemon, gSelfPend1, gSelfPend0);
    setRow(2, row);
    gicRow();
}
void startHeartbeatThread();
void gpuStart() {
    char row[64];
    uint32_t *c = native_gpu_cp;
    if (!NativeAdrenoGpuContextBringup || !NativeAdrenoGpuContextBuffers || !gSmmuMap) {setRow(6, "Q nolink"); return;}
    // 1. The GPU's own context bank and buffers (arm-smmu-qcom: the GPU always gets bank 0; DT iommus SIDs 0 and 1).
    NativeAdrenoGmuContextReport &q = native_gpu_context;
    // The BO pool (one contiguous block, sheng-gpu-pool MiB, default 1536) is taken inside the bring-up, before memory fragments. int-159:
    // with 1536 MiB no BO fell back to scattered pages (peak 782 MiB in use); with 384 MiB (int-160) 18 did, 81933 single-page spans
    // whose attribute XNU kept setting back (123847 pages write-combined again at submissions, 766 ms).
    {unsigned pool = 1536; PE_parse_boot_argn("sheng-gpu-pool", &pool, sizeof pool); native_adreno_pool_mib = pool;}
    const IOReturn ctx = NativeAdrenoGpuContextBringup(reinterpret_cast<volatile uint8_t *>(gSmmuMap->getVirtualAddress()),
        kSmmuWindow, 0, 2, 0, 0, 1, 1, gpuCtxProgress, &q);
    // Q s<step> st<IOReturn> k<program>/<attach kit status> p<phase>e<enabled> r<route phase>v<verified> n<spans>
    snprintf(row, sizeof row, "Q s%u st%x k%u/%u p%ue%u r%uv%u n%u", q.step, unsigned(ctx), q.program_status, q.route_status,
             q.program_phase, q.program_enabled, q.route_phase, q.routes_verified, q.spans);
    setRow(6, row);
    if (ctx) return;
    gpuSetActlr();
    c[0] = 1;
    if (NativeAdrenoGpuContextBuffers(gGpuBuffers)) {setRow(7, "C1 buffers refused"); return;}
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(gGpuBuffers[i].physical, gGpuBuffers[i].bytes);
        if (m) {
            gGpuMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gGpuMaps[i]) {setRow(7, "C1 nomap buffer"); return;}
        gGpuCpu[i] = reinterpret_cast<volatile uint8_t *>(gGpuMaps[i]->getVirtualAddress());
    }
    // 2. SQE: adreno_fw_create_bo copies the firmware without its first dword.
    c[0] = 2;
    const uint64_t sqe = &native_adreno_sqe_firmware_image_size ? native_adreno_sqe_firmware_image_size : 0;
    if (sqe < 8 || sqe - 4 > gGpuBuffers[0].bytes) {setRow(7, "C2 no sqe image"); return;}
    for (uint64_t i = 4; i < sqe; ++i) gGpuCpu[0][i - 4] = native_adreno_sqe_firmware_image[i];
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint64_t i = 4; i < sqe; ++i) if (gGpuCpu[0][i - 4] != native_adreno_sqe_firmware_image[i]) {c[1] = uint32_t(i); break;}
    if (c[1]) {snprintf(row, sizeof row, "C2 sqe readback differs at %x", c[1]); setRow(7, row); return;}
    // 3. hw_init (a6xx_gpu.c), A740 branches only. OOB GPU_SET is already held (mode 8).
    c[0] = 3;
    gpuWr(A740_GBIF_HALT, 0); (void)gpuRd(A740_GBIF_HALT);
    gpuWr(A740_RBBM_GBIF_HALT, 0); (void)gpuRd(A740_RBBM_GBIF_HALT);
    gpuWr(A740_RBBM_SECVID_TSB_CNTL, 0);
    gpuWr64(A740_RBBM_SECVID_TSB_TRUSTED_BASE, 0);
    gpuWr(A740_RBBM_SECVID_TSB_TRUSTED_SIZE, 0);
    // a6xx_set_hwcg(true): gmu_cgc_mode 0x00020202, cgc delay 0x10111, hyst 0x5555, then the table unless already on.
    gmuWr(A740_GPU_GMU_AO_GMU_CGC_MODE_CNTL, 0x00020202);
    gmuWr(A740_GPU_GMU_AO_GMU_CGC_DELAY_CNTL, 0x10111);
    gmuWr(A740_GPU_GMU_AO_GMU_CGC_HYST_CNTL, 0x5555);
    if (gpuRd(A740_RBBM_CLOCK_CNTL) != 0x8aa8aa82) {
        for (unsigned i = 0; i < NATIVE_ADRENO_A740_HWCG_COUNT; ++i) gpuWr(NativeAdrenoA740Hwcg[i][0], NativeAdrenoA740Hwcg[i][1]);
        gpuWr(A740_RBBM_CLOCK_CNTL, 0x8aa8aa82);
    }
    gpuWr(A740_RBBM_GBIF_CLIENT_QOS_CNTL, 0x2120212);
    gpuWr(A740_UCHE_GBIF_GX_CONFIG, 0x10240e0);
    gpuWr(A740_RBBM_PERFCTR_GPU_BUSY_MASKED, 0xffffffff);
    gpuWr64(A740_UCHE_TRAP_BASE, 0x1fffffffff000ull);
    gpuWr64(A740_UCHE_WRITE_THRU_BASE, 0x1fffffffff000ull);
    gpuWr64(A740_UCHE_GMEM_RANGE_MIN, 0x1000000);                    // a740 family: SZ_16M
    gpuWr64(A740_UCHE_GMEM_RANGE_MAX, 0x1000000 + 3 * 0x100000 - 1);  // gmem 3 MiB
    gpuWr(A740_UCHE_CACHE_WAYS, 1u << 23);
    gpuWr(A740_CP_AHB_CNTL, 0x1);
    gpuWr(A740_RBBM_PERFCTR_CNTL, 0x1);
    gpuWr(A740_RBBM_INTERFACE_HANG_INT_CNTL, (1u << 30) | 0xcfffff);
    gpuWr(A740_UCHE_CLIENT_PF, (1u << 7) | 0x1);
    // a6xx_set_cp_protect: ACCESS_PROT_EN | ACCESS_FAULT_ON_VIOL_EN | LAST_SPAN_INF_RANGE, then a730_protect.
    gpuWr(A740_CP_PROTECT_CNTL, (1u << 0) | (1u << 1) | (1u << 3));
    for (unsigned i = 0; i < NATIVE_ADRENO_A740_PROTECT_COUNT; ++i)
        gpuWr(A740_CP_PROTECT + NativeAdrenoA740Protect[i][0], NativeAdrenoA740Protect[i][1]);
    gpuWr(A740_UCHE_CMDQ_CONFIG, (6u << 16) | (6u << 12) | (9u << 8) | (1u << 3) | (1u << 2) | 2u);
    // hw_apriv: A7XX_BR_APRIVMASK (ICACHE|RBFETCH|RBPRIVLEVEL|RBRPWB|CDREAD|CDWRITE) and A7XX_APRIV_MASK.
    gpuWr(A740_CP_APRIV_CNTL, 0x6f);
    gpuWr(A740_CP_BV_APRIV_CNTL, 0x0f);
    gpuWr(A740_CP_LPAC_APRIV_CNTL, 0x0f);
    gpuWr(A740_RB_CMP_DBG_ECO_CNTL, gpuRd(A740_RB_CMP_DBG_ECO_CNTL) | (1u << 11));
    // a6xx_set_ubwc_config, sm8550_data: UBWC 4.0 encoder, swizzle levels 2+3, highest bank bit 16 (hbb_lo 3, hbb_hi 0),
    // AMSBC on (>= 3.0), rgb565 predicator on (>= 4.0), min_acc_len_64b off, ubwc_mode off, uavflagprd_inv 2,
    // macrotile mode on.
    gpuWr(A740_RB_NC_MODE_CNTL, (1u << 11) | (1u << 4) | (3u << 1));
    gpuWr(A740_TPL1_NC_MODE_CNTL, 3u << 1);
    gpuWr(A740_SP_NC_MODE_CNTL, (2u << 4) | (3u << 1));
    for (uint32_t pipe = 1; pipe <= 2; ++pipe) {   // PIPE_BR, PIPE_BV
        gpuWr(A740_CP_APERTURE_CNTL_HOST, pipe << 12);
        gpuWr(A740_GRAS_NC_MODE_CNTL, 3u << 5);
    }
    gpuWr(A740_CP_APERTURE_CNTL_HOST, 0);           // PIPE_NONE
    gpuWr(A740_UCHE_MODE_CNTL, 3u << 21);
    gpuWr(A740_RBBM_NC_MODE_CNTL, 1);
    // A7XX_INT_MASK: bits 1,6,7,8,9,15,17,20,22,23,24,25,28,29. Nothing services the GPU interrupt yet; status is polled.
    gpuWr(A740_RBBM_INT_0_MASK, 0x33d283c2);
    gpuWr64(A740_CP_SQE_INSTR_BASE, gGpuBuffers[0].iova);
    // 4. Ring 0 (MSM_GPU_RB_CNTL_DEFAULT: 32 KiB, block 32; hw_apriv uses the hardware rptr shadow at memptrs+0).
    c[0] = 4;
    const NativeAdrenoRingIo rio{nullptr, ringWrite, ringRead, ringBarrier};
    const NativeAdrenoRingCache cache{nullptr, ringPublish, ringAcquire};
    native_adreno_fence_phys = gGpuBuffers[2].physical;   // the page the CP writes its timestamps to, for the UFS log
    const IOReturn attached = gRing.attach(&rio, &cache, const_cast<uint8_t *>(gGpuCpu[1]), gGpuBuffers[1].bytes,
        gGpuBuffers[1].iova, const_cast<uint8_t *>(gGpuCpu[2]), gGpuBuffers[2].bytes, gGpuBuffers[2].iova);
    if (attached) {snprintf(row, sizeof row, "C4 ring attach %x", unsigned(attached)); setRow(7, row); return;}
    gpuWr64(A740_CP_BV_RB_RPTR_ADDR, gGpuBuffers[2].iova + 8);   // rbmemptr(bv_rptr)
    gpuWr(A740_CP_SQE_CNTL, 1);
    // 5. a7xx_patch_pwrup_reglist (preemption list only; IFPC is off) and a7xx_cp_init through the ring.
    c[0] = 5;
    size_t written = 0;
    const IOReturn list = NativeAdrenoPwrupList::buildA740(ringRead, nullptr, const_cast<uint8_t *>(gGpuCpu[3]),
        gGpuBuffers[3].bytes, &written);
    if (list) {snprintf(row, sizeof row, "C5 pwrup list %x", unsigned(list)); setRow(7, row); return;}
    __asm__ volatile("dsb sy" ::: "memory");
    const NativeAdrenoCpInitConfig config{gGpuBuffers[0].iova, gGpuBuffers[3].iova, 1};   // base written above, before SQE_CNTL
    uint32_t fence = 0;
    c[2] = uint32_t(gCpInit.run(&rio, &gRing, &config, &fence));
    if (c[2]) {cpRow("ref"); return;}
    c[0] = 6;
    cpRow("sent");
    const NativeAdrenoRingClock clock{nullptr, ringNow, ringIdle};
    c[3] = gRing.wait(fence, &clock, 200000, &c[4]);
    if (c[3]) {cpRow(c[3] == NativeAdrenoRingFenceTimeout ? "to" : "flt"); return;}
    // 6. First real command: CP_MEM_WRITE (opcode 0x3d: address lo/hi, data) into the scratch page, then its fence.
    c[0] = 7;
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gGpuCpu[4]);
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t words[4] = {};
    if (AdrenoPM4Type7(0x3d, 3, &words[0]) != AdrenoPM4Success) {cpRow("pm4"); return;}
    words[1] = uint32_t(gGpuBuffers[4].iova); words[2] = uint32_t(gGpuBuffers[4].iova >> 32); words[3] = 0x5e9a740;
    c[5] = uint32_t(gRing.submit(words, 4, &fence));
    if (c[5]) {cpRow("wref"); return;}
    c[6] = gRing.wait(fence, &clock, 200000, &c[4]);
    c[7] = scratch[0];
    c[0] = c[6] ? 8 : (c[7] == 0x5e9a740 ? 10 : 9);
    // C10 = the GPU wrote the scratch word through its own 4K context and signalled the fence.
    cpRow(c[0] == 10 ? "ok" : c[6] ? "wto" : "wmiss");
    // sheng-gpu-zap=0 skips the zap/secure-mode step (int-019 bisect: int-017 stalled userspace at the logo, int-018 with
    // the probe off reached the desktop). The CP then stays in secure mode, as in int-016.
    if (c[0] == 10) {
        setRow(6, "S zap");
        if (zapLimit()) zapAndSecure();
        else setRow(2, "Z off");
        startHeartbeatThread();
        setRow(6, "S irq");
        irqAndTimer();
        const bool selftest = bootArg("sheng-gpu-selftest", 0) != 0;
        if (selftest) userClientTest();
        setRow(5, "P start");
        publishAndWrapperTest(selftest);
        if (bootArg("sheng-gpu-metal", 0) == 1) publishMetal();
    }
}
void heartbeat();
volatile bool gHeartbeatThread;
void heartbeatThread(void *, wait_result_t) {heartbeat();}
// The heartbeat runs on its own thread from the end of the zap, so the GPU rows keep refreshing even if a self-test below
// stalls (int-027's first BoAlloc never returned and froze rows 4..7).
void startHeartbeatThread() {
    thread_t thread = nullptr;
    if (kernel_thread_start(heartbeatThread, nullptr, &thread) == KERN_SUCCESS) {thread_deallocate(thread); gHeartbeatThread = true;}
}
void heartbeat() {
    // Row 4 from now on: seconds since the run ended, GX state, and (only with GX on) the GPU status words, so a photo
    // tells a live kernel from a hung one and shows whether the GPU stays idle.
    char row[64];
    for (unsigned t = 0;; t += 2) {
        uint32_t info = 0;
        gColdDevice.io.read32(gColdDevice.io.cookie, ShengGmuCore, kG2hInfo, &info);
        if (gxOn() && gpuRd(A740_CP_RB_RPTR) != gpuRd(A740_CP_RB_WPTR)) {
            // The ring is not drained: where each CP thread is (a6xx.xml A7XX: BV RB_RPTR 0xa66, IB1/IB2 base and remaining dwords
            // 0x928/0x92a and 0x92b/0x92d, BV IB1 0xa6d/0xa70), so a photograph tells which IB and how far into it the CP stopped.
            // Rows are 47 characters, so IB bases show their low 24 bits.
            snprintf(row, sizeof row, "H%u R%x r%x/%x b%x i%x/%x j%x v%x/%x", t, gpuRd(A740_RBBM_STATUS), gpuRd(A740_CP_RB_RPTR),
                     gpuRd(A740_CP_RB_WPTR), gpuRd(0x0a66), gpuRd(0x0928) & 0xffffff, gpuRd(0x092a), gpuRd(0x092d),
                     gpuRd(0x0a6d) & 0xffffff, gpuRd(0x0a70));
            // Row 3 (the load trace, finished by now): RBBM_STATUS1..3 (0x211..0x213; STATUS3 bit 24 SMMU stalled on fault),
            // CP_INTERRUPT_STATUS 0x823, CP_PROTECT_STATUS for BR/BV (0x824 / 0xa61) and the GPU context bank FSR (a terminated
            // translation fault does not stall the SMMU, so STATUS3 alone cannot rule one out).
            char stall[64];
            snprintf(stall, sizeof stall, "S%x/%x/%x I%x P%x/%x f%x a%x/%x c%x", gpuRd(0x0211), gpuRd(0x0212),
                     gpuRd(0x0213), gpuRd(0x0823), gpuRd(0x0824), gpuRd(0x0a61), gpuFsr(), gActlr, gActlrSet, gSacr);
            setRow(3, stall);
            // Row 2: each thread's SQE PC and the header of the packet it is on. a6xx_cp_hw_err_irq() reads CP_SQE_STAT_DATA after
            // selecting index 1 for the current opcode; index 0 is the PC and successive reads advance (crashdec).
            uint32_t sqe[4];
            gpuWr(0x0908, 0); sqe[0] = gpuRd(0x0909); sqe[1] = gpuRd(0x0909);
            gpuWr(0x0a87, 0); sqe[2] = gpuRd(0x0a88); sqe[3] = gpuRd(0x0a88);
            snprintf(stall, sizeof stall, "Q br %x %08x bv %x %08x", sqe[0], sqe[1], sqe[2], sqe[3]);
            setRow(2, stall);
            // int-070..099 showed the last submission's coherence sample here once per hang; int-100: the banner's GPUC/GPUW rows carry the write-combine audit, so row 2 keeps the SQE row: PC 0x222/0x1c3 with a zero
            // header is the stale-IB trap of int-065..069, anything else a wait inside the IB.
        } else if (gxOn()) {
            // Without a stall, row 2 shows the coherence samples, so a passing run still shows whether a CPU cache held an IB.
            if (native_adreno_coherence[0]) {
                char sample[64];
                // C<samples> m<samples with differing values> and the first such sample (else the newest).
                const unsigned at = native_adreno_coherence[4] ? 5 : 1;
                snprintf(sample, sizeof sample, "C%u m%u c%x u%x v%x", native_adreno_coherence[0], native_adreno_coherence[4],
                         native_adreno_coherence[at], native_adreno_coherence[at + 1], native_adreno_coherence[at + 2]);
                setRow(2, sample);
            }
            const volatile uint32_t *mem = reinterpret_cast<const volatile uint32_t *>(gGpuCpu[2]);
            // s<CB0 FSR> G<global GFSR at +0x48>: does the GPU fault through its context after the secure-mode exit?
            const uint32_t gfsr = gSmmuMap ? *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + 0x48) : 0xdead;
            // q<GPU interrupts handled> u<unhandled SPIs>
            snprintf(row, sizeof row, "H%u R%x I%x F%x r%x/%x f%x g%x s%x G%x q%u u%llu", t, gpuRd(A740_RBBM_STATUS),
                     gpuRd(A740_RBBM_INT_0_STATUS), gpuRd(A740_CP_HW_FAULT), gpuRd(A740_CP_RB_RPTR), gpuRd(A740_CP_RB_WPTR),
                     mem ? mem[1] : 0, info, gpuFsr(), gfsr, gIrqCount,
                     (unsigned long long)(&armcpu_spi_unhandled ? armcpu_spi_unhandled : 0));
        } else {
            snprintf(row, sizeof row, "H%u gx-off g%x", t, info);
        }
        setRow(4, row);
        metalRows();
        registerExecObserver();
        if (!(gxOn() && gpuRd(A740_CP_RB_RPTR) != gpuRd(A740_CP_RB_WPTR))) gicRow();
        if (t == 4 || t == 40 || t == 120) ipiSelfTest();
        IOSleep(2000);
    }
}

void gpuOn() {
    char row[64];
    ShengGmuIo &io = gColdDevice.io;
    uint32_t *g = native_gpu_gx;
    auto rd = [&](unsigned w, uint32_t at) {uint32_t v = 0xdeadbeef; io.read32(io.cookie, w, at, &v); return v;};
    native_gpu_power_stage = 0x81;
    // a6xx_gmu_resume, after a successful a6xx_hfi_start: clear GMU2HOST and unmask only CM3_FAULT (A6XX_HFI_IRQ_MASK).
    io.write32(io.cookie, ShengGmuCore, kG2hClr, ~0u);
    io.write32(io.cookie, ShengGmuCore, kG2hMask, ~(1u << 23));
    io.barrier(io.cookie);
    g[3] = rd(ShengGmuCore, kGxStatus);
    // a6xx_gmu_set_initial_freq -> a6xx_hfi_set_freq: {ack_type 1 (blocking), freq index, bw index}. `sheng-gpu-perf=<n>` picks the
    // GPU OPP (1 = 220 MHz ... 8 = 680 MHz, default 1) and the bandwidth level that OPP's opp-peak-kBps names; the bandwidth vote is
    // also the DDR floor while the GPU is on (no GPU DCVS changes either afterwards).
    unsigned perf = 1, bw = 1;
    PE_parse_boot_argn("sheng-gpu-perf", &perf, sizeof(perf));
    perf = ShengGmuPerfVote ? ShengGmuPerfVote(perf, &bw) : 1;
    if (!ShengGmuPerfVote) bw = 1;
    native_gpu_perf_vote[0] = perf; native_gpu_perf_vote[1] = bw;
    const uint32_t vote[3] = {1, perf, bw};
    uint32_t out[16], error = 0;
    size_t count = 0;
    g[0] = AdrenoHfiExchange(&gColdDevice.hfi, 30, vote, 3, 1000, hfiWait, out, 16, &count, &error);
    g[1] = error;
    native_gpu_power_stage = 0x82;
    if (!g[0]) {
        // hw_init: a6xx_gmu_set_oob(GMU_OOB_GPU_SET), non-legacy bits: request 30, ack 31, 10 ms at 100 us.
        io.write32(io.cookie, ShengGmuCore, kH2gSet, 1u << 30);
        io.barrier(io.cookie);
        for (g[2] = 1; g[2] <= 100; ++g[2]) {
            if (rd(ShengGmuCore, kG2hInfo) & (1u << 31)) break;
            IODelay(100);
        }
        io.write32(io.cookie, ShengGmuCore, kG2hClr, 1u << 31);
        io.barrier(io.cookie);
    }
    g[4] = rd(ShengGmuCore, kGxStatus);
    uint32_t gdscr = 0;
    if (gVa[GPB_WIN_GPUCC]) gdscr = *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPUCC] + GPB_CC_GX_GDSCR);
    g[5] = gdscr;
    native_gpu_power_stage = 0x83;
    // GX on per a7xx_gmu_gx_is_on: GX_HM_GDSC_POWER_OFF (bit 0) and GX_HM_CLK_OFF (bit 1) both clear.
    const bool gx = g[2] <= 100 && !(g[4] & 3);
    if (gx) {
        g[6] = rd(ShengGmuGpu, kRbbmStatus);
        g[7] = rd(ShengGmuGpu, kGbifHalt);
    }
    // X v<vote status>/<fw error> o<ack polls, 101 = none> x<gx before>><after> c<GX GDSCR> R<RBBM_STATUS> H<GBIF_HALT>
    if (gx) snprintf(row, sizeof row, "X v%u/%x o%u x%x>%x c%08x R%08x H%x P%u/%u", g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7],
                      native_gpu_perf_vote[0], native_gpu_perf_vote[1]);
    else snprintf(row, sizeof row, "X v%u/%x o%u x%x>%x c%08x gx-off P%u/%u", g[0], g[1], g[2], g[3], g[4], g[5],
                   native_gpu_perf_vote[0], native_gpu_perf_vote[1]);
    setRow(5, row);
    native_gpu_power_stage = gx ? 0x87 : 0x8f;
    if (gx && mode() >= 9) gpuStart();
    if (gHeartbeatThread) for (;;) IOSleep(60000);
    heartbeat();
}
void gmuColdStart(const gpb_result &r) {
    char row[64];
    native_gpu_power_stage = 0x71;
    if (!ShengGmuColdStart || !ShengGmuBindMmio || !NativeAdrenoGmuContextPayloads || !native_adreno_firmware_image ||
        !&native_adreno_firmware_image_size || native_adreno_firmware_image_size < 4) {
        setRow(7, "S nolink");
        return;
    }
    for (unsigned w = 0; w < 5; ++w) {
        IODeviceMemory *memory = IODeviceMemory::withRange(kColdPhys[w], kColdLen[w]);
        if (memory) {
            gColdMaps[w] = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
            memory->release();
        }
        if (!gColdMaps[w]) {setRow(7, "S nomap window"); return;}
        gColdMmio.windows[w] = {reinterpret_cast<volatile unsigned char *>(gColdMaps[w]->getVirtualAddress()), kColdPhys[w],
                                kColdLen[w]};
    }
    NativeAdrenoGmuPayload payloads[5];
    const IOReturn got = NativeAdrenoGmuContextPayloads(payloads);
    if (got) {snprintf(row, sizeof row, "S payloads %x", static_cast<unsigned>(got)); setRow(7, row); return;}
    ShengGmuMemory *memory[5] = {&gColdConfig.icache, &gColdConfig.dummy, &gColdConfig.debug, &gColdConfig.log, &gColdConfig.hfi};
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(payloads[i].physical, payloads[i].bytes);
        if (m) {
            gPayloadMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gPayloadMaps[i]) {setRow(7, "S nomap payload"); return;}
        *memory[i] = {reinterpret_cast<volatile uint32_t *>(gPayloadMaps[i]->getVirtualAddress()), payloads[i].bytes,
                      payloads[i].iova};
    }
    ShengGmuDevice &d = gColdDevice;
    d = {};
    if (ShengGmuBindMmio(&gColdMmio, &d.io)) {setRow(7, "S bind refused"); return;}
    d.services = {nullptr, coldSha256, coldNow, coldWait};
    gColdConfig.chip_id = 0x43050a01;
    gColdConfig.firmware = native_adreno_firmware_image;
    gColdConfig.firmware_bytes = static_cast<size_t>(native_adreno_firmware_image_size);
    gColdConfig.poll_budget = 200;   // ShengGmu also stops each poll at 10 ms, as gmu_poll_timeout(.., 100, 10000)
    // a6xx_gmu_resume, before a6xx_gmu_fw_start: clear every AO interrupt and unmask only the fatal ones.
    d.io.write32(d.io.cookie, ShengGmuCore, kAoIrqClr, ~0u);
    d.io.write32(d.io.cookie, ShengGmuCore, kAoIrqMask, ~kAoFatal);
    d.io.barrier(d.io.cookie);
    uint32_t before[4], after[4];
    readGmuState(before);
    const ShengGmuStartupLimits limits = {1000, 100, 50000, 2000000};
    setRow(7, "S running");
    native_gpu_power_stage = 0x72;
    const ShengGmuStatus status = ShengGmuColdStart(&d, &gColdConfig, &gStartup, &limits, &gColdResult);
    native_gpu_power_stage = status ? 0x7f : 0x77;
    readGmuState(after);
    const ShengGmuResult &l = gColdResult.load;
    const AdrenoHfiStartupResult &p = gColdResult.protocol;
    // GPU1: state before > after (SYSRESET/TCM_RET/RSC busy/HFI status); running firmware reads 0/1/0/1
    snprintf(row, sizeof row, "GPU m7 G%x/%x/%x/%x>G%x/%x/%x/%x", before[0], before[1], before[2], before[3], after[0],
             after[1], after[2], after[3]);
    setRow(4, row);
    // GPU3: P<protocol status> g<stage> k<acknowledged stage mask> e<firmware error> t<transport> n<published commands>
    //   <elapsed ms> s<skipped response packets> h<first skipped header> r<its return header> (int-013: P8 t6 on the
    //   first reply, which the old exact-header match rejected)
    const AdrenoHfi &q = gColdDevice.hfi;
    snprintf(row, sizeof row, "P%u g%u k%x e%x t%u n%u %llums s%u h%x r%x", gColdResult.protocol_status, p.stage,
             p.acknowledged_stages, p.firmware_error, p.transport_status, p.published_commands,
             static_cast<unsigned long long>(p.elapsed_us / 1000), q.skipped, q.skipped_header, q.skipped_ret);
    setRow(6, row);
    // GPU4: S<ShengGmuStatus> ph<load phase> h<hash ok> f<FW_INIT_RESULT> H<HFI status> r<FW ready>/<HFI ready>
    //   a<AO fault status> c<GMU2HOST info> v<reset verified>
    snprintf(row, sizeof row, "S%u ph%u h%u f%x H%x r%u%u a%x c%x v%u", status, l.phase, l.complete_hash_verified,
             l.firmware_init_result, l.hfi_status, l.firmware_ready_observed, l.hfi_ready_observed, l.ao_fault_status,
             l.cm3_fault_status, l.reset_verified);
    setRow(7, row);
    if (mode() >= 8 && status == ShengGmuSuccess) gpuOn();
    (void)r;
}
void gmuPrerequisites(const gpb_result &r) {
    char row[64];
    gpb_io io{nullptr, rd, wr, delayUs};
    gpb_result scratch = r;   // gpb_rd records io_error into this copy, not into the published result
    gpb_gmu_clocks &k = native_gpu_gmu_clocks;
    native_gpu_power_stage = 0x51;
    gpb_read_gmu_clocks(&io, &scratch, &k);
    // K p<PLL1 MODE> o<OPMODE> l<L_VAL> g<gmu root off><cfg> h<hub root off><cfg>
    //   b<cx_gmu><cxo><hub_cx_int><demet><gcc axi><gcc memnoc> v<GPLL0 vote bits 15/16>
    if (!k.done) snprintf(row, sizeof row, "K skip");
    else snprintf(row, sizeof row, "p%08x o%x l%08x g%x%03x h%x%03x b%x%x%x%x%x%x v%x", k.pll1_mode, k.pll1_opmode,
                  k.pll1_l, k.gmu_cmd >> 31, k.gmu_cfg & 0x7ff, k.hub_cmd >> 31, k.hub_cfg & 0x7ff,
                  gpb_branch_digit(k.cx_gmu), gpb_branch_digit(k.cxo), gpb_branch_digit(k.hub_cx_int),
                  gpb_branch_digit(k.demet), gpb_branch_digit(k.axi), gpb_branch_digit(k.memnoc), (k.gpll0_vote >> 15) & 3);
    setRow(6, row);
    if (mode() >= 6 && k.done) {
        // C s<last step> f<0 ok, 1 timeout/refused, 2 I/O> l<lock polls> u<hub>/<gmu update polls> then PLL1 MODE/OPMODE
        //   and the hub/gmu CFG low bits after the switch (505 = GPLL0/3, 303 = PLL1/2)
        native_gpu_power_stage = 0x61;
        gpb_gmu_clock_on &o = native_gpu_gmu_clock_on;
        gpb_gmu_clocks_on(&io, &scratch, &k, &o);
        snprintf(row, sizeof row, "C s%u f%u l%u u%u/%u p%08x o%x h%03x g%03x", o.step, o.failed, o.lock_polls, o.hub_polls,
                 o.gmu_polls, o.pll1_mode, o.pll1_opmode, o.hub_cfg & 0x7ff, o.gmu_cfg & 0x7ff);
        setRow(5, row);
        gpb_gmu_clocks after;
        gpb_read_gmu_clocks(&io, &scratch, &after);
        snprintf(row, sizeof row, "p%08x o%x l%08x g%x%03x h%x%03x b%x%x%x%x%x%x v%x", after.pll1_mode, after.pll1_opmode,
                 after.pll1_l, after.gmu_cmd >> 31, after.gmu_cfg & 0x7ff, after.hub_cmd >> 31, after.hub_cfg & 0x7ff,
                 gpb_branch_digit(after.cx_gmu), gpb_branch_digit(after.cxo), gpb_branch_digit(after.hub_cx_int),
                 gpb_branch_digit(after.demet), gpb_branch_digit(after.axi), gpb_branch_digit(after.memnoc),
                 (after.gpll0_vote >> 15) & 3);
        setRow(6, row);
        k.cx_gmu = after.cx_gmu;
    }
    // Command DB: copy with aligned 32-bit reads (the mapping is Device memory), then parse a stable snapshot.
    native_gpu_power_stage = 0x52;
    unsigned parse = 9, build = 9;
    uint32_t version = 0, magic = 0;
    if (volatile uint32_t *db = mapWords(kCmdDbBase, kCmdDbBytes, gCmdDbMap)) {
        for (uint64_t i = 0; i < kCmdDbBytes / 4; ++i) {
            const uint32_t w = db[i];
            memcpy(gCmdDb + 4 * i, &w, 4);
        }
        memcpy(&version, gCmdDb, 4);
        memcpy(&magic, gCmdDb + 4, 4);
        if (ShengGmuCommandDbInputs && ShengGmuBuildStartup) {
            gVoteInputs = {};
            gVoteInputs.chip_id = 0x43050a01;   // a6xx_catalog.c A740 chip_ids, the ID ShengGmuVotes is pinned to
            parse = ShengGmuCommandDbInputs(gCmdDb, kCmdDbBytes, version, &gVoteInputs);
            if (parse == ShengGmuDbSuccess) build = ShengGmuBuildStartup(&gVoteInputs, &gStartup);
        } else parse = 8;
    }
    // GMU state words, only with the GMU clock and the GPUCC AHB clock already running (nothing here enables them).
    native_gpu_power_stage = 0x53;
    bool gmu = gpb_gmu_regs_readable(&scratch, &k);
    if (gmu) {
        volatile uint32_t *core = mapWords(kGmuPhys[0], kGmuLen[0], gGmuMaps[0]);
        volatile uint32_t *rscc = mapWords(kGmuPhys[1], kGmuLen[1], gGmuMaps[1]);
        volatile uint32_t *misc = mapWords(kGmuPhys[2], kGmuLen[2], gGmuMaps[2]);
        gmu = core && rscc && misc;
        if (gmu) {
            native_gpu_gmu_state[0] = core[kGmuSysReset / 4];
            native_gpu_gmu_state[1] = misc[kCxMiscTcmRet / 4];
            native_gpu_gmu_state[2] = rscc[kRsccSeqBusy / 4];
            native_gpu_gmu_state[3] = core[kGmuHfiStatus / 4];
        }
    }
    // D v<version> m<magic> p<parse ShengGmuDbStatus> s<build AdrenoHfiStartupStatus> x<GMxC present> then
    //   G<sysreset>/<tcm ret>/<rsc busy>/<hfi status>, or G- when the GMU clock is off
    char gmuText[32];
    if (gmu) snprintf(gmuText, sizeof gmuText, "G%x/%x/%x/%x", native_gpu_gmu_state[0], native_gpu_gmu_state[1],
                      native_gpu_gmu_state[2], native_gpu_gmu_state[3]);
    else snprintf(gmuText, sizeof gmuText, "G-");
    // Row 4 keeps the mode and the power summary and carries the GMU state, so a command-DB diagnostic in row 7 never hides it.
    snprintf(row, sizeof row, "GPU m%u io=%d pw%u%u %s", r.mode, r.io_error, r.powered_cx, r.powered_clocks, gmuText);
    setRow(4, row);
    if (parse != ShengGmuDbSuccess && parse != 9) {
        // H<slv_id>:<cnt> of the slave table (cmd_db_header.header[i], 16 bytes from offset 8) up to the first zero id
        int at = snprintf(gmuText, sizeof gmuText, "H");
        for (unsigned i = 0; i < 8 && at < static_cast<int>(sizeof gmuText) - 8; ++i) {
            uint16_t id, cnt;
            memcpy(&id, gCmdDb + 8 + 16 * i, 2);
            memcpy(&cnt, gCmdDb + 8 + 16 * i + 6, 2);
            if (!id) break;
            at += snprintf(gmuText + at, sizeof gmuText - static_cast<size_t>(at), "%x:%x ", id, cnt);
        }
    }
    if (gmuText[0] == 'H') snprintf(row, sizeof row, "D p%u %s", parse, gmuText);   // int-011 read v1 m0c0330db already
    else snprintf(row, sizeof row, "D v%x m%08x p%u s%u x%u %s", version, magic, parse, build, gVoteInputs.gmxc_present,
                  gmuText);
    setRow(7, row);
    native_gpu_power_stage = 0x5f;
    if (mode() >= 7) {
        const gpb_gmu_clock_on &o = native_gpu_gmu_clock_on;
        const bool idle = gmu && native_gpu_gmu_state[0] == 1 && !native_gpu_gmu_state[1] && !native_gpu_gmu_state[2] &&
                          !native_gpu_gmu_state[3];
        if (o.step == 7 && !o.failed && parse == ShengGmuDbSuccess && build == AdrenoHfiStartupSuccess && idle)
            gmuColdStart(r);
        else {
            snprintf(row, sizeof row, "S skip c%u/%u p%u s%u i%u", o.step, o.failed, parse, build, idle);
            setRow(7, row);
        }
    }
}
void probeThread(void *, wait_result_t) {
    registerExecObserver();
    unsigned mode = 0;
    PE_parse_boot_argn("sheng-gpu-power", &mode, sizeof mode);
    if (mode > 9) mode = 9;
    IOSleep(10000);
    native_gpu_power_stage = 1;
    unsigned have = 0;
    for (unsigned w = 0; w < 5; ++w) {
        IODeviceMemory *memory = IODeviceMemory::withRange(kPhys[w], kLen[w]);
        if (!memory) continue;
        gMaps[w] = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        memory->release();
        if (gMaps[w]) {gVa[w] = static_cast<uintptr_t>(gMaps[w]->getVirtualAddress()); have |= 1u << w;}
    }
    // Rows first show "mapped" so a fault in the sequence still leaves the stage on screen.
    char row[64];
    snprintf(row, sizeof row, "GPU m%u mapped=%02x running", mode, have);
    setRow(4, row);
    native_gpu_power_stage = 2;
    gpb_io io{nullptr, rd, wr, delayUs};
    gpb_run(&io, mode, have, &native_gpu_power_result);
    publish(native_gpu_power_result);
    native_gpu_power_stage = 3;
    if (mode >= 4) gmuContext(native_gpu_power_result);
    if (mode >= 5 && native_gpu_gmu_context.step == NativeAdrenoGmuContext::Ready && !native_gpu_gmu_context.status)
        gmuPrerequisites(native_gpu_power_result);
}
}

extern "C" void sheng_gpu_power_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("sheng-gpu-power", &mode, sizeof mode) || !mode || native_gpu_power_stage) return;
    native_gpu_power_stage = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(probeThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_gpu_power_stage = 0xdead;
}
