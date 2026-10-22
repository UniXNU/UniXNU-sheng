// SPDX-License-Identifier: BSD-2-Clause-Patent
// Display controller state on the sheng board (boot argument sheng-dpu=1): a read-only look at what the boot loader left running, so the
// display driver can take its refresh timing from the hardware and later flip between two scanout buffers.
// The devicetree facts: MDSS at 0xae00000 (qcom,sm8550-mdss), the DPU's MDP block at 0xae01000 (0x8f000 bytes, qcom,sm8550-dpu), dual DSI
// (dsi@ae94000 master, dsi@ae96000) to the NT36532E panel. DPU 9.2 catalog offsets inside the MDP block: interfaces 1 and 2 (the DSI
// controllers) at 0x35000 and 0x36000, CTL 0..3 at 0x15000 + 0x1000 * n, VIG pipes 0..3 at 0x4000 + 0x2000 * n, DMA pipes 0..5 at
// 0x24000 + 0x2000 * n. The block is only read while the boot loader's scanout keeps its clocks on (the screen shows the console).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

// [0] stage (1 mapped, 2 first sample, 3 second sample; negative: the step that failed), [1] MDP hardware version,
// per interface i (0, 1) at [2 + 8 * i]: timing engine enable, config, frame/line counter enable, frame count at the first sample,
// frame count one second later, line count, vsync period (lines), status; [18] microseconds between the two samples.
extern "C" {volatile uint32_t native_dpu_stats[20];}
// Per pipe (VIG 0..3, DMA 0..5): source size, source address 0, stride 0, source format; [40] the boot framebuffer's physical address
// (low 32 bits), [41] its high bits; per CTL 0..3 at [42 + 3 * n]: layer mixer 0 setting, flush, interface active.
extern "C" {volatile uint32_t native_dpu_pipes[54];}

namespace {
constexpr uint64_t kMdp = 0xae01000, kMdpBytes = 0x8f000;
constexpr uint32_t kIntf[2] = {0x35000, 0x36000};
constexpr uint32_t kTimingEnable = 0x6c, kConfig = 0x04, kCountEnable = 0xa8, kFrameCount = 0xac, kLineCount = 0xb0,
                   kVsyncPeriod = 0x0c, kStatus = 0x26c;
constexpr uint32_t kPipes[10] = {0x4000, 0x6000, 0x8000, 0xa000, 0x24000, 0x26000, 0x28000, 0x2a000, 0x2c000, 0x2e000};
constexpr uint32_t kSrcSize = 0x00, kSrc0 = 0x14, kStride0 = 0x24, kFormat = 0x30;
constexpr uint32_t kCtl = 0x15000, kCtlStride = 0x1000, kCtlLayer0 = 0x00, kCtlFlush = 0x18, kCtlIntfActive = 0xf4;

void dpuThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng-dpu");
    // After the render server has taken the display (the first desktop frame comes at about two minutes).
    IOSleep(150000);
    IODeviceMemory *memory = IODeviceMemory::withRange(kMdp, kMdpBytes);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    if (!map) {native_dpu_stats[0] = uint32_t(-1); return;}
    const volatile uint32_t *mdp = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    auto rd = [&](uint32_t offset) {return mdp[offset / 4];};
    native_dpu_stats[0] = 1;
    native_dpu_stats[1] = rd(0);
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t b = kIntf[i];
        volatile uint32_t *s = &native_dpu_stats[2 + 8 * i];
        s[0] = rd(b + kTimingEnable); s[1] = rd(b + kConfig); s[2] = rd(b + kCountEnable); s[3] = rd(b + kFrameCount);
        s[5] = rd(b + kLineCount); s[6] = rd(b + kVsyncPeriod); s[7] = rd(b + kStatus);
    }
    native_dpu_stats[0] = 2;
    const uint64_t began = mach_absolute_time();
    IOSleep(1000);
    uint64_t elapsed = 0;
    for (unsigned i = 0; i < 2; ++i) native_dpu_stats[2 + 8 * i + 4] = rd(kIntf[i] + kFrameCount);
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &elapsed);
    native_dpu_stats[18] = uint32_t(elapsed / 1000);
    for (unsigned p = 0; p < 10; ++p) {
        native_dpu_pipes[4 * p] = rd(kPipes[p] + kSrcSize); native_dpu_pipes[4 * p + 1] = rd(kPipes[p] + kSrc0);
        native_dpu_pipes[4 * p + 2] = rd(kPipes[p] + kStride0); native_dpu_pipes[4 * p + 3] = rd(kPipes[p] + kFormat);
    }
    PE_Video video = {};
    if (!PE_current_console(&video)) {native_dpu_pipes[40] = uint32_t(video.v_baseAddr); native_dpu_pipes[41] = uint32_t(uint64_t(video.v_baseAddr) >> 32);}
    for (unsigned c = 0; c < 4; ++c) {
        const uint32_t b = kCtl + c * kCtlStride;
        native_dpu_pipes[42 + 3 * c] = rd(b + kCtlLayer0); native_dpu_pipes[43 + 3 * c] = rd(b + kCtlFlush);
        native_dpu_pipes[44 + 3 * c] = rd(b + kCtlIntfActive);
    }
    native_dpu_stats[0] = 3;
    map->release();
}
}  // namespace

extern "C" void sheng_dpu_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("sheng-dpu", &enabled, sizeof enabled) || enabled != 1 || native_dpu_stats[0]) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(dpuThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_dpu_stats[0] = uint32_t(-7);
}
