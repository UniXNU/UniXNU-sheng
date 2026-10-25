// SPDX-License-Identifier: BSD-2-Clause-Patent
// Speaker playback on the sheng board (boot argument sheng-audio, bits: 1 serve, 2 play a test tone first, 4 bring up the microphone
// path, 8 press the power button once at 150 s for the lock sound; needs sheng-adsp=2). One kernel thread brings the path up in order:
//   1  pass-through stream mapping in the apps SMMU for the DSP's audio streams, and the DSP's period buffers
//   2  pins: secondary MI2S to the LPASS function, the two amplifier I2C buses, the shared amplifier reset (low 2 ms, then high)
//   3  QUP clocks and the two GENI I2C engines (QUP1 SE5, QUP2 SE0) at 400 kHz from the 19.2 MHz DFS level
//   4  the six CS35L43 amplifiers: identity and configuration (ShengCs35l43.c)
//   5  the audio DSP: "adsp_apps" packet router channel, framework readiness, speaker device and playback stream graphs
//      (ShengAudioReach.c), buffer mapping
//   6  the audio service (ShengAudioService.cpp) that iOS's audio server plug-in opens; with sheng-audio=2 a 750 Hz test tone plays first
//      for sheng-audio-tone-ms (default 1500), attenuated by sheng-audio-atten dB (default 30)
// Then the thread serves the plug-in: on start it powers the amplifiers up, prepares and starts both graphs and keeps four 10 ms
// periods queued at the DSP, each copied from the shared playback ring (ShengAudioAbi.h) as the previous one returns; on stop it stops
// the graphs and powers the amplifiers down. Once the microphone test passed, input requests start and stop the capture graphs the same
// way, and each returned capture period is copied into the shared capture ring. Waiting happens only on this thread: the DSP thread's
// callbacks copy each answer into a ring and wake it.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "ShengAdsp.h"
#include "ShengGpr.h"
#include "ShengAudioReach.h"
#include "ShengCs35l43.h"
#include "GeniI2c.h"
#include "TouchBoard.h"
#include "SmmuBypass.h"
#include "QcomRpmh.h"
#include "ShengAudioAbi.h"
}
#include <IOKit/IOService.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_audio_service_static_init(void) __asm__("__GLOBAL__sub_I_ShengAudioService.cpp");
extern "C" void sheng_audio_service_start(void);

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *) __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));
// [0] stage (see the list above, x10 for sub-steps, 60 serving; negative: the step that failed), [1] last DSP status, [2] DSP
// answers, [3] buffer map handle, [4] periods sent, [5] periods returned, [6] amplifiers identified (bit per amplifier),
// [7] amplifiers with the PLL locked, [8] amplifiers reporting errors, [9] SMMU result (bit per stream, or an error code << 8),
// [10] bus 0 engine (proto << 24 | clock config), [11] bus 1 engine, [12] last opcode received, [13] I2C failures,
// [14] output starts | stops << 16, [15] ms from start to serving
extern "C" {volatile int32_t native_audio_stats[16];}
// Per amplifier: device id, revision, EINT1, EINT2 while playing.
extern "C" {volatile uint32_t native_audio_amps[CS35_AMPS][4];}
// The microphone path (sheng-audio bit 2, after the speaker graphs): [0] stage (positive progress, negative the step that failed), [1] last PRM
// status, [2] codec rail (bit 0 found, bit 1 voted), [3] TX SoundWire master version, [4] its link status, [5] device status register
// (2 bits per device number), [6] device 1 id bytes 5..4 | 3..0 low word, [7] high word, [8] the codec's efuse register, [9] its chip
// id registers, [10] TX macro MCLK control, [11] VA macro MCLK control, [12] master interrupt status, [13] component parameters,
// [14] reads that timed out, [15] ms the stage took
extern "C" {volatile int32_t native_mic_stats[16];}
// After the TX master start: TX macro 0x0/0x4/0x8/0x90, VA macro 0x0/0x4/0x80, master 0x4/0x18/0x1044/0x104c/0x5060/0x5064/0x5000, the TX
// SoundWire clock pin configuration, the slew register
extern "C" {volatile uint32_t native_mic_regs[16];}
extern "C" {volatile uint64_t native_audio_pa;}   // the DSP period buffer

namespace {
constexpr uint32_t kIntentSize = 512, kIntentCount = 20, kRing = 16, kMessage = 128;
constexpr uint32_t kPort = 0x2001;                   // our GPR port; every answer comes back to it
constexpr uint32_t kRate = SHENG_AUDIO_RATE, kChannels = SHENG_AUDIO_OUT_CHANNELS, kPeriodFrames = SHENG_AUDIO_PERIOD_FRAMES;
constexpr uint32_t kPeriodBytes = kPeriodFrames * kChannels * 2, kSlots = 8, kBufferBytes = 64 * 1024, kRingFrames = SHENG_AUDIO_RING_FRAMES;
constexpr uint32_t kInFlight = 4, kLeadFrames = (kInFlight + 1) * kPeriodFrames;
// Capture periods (mono) live in the second half of the DSP buffer; the microphone graphs start in this order and stop in reverse.
constexpr uint32_t kCapBytes = kPeriodFrames * 2, kCapOffset = 32 * 1024, kCapSlots = 8;
const uint32_t kMicUp[2] = {AR_SG_MIC, AR_SG_CAPTURE}, kMicDown[2] = {AR_SG_CAPTURE, AR_SG_MIC};
constexpr uint64_t kGcc = 0x00100000ULL, kTlmm = 0x0f100000ULL, kSmmu = 0x15000000ULL;
constexpr uint64_t kSe[2] = {0x00a94000ULL, 0x00880000ULL};
// GCC (gcc-sm8550.c): vote registers and bits, branch halt registers, the engines' DFS clock sources.
constexpr uint32_t kVote1 = 0x52008, kVote1Bits = (1u << 18) | (1u << 19) | (1u << 20) | (1u << 21) | (1u << 27);
constexpr uint32_t kVote2 = 0x52010, kVote2Bits = 0x1fu;
constexpr uint32_t kHalts[] = {0x2327c, 0x23280, 0x23284, 0x23294, 0x1861c, 0x233cc, 0x233d0, 0x233d4, 0x23004, 0x1e004};
constexpr uint32_t kRcg[2] = {0x18628, 0x1e010};
// TLMM pins: secondary MI2S (function 1, 8 mA), I2C (function 2, pull-up), amplifier reset.
constexpr unsigned kI2sPins[] = {121, 122, 123, 124}, kI2cPins[] = {52, 53, 0, 1}, kResetPin = 90;
constexpr uint16_t kStreams[] = {0x1001, 0x1081, 0x1061};
// GENI I2C at 400 kHz from 19.2 MHz (i2c-qcom-geni.c): divider 2, high 5, low 11, cycle 22.
constexpr uint32_t kSclCounters = 5u << 20 | 11u << 10 | 22u, kClkCfg = 2u << 4 | 1u;
const int16_t kSine[64] = {0, 3212, 6393, 9512, 12539, 15446, 18204, 20787, 23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609,
    32767, 32609, 32137, 31356, 30273, 28898, 27245, 25329, 23170, 20787, 18204, 15446, 12539, 9512, 6393, 3212, 0, -3212, -6393, -9512,
    -12539, -15446, -18204, -20787, -23170, -25329, -27245, -28898, -30273, -31356, -32137, -32609, -32767, -32609, -32137, -31356, -30273,
    -28898, -27245, -25329, -23170, -20787, -18204, -15446, -12539, -9512, -6393, -3212};

struct Region {OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0;};
bool mapRegion(uint64_t phys, uint64_t length, Region &region, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | cache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}
uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
void cacheClean(const void *p, size_t len) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const size_t line = 4u << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1), end = reinterpret_cast<uintptr_t>(p) + len; a < end; a += line)
        __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}
uint32_t smmuRd(void *ctx, unsigned o) {return rd32(reinterpret_cast<uintptr_t>(ctx), o);}
void smmuWr(void *ctx, unsigned o, uint32_t v) {wr32(reinterpret_cast<uintptr_t>(ctx), o, v);}
int32_t smmuErrorCode(const char *e) {
    static const char *const names[] = {"SMMU AArch64 stage 1 unsupported", "DMA exceeds SMMU address width", "unsupported SMMU geometry",
        "conflicting stream mapping", "reserved context bank is not a bypass bank", "no free stream-match group",
        "SMMU bypass bank readback", "SMMU stream context readback", "SMMU stream mapping readback", "no free context bank", "bad window",
        "SMMU window bank readback", "SMMU window stream readback"};
    for (unsigned i = 0; i < sizeof names / sizeof *names; ++i) if (strcmp(e, names[i]) == 0) return static_cast<int32_t>(i) + 1;
    return 99;
}
uint64_t nowMs() {uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
void fail(int step) {native_audio_stats[0] = -step;}
void stage(int step) {native_audio_stats[0] = step;}

struct Audio {
    Region gccLow, gcc18, gcc1e, gcc23, gccVote, smmu, se[2];
    Region pins[16];
    unsigned pinCount;
    GeniI2c bus[2];
    OSSharedPtr<IOBufferMemoryDescriptor> buffer;
    uint8_t *bufferVa;
    uint64_t bufferPa;
    Region low;
    uint64_t dspAddr;   // the buffer as the DSP takes it
    ShengAdspChannel *channel;
    IOLock *lock;
    IOSimpleLock *ringLock;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    uint32_t ringHead, ringCount, token, handle;
    volatile uint32_t up;
    uint32_t toneMs, atten, mode;
    // The shared memory of the service (ShengAudioAbi.h).
    OSSharedPtr<IOBufferMemoryDescriptor> control, outRing, inRing;
    ShengAudioControl *ctl;
    int16_t *out;
    volatile uint32_t want;            // ShengAudioOutputRunning / ShengAudioInputRunning as the plug-in asks
    volatile uint32_t serving;         // the service accepts requests
    bool running, statusRead;
    uint64_t next, done;
    uint32_t inFlight;
    // Capture for the plug-in's input stream, once the microphone test passed: periods read from the DSP land in the capture ring at
    // capBase + n * kPeriodFrames, capBase being the playback position when capture started (both directions share one clock).
    int16_t *in;
    bool micReady, capturing;
    uint64_t capNext, capDone, capBase;
    uint32_t capInFlight;
};
Audio gAudio;

// DSP thread: copy and wake, nothing else.
void dspState(void *, int up) {
    __atomic_store_n(&gAudio.up, up ? 1u : 0u, __ATOMIC_RELEASE);
    IOLockWakeup(gAudio.lock, &gAudio.ringCount, false);
}
void dspReceive(void *, const uint8_t *data, uint32_t length) {
    IOSimpleLockLock(gAudio.ringLock);
    if (gAudio.ringCount < kRing) {
        auto &slot = gAudio.ring[(gAudio.ringHead + gAudio.ringCount) % kRing];
        slot.length = length < kMessage ? length : kMessage;
        memcpy(slot.data, data, slot.length);
        gAudio.ringCount++;
    }
    IOSimpleLockUnlock(gAudio.ringLock);
    IOLockWakeup(gAudio.lock, &gAudio.ringCount, false);
}
// The next answer, waiting up to ms; false on timeout.
bool nextAnswer(ArAnswer &a, uint32_t ms) {
    const uint64_t end = nowMs() + ms;
    for (;;) {
        uint8_t data[kMessage];
        uint32_t length = 0;
        IOSimpleLockLock(gAudio.ringLock);
        if (gAudio.ringCount) {
            auto &slot = gAudio.ring[gAudio.ringHead];
            length = slot.length;
            memcpy(data, slot.data, length);
            gAudio.ringHead = (gAudio.ringHead + 1) % kRing;
            gAudio.ringCount--;
        }
        IOSimpleLockUnlock(gAudio.ringLock);
        if (length) {
            if (ar_parse(data, length, &a) == ArBad) continue;
            native_audio_stats[2] = native_audio_stats[2] + 1;
            native_audio_stats[12] = (int32_t)a.opcode;
            return true;
        }
        const uint64_t now = nowMs();
        if (now >= end) return false;
        uint64_t deadline;
        clock_interval_to_deadline((uint32_t)(end - now), kMillisecondScale, &deadline);
        IOLockLock(gAudio.lock);
        if (!gAudio.ringCount) IOLockSleepDeadline(gAudio.lock, &gAudio.ringCount, deadline, THREAD_UNINT);
        IOLockUnlock(gAudio.lock);
    }
}
bool send(const uint8_t *packet, uint32_t length) {return length && sheng_adsp_send(gAudio.channel, packet, length) == 0;}
// Sends an APM command and waits for its basic result (or, for the map, the handle).
bool command(uint32_t length, const uint8_t *packet, uint32_t opcode) {
    if (!send(packet, length)) return false;
    ArAnswer a;
    const uint64_t end = nowMs() + 2000;
    while (nowMs() < end) {
        if (!nextAnswer(a, 2000)) break;
        if (a.opcode == AR_OP_MEM_MAP_DONE && opcode == AR_OP_MEM_MAP) {gAudio.handle = a.handle; native_audio_stats[3] = (int32_t)a.handle; return true;}
        if (a.opcode == GPR_OP_BASIC_RESULT && a.result_opcode == opcode) {native_audio_stats[1] = (int32_t)a.status; return a.status == 0;}
        if ((a.opcode == AR_OP_PRM_REQUEST_DONE && opcode == AR_OP_PRM_REQUEST) || (a.opcode == AR_OP_PRM_RELEASE_DONE && opcode == AR_OP_PRM_RELEASE)) {
            native_mic_stats[1] = (int32_t)a.status;
            return a.status == 0;
        }
    }
    native_audio_stats[1] = -60;
    return false;
}

// One I2C transfer on bus b, waiting by polling (20 us steps, 20 ms per stage of the timeout ladder).
bool transfer(unsigned b, uint8_t address, const Cs35Action &act, uint32_t *value) {
    GeniI2c &g = gAudio.bus[b];
    g.address = address;
    const int started = act.read_length ? geni_i2c_start_read_bytes(&g, act.bytes, act.length, act.read_length)
                                        : geni_i2c_start_write_bytes(&g, act.bytes, act.length);
    if (started) return false;
    for (int stageLeft = 3; stageLeft > 0; ) {
        for (unsigned i = 0; i < 1000; ++i) {
            const int r = geni_i2c_poll(&g);
            if (r == GeniDone) {if (value) *value = cs35_wire_value(g.rx); return true;}
            if (r == GeniFailed) {native_audio_stats[13] = native_audio_stats[13] + 1; return false;}
            IODelay(20);
        }
        const int r = geni_i2c_timeout(&g);
        if (r != GeniPending) break;
        --stageLeft;
    }
    native_audio_stats[13] = native_audio_stats[13] + 1;
    return false;
}
// Runs a program on one amplifier; false on the first failed transfer.
bool runProgram(unsigned amp, const Cs35Step *steps, unsigned count, Cs35Run &run) {
    const unsigned b = amp / 3;
    const uint8_t address = (uint8_t)(0x40 + amp % 3);
    cs35_run_start(&run, steps, count);
    for (;;) {
        Cs35Action act;
        const int k = cs35_run_next(&run, &act);
        if (k == Cs35Finished) return true;
        if (k == Cs35Wait) {IOSleep(act.delay_ms); continue;}
        uint32_t value = 0;
        if (!transfer(b, address, act, &value)) return false;
        cs35_run_result(&run, value);
    }
}

// The DSP's audio streams in pass-through (the bypass bank the storage and touch drivers share). Programming a translating context for
// them reset the board every time (i496 to i502, whatever bank or table): the hypervisor does not let the kernel translate them.
bool setupSmmu() {
    if (!mapRegion(kSmmu, TB_APPS_SMMU_SIZE, gAudio.smmu)) return false;
    struct smmu_io io = {reinterpret_cast<void *>(gAudio.smmu.va), smmuRd, smmuWr};
    int32_t result = 0;
    for (unsigned i = 0; i < sizeof kStreams / sizeof *kStreams; ++i) {
        struct smmu_plan plan = smmu_plan_bypass(&io, kStreams[i], gAudio.bufferPa, kBufferBytes);
        const char *e = plan.error ? plan.error : smmu_configure_bypass(&io, &plan, kStreams[i]);
        if (!e) result |= 1 << i;
        else if (!(result >> 8)) result |= smmuErrorCode(e) << 8;
    }
    native_audio_stats[9] = result;
    return (result & 1) != 0;
}

// The DSP takes 32-bit buffer addresses (its client hands it 32-bit IOVAs; i491 to i495: a buffer above 4 GiB was refused with status 1),
// and every page the kernel owns lies above 4 GiB. The buffer is 64 KiB of low DRAM the UEFI memory map leaves conventional and nothing
// uses after boot (the gap 0xCCF80000 to 0xD8000000; sheng-audio-pa moves it), mapped write-combined (normal non-cacheable: memset and
// memcpy need normal memory). The DSP takes it as address | stream index << 32: index 1 of the audio streams (0x1001 & 0xf), as its
// client sends it; i491 to i495 sent a 36-bit address, whose upper word the DSP read as stream 8 and refused with status 1.
constexpr uint64_t kLowBuffer = 0xd0000000ULL;
bool setupBuffer() {
    uint64_t pa = kLowBuffer;
    PE_parse_boot_argn("sheng-audio-pa", &pa, sizeof pa);
    if ((pa & 0xfff) || pa + kBufferBytes > 0x100000000ULL || !mapRegion(pa, kBufferBytes, gAudio.low, kIOMapWriteCombineCache)) return false;
    gAudio.bufferPa = pa;
    gAudio.dspAddr = pa | 1ull << 32;
    gAudio.bufferVa = reinterpret_cast<uint8_t *>(gAudio.low.va);
    native_audio_pa = gAudio.bufferPa;
    memset(gAudio.bufferVa, 0, kBufferBytes);
    return true;
}

bool setupPins() {
    auto pin = [](unsigned gpio) -> uintptr_t {
        if (gAudio.pinCount >= 16 || !mapRegion(kTlmm + tb_tlmm_ctl(gpio), 0x1000, gAudio.pins[gAudio.pinCount])) return 0;
        return gAudio.pins[gAudio.pinCount++].va;
    };
    for (unsigned g : kI2sPins) {uintptr_t p = pin(g); if (!p) return false; wr32(p, 0, 1u << 2 | 3u << 6);}
    for (unsigned g : kI2cPins) {uintptr_t p = pin(g); if (!p) return false; wr32(p, 0, 2u << 2 | 0u << 6 | 3u);}
    uintptr_t reset = pin(kResetPin);
    if (!reset) return false;
    wr32(reset, 4, 0);                       // output low
    wr32(reset, 0, 1u << 9);                 // GPIO, output enabled, 2 mA, no pull
    IOSleep(2);
    wr32(reset, 4, 1u << 1);                 // release
    IOSleep(3);
    return true;
}

bool setupClocks() {
    if (!mapRegion(kGcc, 0x1000, gAudio.gccLow) || !mapRegion(kGcc + 0x18000, 0x1000, gAudio.gcc18) ||
        !mapRegion(kGcc + 0x1e000, 0x1000, gAudio.gcc1e) || !mapRegion(kGcc + 0x23000, 0x1000, gAudio.gcc23) ||
        !mapRegion(kGcc + 0x52000, 0x1000, gAudio.gccVote)) return false;
    wr32(gAudio.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff, rd32(gAudio.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfff) | 1u);
    for (unsigned i = 0; i < 10000 && !(rd32(gAudio.gccLow.va, TB_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);
    wr32(gAudio.gccVote.va, kVote1 & 0xfff, rd32(gAudio.gccVote.va, kVote1 & 0xfff) | kVote1Bits);
    wr32(gAudio.gccVote.va, kVote2 & 0xfff, rd32(gAudio.gccVote.va, kVote2 & 0xfff) | kVote2Bits);
    auto halt = [](uint32_t reg) -> uint32_t {
        const uintptr_t page = (reg >> 12) == 0x18 ? gAudio.gcc18.va : (reg >> 12) == 0x1e ? gAudio.gcc1e.va : gAudio.gcc23.va;
        return rd32(page, reg & 0xfff);
    };
    bool ok = false;
    for (unsigned i = 0; i < 2000 && !ok; ++i) {
        ok = true;
        for (uint32_t reg : kHalts) {const uint32_t v = halt(reg); if ((v & TB_CBCR_CLK_OFF) && !(v & 2u)) ok = false;}
        if (!ok) IODelay(10);
    }
    return ok;   // never touch an engine without running clocks
}

// Brings one engine to I2C at 400 kHz if the boot loader left it unconfigured, then hands it to the transfer core.
bool setupBus(unsigned b) {
    if (!mapRegion(kSe[b], TB_SE_SIZE, gAudio.se[b])) return false;
    const uintptr_t se = gAudio.se[b].va;
    const uintptr_t rcg = (b == 0 ? gAudio.gcc18.va : gAudio.gcc1e.va) + (kRcg[b] & 0xfff);
    unsigned level = 0xff;
    for (unsigned l = 0; l < TB_DFS_LEVELS && level == 0xff; ++l) {
        const uint64_t hz = tb_dfs_level_hz(rd32(rcg, TB_RCG_PERF_CFG(l)), rd32(rcg, TB_RCG_PERF_M(l)), rd32(rcg, TB_RCG_PERF_N(l)));
        if (hz == 19200000ULL) level = l;
    }
    const uint32_t proto = (rd32(se, GENI_FW_REVISION) >> 8) & 0xff;
    native_audio_stats[10 + b] = (int32_t)(proto << 24 | (rd32(se, GENI_CLK_CFG) & 0xffffff));
    if (proto != GENI_PROTO_I2C) return false;
    if (!(rd32(se, GENI_CLK_CFG) & 1u) || !rd32(se, GENI_SCL_COUNTERS)) {
        if (level == 0xff) return false;
        wr32(se, 0x28, rd32(se, 0x28) | 0x7fu);             // clock gating controls on
        wr32(se, 0xe30, rd32(se, 0xe30) | 0xfu);            // DMA clock gating controls on
        wr32(se, 0x24, 0x7fu);                              // default output control
        wr32(se, 0x20, 1u);                                 // force default
        wr32(se, 0xe18, 0);                                 // no GSI events
        wr32(se, 0x7c, level);
        wr32(se, GENI_CLK_CFG, kClkCfg);
        wr32(se, GENI_SCL_COUNTERS, kSclCounters);
    }
    const GeniIo io = {ioRd, ioWr, reinterpret_cast<void *>(se)};
    uint32_t config = 0;
    if (geni_i2c_prepare(&gAudio.bus[b], &io, 0x40, &config)) return false;
    native_audio_stats[10 + b] = (int32_t)config;
    return true;
}

bool setupAmps() {
    Cs35Step steps[CS35_MAX_STEPS];
    Cs35Run run;
    int32_t found = 0;
    for (unsigned amp = 0; amp < CS35_AMPS; ++amp) {
        const unsigned n = cs35_init_program(steps, cs35_slot(amp));
        if (!runProgram(amp, steps, n, run)) continue;
        native_audio_amps[amp][0] = run.reads[0];
        native_audio_amps[amp][1] = run.reads[1];
        found |= 1 << amp;
    }
    native_audio_stats[6] = found;
    return found != 0;
}

void readAmpStatus() {
    Cs35Step steps[CS35_MAX_STEPS];
    Cs35Run run;
    int32_t locked = 0, errors = 0;
    for (unsigned amp = 0; amp < CS35_AMPS; ++amp) {
        if (!(native_audio_stats[6] & (1 << amp))) continue;
        const unsigned n = cs35_status_program(steps);
        if (!runProgram(amp, steps, n, run)) continue;
        native_audio_amps[amp][2] = run.reads[0];
        native_audio_amps[amp][3] = run.reads[1];
        if (run.reads[1] & CS35_EINT2_PLL_LOCK) locked |= 1 << amp;
        if (run.reads[0] & CS35_EINT1_ERRORS) errors |= 1 << amp;
    }
    native_audio_stats[7] = locked;
    native_audio_stats[8] = errors;
}

void powerDownAmps() {
    Cs35Step steps[CS35_MAX_STEPS];
    Cs35Run run;
    for (unsigned amp = 0; amp < CS35_AMPS; ++amp) {
        if (!(native_audio_stats[6] & (1 << amp))) continue;
        runProgram(amp, steps, cs35_power_down_program(steps), run);
    }
}

bool waitUp(uint32_t ms) {
    const uint64_t end = nowMs() + ms;
    while (!__atomic_load_n(&gAudio.up, __ATOMIC_ACQUIRE)) {
        if (nowMs() >= end) return false;
        IOSleep(10);
    }
    return true;
}

// Asks until the framework manager says ready (status 1), 100 ms apart, at most 100 times.
bool dspReady() {
    uint8_t b[GPR_MAX_PACKET];
    for (unsigned query = 0; query < 100; ++query) {
        const GprPacket q = {.dst_domain = GPR_DOMAIN_ADSP, .src_domain = GPR_DOMAIN_APPS, .src_port = kPort, .dst_port = GPR_PORT_APM,
                             .token = ++gAudio.token, .opcode = GPR_OP_APM_GET_SPF_STATE};
        if (send(b, gpr_encode(b, sizeof b, &q, nullptr, 0))) {
            const uint64_t end = nowMs() + 1000;
            ArAnswer a;
            while (nowMs() < end && nextAnswer(a, 1000)) {
                if (a.opcode == GPR_OP_APM_SPF_STATE) {native_audio_stats[1] = (int32_t)a.status; if (a.status == 1) return true; break;}
                if (a.opcode == GPR_OP_BASIC_RESULT && a.result_opcode == GPR_OP_APM_GET_SPF_STATE) {native_audio_stats[1] = (int32_t)a.status; break;}
            }
        }
        IOSleep(100);
    }
    fail(51);
    return false;
}

bool setupShared() {
    const IOOptionBits shared = static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryKernelUserShared);
    gAudio.control = OSSharedPtr<IOBufferMemoryDescriptor>(IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, 4096, 4096), OSNoRetain);
    gAudio.outRing = OSSharedPtr<IOBufferMemoryDescriptor>(IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared,
        kRingFrames * SHENG_AUDIO_OUT_CHANNELS * 2, 4096), OSNoRetain);
    gAudio.inRing = OSSharedPtr<IOBufferMemoryDescriptor>(IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared,
        kRingFrames * SHENG_AUDIO_IN_CHANNELS * 2, 4096), OSNoRetain);
    if (!gAudio.control || !gAudio.outRing || !gAudio.inRing) return false;
    gAudio.ctl = static_cast<ShengAudioControl *>(gAudio.control->getBytesNoCopy());
    gAudio.out = static_cast<int16_t *>(gAudio.outRing->getBytesNoCopy());
    gAudio.in = static_cast<int16_t *>(gAudio.inRing->getBytesNoCopy());
    memset(gAudio.ctl, 0, 4096);
    memset(gAudio.out, 0, kRingFrames * SHENG_AUDIO_OUT_CHANNELS * 2);
    memset(gAudio.inRing->getBytesNoCopy(), 0, kRingFrames * SHENG_AUDIO_IN_CHANNELS * 2);
    mach_timebase_info_data_t tb;
    clock_timebase_info(&tb);
    ShengAudioControl *c = gAudio.ctl;
    c->rate = kRate; c->out_channels = SHENG_AUDIO_OUT_CHANNELS; c->in_channels = SHENG_AUDIO_IN_CHANNELS;
    c->period_frames = kPeriodFrames; c->ring_frames = kRingFrames; c->lead_frames = kLeadFrames;
    c->ticks_per_frame_q32 = ((uint64_t)1000000000ULL * tb.denom << 32) / ((uint64_t)kRate * tb.numer);
    c->version = SHENG_AUDIO_VERSION;
    __atomic_store_n(&c->magic, SHENG_AUDIO_MAGIC, __ATOMIC_RELEASE);
    return true;
}

// Sends period n from the playback ring through DSP slot n % kSlots.
bool submit(uint64_t n) {
    uint8_t b[GPR_MAX_PACKET];
    const uint32_t slot = (uint32_t)(n % kSlots);
    int16_t *dst = reinterpret_cast<int16_t *>(gAudio.bufferVa + slot * kPeriodBytes);
    const uint32_t at = (uint32_t)((n * kPeriodFrames) % kRingFrames);
    memcpy(dst, gAudio.out + at * kChannels, kPeriodBytes);   // kRingFrames is a multiple of kPeriodFrames' alignment step
    cacheClean(dst, kPeriodBytes);
    if (!send(b, ar_write(b, sizeof b, kPort, (uint32_t)n, gAudio.dspAddr + slot * kPeriodBytes, gAudio.handle, kPeriodBytes))) return false;
    native_audio_stats[4] = native_audio_stats[4] + 1;
    gAudio.inFlight++;
    return true;
}

bool graphs(uint32_t opcode) {
    uint8_t b[GPR_MAX_PACKET];
    const uint32_t first = opcode == AR_OP_GRAPH_STOP ? AR_SG_PLAY : AR_SG_SPEAKER, second = first == AR_SG_PLAY ? AR_SG_SPEAKER : AR_SG_PLAY;
    const bool a = command(ar_graph_command(b, sizeof b, kPort, ++gAudio.token, opcode, first), b, opcode);
    const bool c = command(ar_graph_command(b, sizeof b, kPort, ++gAudio.token, opcode, second), b, opcode);
    return a && c;
}

void powerUpAmps() {
    Cs35Step steps[CS35_MAX_STEPS];
    Cs35Run run;
    for (unsigned amp = 0; amp < CS35_AMPS; ++amp)
        if (native_audio_stats[6] & (1 << amp)) runProgram(amp, steps, cs35_power_up_program(steps), run);
}

void startOutput() {
    if (gAudio.running) return;
    powerUpAmps();
    if (!graphs(AR_OP_GRAPH_PREPARE) || !graphs(AR_OP_GRAPH_START)) {powerDownAmps(); return;}
    ShengAudioControl *c = gAudio.ctl;
    uint64_t lead;
    nanoseconds_to_absolutetime((uint64_t)kLeadFrames * 1000000000ULL / kRate, &lead);
    __atomic_add_fetch(&c->out_seq, 1, __ATOMIC_ACQ_REL);
    c->out_anchor_host = mach_absolute_time() + lead;
    c->out_frames = 0;
    __atomic_add_fetch(&c->out_seq, 1, __ATOMIC_ACQ_REL);
    gAudio.next = gAudio.done = 0;
    gAudio.inFlight = 0;
    gAudio.statusRead = false;
    gAudio.running = true;
    while (gAudio.inFlight < kInFlight && submit(gAudio.next)) gAudio.next++;
    __atomic_or_fetch(&c->state, ShengAudioOutputRunning, __ATOMIC_ACQ_REL);
    native_audio_stats[14] = native_audio_stats[14] + 1;
}

void stopOutput() {
    if (!gAudio.running) return;
    gAudio.running = false;
    graphs(AR_OP_GRAPH_STOP);
    powerDownAmps();
    __atomic_and_fetch(&gAudio.ctl->state, ~ShengAudioOutputRunning, __ATOMIC_ACQ_REL);
    native_audio_stats[14] = native_audio_stats[14] + (1 << 16);
}

extern "C" {extern volatile int32_t native_mic_capture[8];}
// Asks the DSP to fill capture period n.
bool askCapture(uint64_t n) {
    uint8_t b[GPR_MAX_PACKET];
    const uint32_t slot = (uint32_t)(n % kCapSlots);
    if (!send(b, ar_read(b, sizeof b, kPort, (uint32_t)n, gAudio.dspAddr + kCapOffset + slot * kCapBytes, gAudio.handle, kCapBytes))) return false;
    gAudio.capInFlight++;
    return true;
}

void startInput() {
    if (gAudio.capturing || !gAudio.micReady) return;
    uint8_t b[GPR_MAX_PACKET];
    for (uint32_t sg : kMicUp) command(ar_graph_command(b, sizeof b, kPort, ++gAudio.token, AR_OP_GRAPH_PREPARE, sg), b, AR_OP_GRAPH_PREPARE);
    for (uint32_t sg : kMicUp)
        if (!command(ar_graph_command(b, sizeof b, kPort, ++gAudio.token, AR_OP_GRAPH_START, sg), b, AR_OP_GRAPH_START)) return;
    ShengAudioControl *c = gAudio.ctl;
    const uint64_t now = mach_absolute_time(), anchor = c->out_anchor_host;
    const double ticks = (double)c->ticks_per_frame_q32 / 4294967296.0;
    gAudio.capBase = gAudio.running && now > anchor ? (uint64_t)((double)(now - anchor) / ticks) : 0;
    __atomic_add_fetch(&c->in_seq, 1, __ATOMIC_ACQ_REL);
    c->in_anchor_host = anchor;
    c->in_frames = 0;
    __atomic_add_fetch(&c->in_seq, 1, __ATOMIC_ACQ_REL);
    memset(gAudio.in, 0, kRingFrames * 2);
    gAudio.capNext = gAudio.capDone = 0;
    gAudio.capInFlight = 0;
    gAudio.capturing = true;
    while (gAudio.capInFlight < kInFlight && askCapture(gAudio.capNext)) gAudio.capNext++;
    __atomic_or_fetch(&c->state, ShengAudioInputRunning, __ATOMIC_ACQ_REL);
    native_audio_stats[14] = native_audio_stats[14] + (1 << 8);
}

void stopInput() {
    if (!gAudio.capturing) return;
    gAudio.capturing = false;
    uint8_t b[GPR_MAX_PACKET];
    for (uint32_t sg : kMicDown) command(ar_graph_command(b, sizeof b, kPort, ++gAudio.token, AR_OP_GRAPH_STOP, sg), b, AR_OP_GRAPH_STOP);
    __atomic_and_fetch(&gAudio.ctl->state, ~ShengAudioInputRunning, __ATOMIC_ACQ_REL);
    native_audio_stats[14] = native_audio_stats[14] + (1 << 24);
}

// A returned capture period goes to the capture ring at its place in the playback timeline.
void captured(const ArAnswer &a) {
    if (gAudio.capInFlight) gAudio.capInFlight--;
    const uint32_t slot = (uint32_t)((a.address - (gAudio.dspAddr + kCapOffset)) / kCapBytes) % kCapSlots;
    const int16_t *src = reinterpret_cast<const int16_t *>(gAudio.bufferVa + kCapOffset + slot * kCapBytes);
    const uint64_t t = gAudio.capBase + gAudio.capDone * kPeriodFrames;
    for (uint32_t i = 0; i < kPeriodFrames; ++i) gAudio.in[(t + i) % kRingFrames] = src[i];
    gAudio.capDone++;
    gAudio.ctl->in_frames = gAudio.capDone * kPeriodFrames;
    native_mic_capture[3] = native_mic_capture[3] + 1;
    if (gAudio.capturing) while (gAudio.capInFlight < kInFlight && askCapture(gAudio.capNext)) gAudio.capNext++;
}

void handle(const ArAnswer &a) {
    if (a.opcode == AR_OP_WRITE_DONE) {
        native_audio_stats[5] = native_audio_stats[5] + 1;
        if (a.status) native_audio_stats[1] = (int32_t)a.status;
        if (gAudio.inFlight) gAudio.inFlight--;
        gAudio.done++;
        gAudio.ctl->out_frames = gAudio.done * kPeriodFrames;
        if (gAudio.running) while (gAudio.inFlight < kInFlight && submit(gAudio.next)) gAudio.next++;
        if (gAudio.running && !gAudio.statusRead && gAudio.done == 10) {readAmpStatus(); gAudio.statusRead = true;}
    } else if (a.opcode == AR_OP_READ_DONE) {
        captured(a);
    } else if (a.opcode == GPR_OP_BASIC_RESULT && a.status) {
        native_audio_stats[1] = (int32_t)a.status;
    }
}

// Serves requests and answers until the end of time; with ms > 0 it returns after ms (the test tone).
void serve(uint32_t ms) {
    const uint64_t end = ms ? nowMs() + ms : 0;
    for (;;) {
        const uint32_t want = __atomic_load_n(&gAudio.want, __ATOMIC_ACQUIRE);
        if (!ms) {
            if ((want & ShengAudioOutputRunning) && !gAudio.running) startOutput();
            if (!(want & ShengAudioOutputRunning) && gAudio.running) stopOutput();
            if ((want & ShengAudioInputRunning) && !gAudio.capturing) startInput();
            if (!(want & ShengAudioInputRunning) && gAudio.capturing) stopInput();
        }
        ArAnswer a;
        if (nextAnswer(a, gAudio.running || gAudio.capturing ? 100 : 1000)) handle(a);
        if (end && nowMs() >= end) return;
    }
}

void playTone() {
    const unsigned shift = gAudio.atten / 6 > 15 ? 15 : gAudio.atten / 6;
    for (uint32_t i = 0; i < kRingFrames; ++i) gAudio.out[2 * i] = gAudio.out[2 * i + 1] = (int16_t)(kSine[i % 64] >> shift);
    startOutput();
    serve(gAudio.toneMs);
    stopOutput();
    memset(gAudio.out, 0, kRingFrames * kChannels * 2);
    stage(57);
}

// sheng-audio bit 8: an unattended check of the iOS output path. At 150 s of uptime one synthetic power button press locks the screen,
// and SpringBoard plays its lock sound through the audio server, the plug-in and this service; [audio-service] then counts the requests.
extern "C" bool sheng_buttons_press(uint32_t mask, uint32_t ms) __attribute__((weak_import));
extern "C" volatile int32_t native_audio_service[8];
void lockSoundThread(void *, wait_result_t) {
    uint64_t up = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &up);
    const uint64_t at = 150ull * 1000000000ull;
    if (up < at) IOSleep((uint32_t)((at - up) / 1000000ull));
    native_audio_service[6] = sheng_buttons_press && sheng_buttons_press(1u << 1, 150) ? 1 : -1;
}

bool openGraphs() {
    uint8_t b[GPR_MAX_PACKET];
    const ArFormat f = {kRate, 16, kChannels};
    if (!command(ar_open_speaker_device(b, sizeof b, kPort, ++gAudio.token), b, AR_OP_GRAPH_OPEN)) {fail(54); return false;}
    if (!command(ar_config_speaker_device(b, sizeof b, kPort, ++gAudio.token, &f), b, AR_OP_SET_CFG)) {fail(55); return false;}
    if (!command(ar_open_playback_stream(b, sizeof b, kPort, ++gAudio.token), b, AR_OP_GRAPH_OPEN)) {fail(56); return false;}
    if (!command(ar_config_playback_stream(b, sizeof b, kPort, ++gAudio.token, &f), b, AR_OP_SET_CFG)) {fail(57); return false;}
    if (!command(ar_config_playback_shmem(b, sizeof b, kPort, ++gAudio.token, &f), b, AR_OP_SET_CFG)) {fail(58); return false;}
    if (!command(ar_map(b, sizeof b, kPort, ++gAudio.token, gAudio.dspAddr, kBufferBytes), b, AR_OP_MEM_MAP)) {fail(53); return false;}
    return true;
}

// ---- the microphones (sheng-audio bit 2) ----
// The codec (WCD938x) sits on the TX SoundWire master of the low power audio subsystem. Every LPASS register below (pins, VA and TX
// macros, SoundWire) is unclocked until the DSP's PRM grants the macro and digital codec votes: they are touched only after those succeed.
constexpr uint64_t kLpassTlmm = 0x06e80000ULL, kLpassSlew = 0x07250000ULL + 0xa000, kTxMacro = 0x06ae0000ULL, kVaMacro = 0x06d44000ULL,
    kSwrTx = 0x06d30000ULL;
constexpr unsigned kWcdResetPin = 108;
void micStage(int s) {native_mic_stats[0] = s;}
void micFail(int s) {native_mic_stats[0] = -s;}

// The codec's 1.8 V rail (vreg_l15b_1p8) through active TCS 2 of the apps RSC (0: the WiFi bring-up, 1: the DSP's proxy votes).
bool micRail() {
    static uint8_t db[0x20000];
    Region dbw, rsc;
    if (!mapRegion(0x81c60000ULL, sizeof db, dbw)) return false;
    for (size_t i = 0; i < sizeof db; i += 4) {const uint32_t w = rd32(dbw.va, (uint32_t)i); memcpy(db + i, &w, 4);}
    QcomRpmhResource res;
    if (qcom_cmd_db_find(db, sizeof db, "ldob15", &res)) return false;
    native_mic_stats[2] = 1;
    if (!mapRegion(0x17a20000ULL, 0x10000, rsc)) return false;
    const QcomRscLayout layout = qcom_rsc_layout(rd32(rsc.va, 0));
    auto send = [&](uint32_t address, uint32_t value) {
        QcomRpmhWrite writes[16];
        const unsigned n = qcom_rpmh_active_request(&layout, 2, &address, &value, 1, writes, 16);
        for (unsigned i = 0; i < n; ++i) wr32(rsc.va, 0xd00 + writes[i].offset, writes[i].value);
        IOSleep(2);
        const unsigned m = qcom_rpmh_finish(&layout, 2, writes, 16);
        for (unsigned i = 0; i < m; ++i) wr32(rsc.va, 0xd00 + writes[i].offset, writes[i].value);
    };
    send(res.address + 8, 7);       // high power mode
    send(res.address, 1800);
    send(res.address + 4, 1);
    native_mic_stats[2] = 3;
    return true;
}

// The PRM commands go out from the PRM's own port, as its client sends them (i504: from our port the first vote was refused with status 1).
// [1] the three results: status of the macro vote | digital codec vote << 8 | clock request << 16 (0xff: no answer).
bool micPrm() {
    uint8_t b[GPR_MAX_PACKET];
    uint32_t statuses = 0;
    auto run = [&](uint32_t n) {
        native_mic_stats[1] = -1;
        const bool done = command(n, b, AR_OP_PRM_REQUEST);
        return done ? 0u : native_mic_stats[1] == -1 ? 0xffu : ((uint32_t)native_mic_stats[1] & 0xffu ? (uint32_t)native_mic_stats[1] & 0xffu : 0xfeu);
    };
    statuses |= run(ar_prm_vote(b, sizeof b, AR_PRM_IID, ++gAudio.token, AR_HW_MACRO, 1));
    statuses |= run(ar_prm_vote(b, sizeof b, AR_PRM_IID, ++gAudio.token, AR_HW_DCODEC, 1)) << 8;
    // The TX core clock: 19.2 MHz "couple no" as the macro drivers ask, else 9.6 MHz, else attribute 0 (i505 refused the first with 1).
    // [1] bits 31:24 say which one the DSP took (1 to 3, 0 none).
    static const struct {uint32_t hz, attribute;} clocks[] = {{19200000, 1}, {9600000, 1}, {19200000, 0}};
    uint32_t taken = 0, clock = 0;
    for (unsigned i = 0; i < 3 && !taken; ++i) {
        clock = run(ar_prm_clock_attr(b, sizeof b, AR_PRM_IID, ++gAudio.token, AR_CLK_TX_CORE_MCLK, clocks[i].hz, clocks[i].attribute));
        if (!clock) taken = i + 1;
    }
    statuses |= clock << 16 | taken << 24;
    native_mic_stats[1] = (int32_t)statuses;
    return (statuses & 0xffffu) == 0;   // register access needs the two votes; the clock only drives the bus
}

// SoundWire command FIFO of the TX master (v2.0 layout).
struct Swr {Region r; uint8_t id;};
Swr gSwr;
uint32_t swrPack(uint8_t data, uint8_t dev, uint8_t id, uint16_t reg) {return reg | (uint32_t)id << 16 | (uint32_t)dev << 20 | (uint32_t)data << 24;}
uint8_t nextId() {gSwr.id = gSwr.id < 14 ? gSwr.id + 1 : 0; return gSwr.id;}
bool swrWrite(uint8_t dev, uint16_t reg, uint8_t value) {
    for (unsigned i = 0; i < 100 && ((rd32(gSwr.r.va, 0x5050) >> 8) & 0x1f) >= 8; ++i) IODelay(50);
    wr32(gSwr.r.va, 0x5020, swrPack(value, dev, nextId(), reg));
    return true;
}
bool swrRead(uint8_t dev, uint16_t reg, uint8_t *value) {
    const uint8_t id = nextId();
    wr32(gSwr.r.va, 0x5024, swrPack(1, dev, id, reg));
    for (unsigned i = 0; i < 200; ++i) {
        if ((rd32(gSwr.r.va, 0x5050) >> 16) & 0x1f) {
            const uint32_t v = rd32(gSwr.r.va, 0x5040);
            if (((v >> 8) & 0xf) == id) {*value = (uint8_t)v; return true;}
            wr32(gSwr.r.va, 0x308, 1);   // flush and give up on a stale answer
            break;
        }
        IODelay(50);
    }
    native_mic_stats[14] = native_mic_stats[14] + 1;
    return false;
}

// The capture stream of one microphone (the board's "Mic1" route: AMIC3 on micbias 3 into ADC2, slave data port 1 channel 2, master
// port 2, TX decimator 0 from SWR_MIC1, codec DMA TX3) for sheng-audio bit 2, run for about two seconds: [0] stage (positive progress,
// negative the failing step), [1] last DSP status, [2] periods asked, [3] periods returned, [4] bytes, [5] peak, [6] mean absolute value,
// [7] non-zero samples
extern "C" {volatile int32_t native_mic_capture[8];}
bool swrUpdate(uint8_t dev, uint16_t reg, uint8_t mask, uint8_t value) {
    uint8_t v = 0;
    if (!swrRead(dev, reg, &v)) return false;
    return swrWrite(dev, reg, (uint8_t)((v & ~mask) | (value & mask)));
}
void capStage(int s) {native_mic_capture[0] = s;}
bool micStream(uint8_t dev, uintptr_t tx, uintptr_t m) {
    capStage(1);
    // Codec start (the codec driver's io_init), with the hardware's 1 ms and 10 ms waits.
    bool ok = swrUpdate(dev, 0x3103, 0x0e, 0x0e) && swrUpdate(dev, 0x3103, 0x80, 0x80);
    IOSleep(1);
    ok = ok && swrUpdate(dev, 0x3103, 0x40, 0x40);
    IOSleep(1);
    ok = ok && swrUpdate(dev, 0x312a, 0x10, 0) && swrUpdate(dev, 0x3029, 0xf0, 0x80) && swrUpdate(dev, 0x3001, 0x80, 0x80) &&
         swrUpdate(dev, 0x3001, 0x40, 0x40);
    IOSleep(10);
    ok = ok && swrUpdate(dev, 0x3001, 0x40, 0) && swrUpdate(dev, 0x345b, 0x02, 0x02) && swrUpdate(dev, 0x31e1, 0xff, 0x14) &&
         swrUpdate(dev, 0x31df, 0x1f, 0x08);
    static const uint8_t fb[5] = {0x55, 0x44, 0x11, 0, 0};
    for (unsigned i = 0; i < 5 && ok; ++i) ok = swrWrite(dev, (uint16_t)(0x34d0 + i), fb[i]);
    for (uint16_t r : (const uint16_t[]){0x306b, 0x306e, 0x3071, 0x3074}) ok = ok && swrUpdate(dev, r, 0xe0, 0xe0);
    ok = ok && swrUpdate(dev, 0x308f, 0x01, 0);
    if (!ok) {capStage(-1); return false;}
    capStage(2);
    // Micbias 3 at 2.75 V, then on; ADC2 from INP3 (AMIC3); the ADC clocks; ADC2 request in normal mode; ADC2 gain 10.
    ok = swrUpdate(dev, 0x3025, 0x3f, 0x23) && swrUpdate(dev, 0x3409, 0xf0, 0xf0) && swrUpdate(dev, 0x3408, 0x10, 0x10) &&
         swrUpdate(dev, 0x3417, 0x01, 0x01) && swrUpdate(dev, 0x3025, 0xc0, 0x40) && swrUpdate(dev, 0x3127, 0x80, 0x80) &&
         swrUpdate(dev, 0x3408, 0x18, 0x18) && swrUpdate(dev, 0x3457, 0x03, 0x02) && swrUpdate(dev, 0x300f, 0x20, 0x20) &&
         swrUpdate(dev, 0x3410, 0xf0, 0x30) && swrUpdate(dev, 0x3409, 0x20, 0x20) && swrUpdate(dev, 0x300f, 0x20, 0) &&
         swrUpdate(dev, 0x300f, 0x1f, 10);
    if (!ok) {capStage(-2); return false;}
    capStage(3);
    // Slave data port 1 (ADC1/ADC2), channel 2, both banks: isochronous normal mode, 1 bit per sample, sample interval 2, offsets 0,
    // horizontal start and stop 15, lane 2; then prepare and enable the channel.
    for (uint16_t bank = 0; bank <= 0x10 && ok; bank += 0x10)
        ok = swrWrite(dev, (uint16_t)(0x122 + bank), 1) && swrWrite(dev, (uint16_t)(0x123 + bank), 0) && swrWrite(dev, (uint16_t)(0x124 + bank), 0) &&
             swrWrite(dev, (uint16_t)(0x125 + bank), 0) && swrWrite(dev, (uint16_t)(0x126 + bank), 0xff) && swrWrite(dev, (uint16_t)(0x128 + bank), 2);
    ok = ok && swrWrite(dev, 0x102, 0) && swrWrite(dev, 0x103, 0) && swrWrite(dev, 0x105, 0x02);
    for (uint16_t bank = 0; bank <= 0x10 && ok; bank += 0x10) ok = swrWrite(dev, (uint16_t)(0x120 + bank), 0x02);
    if (!ok) {capStage(-3); return false;}
    // Master data port 2 in both banks: sample interval 1 (the device tree's value), offsets 0, lane 2, horizontal 0 to 15, word length 1,
    // channel 2 enabled.
    for (uint32_t bank = 0; bank < 2; ++bank) {
        wr32(m, 0x1224 + 0x40 * bank, 0x01u | 0x02u << 24);
        wr32(m, 0x1228 + 0x40 * bank, 0x02);
        wr32(m, 0x1234 + 0x40 * bank, 0xf0);
    }
    wr32(m, 0x122c, 0);
    capStage(4);
    // TX macro decimator 0: SWR_MIC1 through SWR_MIC, 48 kHz, 150 Hz high-pass, clock on, muted until the analog path settles.
    wr32(tx, 0x100, (rd32(tx, 0x100) & ~0xfu) | 2);
    wr32(tx, 0x104, (rd32(tx, 0x104) & ~3u) | 1);
    wr32(tx, 0x404, (rd32(tx, 0x404) & ~(3u << 5 | 3u << 1)) | 2u << 5);
    wr32(tx, 0x400, (rd32(tx, 0x400) & ~0x3fu) | 4 | 0x10);
    wr32(tx, 0x400, rd32(tx, 0x400) | 0x20);
    capStage(5);
    // The DSP's capture graphs.
    uint8_t b[GPR_MAX_PACKET];
    const ArFormat f = {kRate, 16, 1};
    if (!command(ar_open_mic_device(b, sizeof b, kPort, ++gAudio.token), b, AR_OP_GRAPH_OPEN)) {capStage(-6); return false;}
    if (!command(ar_config_mic_device(b, sizeof b, kPort, ++gAudio.token, &f, 1), b, AR_OP_SET_CFG)) {capStage(-7); return false;}
    if (!command(ar_open_capture_stream(b, sizeof b, kPort, ++gAudio.token), b, AR_OP_GRAPH_OPEN)) {capStage(-8); return false;}
    if (!command(ar_config_capture_stream(b, sizeof b, kPort, ++gAudio.token, &f), b, AR_OP_SET_CFG)) {capStage(-9); return false;}
    for (uint32_t sg : kMicUp)
        if (!command(ar_graph_command(b, sizeof b, kPort, ++gAudio.token, AR_OP_GRAPH_PREPARE, sg), b, AR_OP_GRAPH_PREPARE)) {capStage(-10); return false;}
    for (uint32_t sg : kMicUp)
        if (!command(ar_graph_command(b, sizeof b, kPort, ++gAudio.token, AR_OP_GRAPH_START, sg), b, AR_OP_GRAPH_START)) {capStage(-11); return false;}
    capStage(6);
    IOSleep(300);
    wr32(tx, 0x400, rd32(tx, 0x400) & ~0x10u);   // unmute after the analog path's settling time
    // Read 200 periods (2 s) through four buffers in the second half of the DSP buffer.
    uint64_t sum = 0, samples = 0;
    int32_t peak = 0, nonzero = 0;
    uint32_t asked = 0, back = 0;
    auto ask = [&](uint32_t n) {
        const uint32_t slot = n % kCapSlots;
        if (send(b, ar_read(b, sizeof b, kPort, n, gAudio.dspAddr + kCapOffset + slot * kCapBytes, gAudio.handle, kCapBytes))) {++asked; native_mic_capture[2] = (int32_t)asked;}
    };
    for (uint32_t n = 0; n < 4; ++n) ask(n);
    while (back < 200) {
        ArAnswer a;
        if (!nextAnswer(a, 1000)) {capStage(-12); break;}
        if (a.opcode == AR_OP_READ_DONE) {
            native_mic_capture[1] = (int32_t)a.status;
            const uint32_t slot = (uint32_t)((a.address - (gAudio.dspAddr + kCapOffset)) / kCapBytes) % kCapSlots;
            const int16_t *p = reinterpret_cast<const int16_t *>(gAudio.bufferVa + kCapOffset + slot * kCapBytes);
            const uint32_t count = (a.data_bytes ? a.data_bytes : kCapBytes) / 2;
            for (uint32_t i = 0; i < count && i < kPeriodFrames; ++i) {
                const int32_t v = p[i] < 0 ? -p[i] : p[i];
                sum += (uint32_t)v; ++samples;
                if (v > peak) peak = v;
                if (v) ++nonzero;
            }
            native_mic_capture[3] = (int32_t)++back;
            native_mic_capture[4] = native_mic_capture[4] + (int32_t)a.data_bytes;
            native_mic_capture[5] = peak;
            native_mic_capture[6] = samples ? (int32_t)(sum / samples) : 0;
            native_mic_capture[7] = nonzero;
            if (asked < 200) ask(asked);
        } else if (a.opcode == GPR_OP_BASIC_RESULT && a.status) {
            native_mic_capture[1] = (int32_t)a.status;
        }
    }
    for (uint32_t sg : kMicDown) command(ar_graph_command(b, sizeof b, kPort, ++gAudio.token, AR_OP_GRAPH_STOP, sg), b, AR_OP_GRAPH_STOP);
    if (native_mic_capture[0] > 0) capStage(7);
    gAudio.micReady = native_mic_capture[0] == 7;
    return gAudio.micReady;
}

bool micBringUp() {
    const uint64_t t0 = nowMs();
    micStage(1);
    if (!micRail()) {micFail(1); return false;}
    // The codec out of reset (active low).
    Region reset;
    if (!mapRegion(kTlmm + tb_tlmm_ctl(kWcdResetPin), 0x1000, reset)) {micFail(2); return false;}
    wr32(reset.va, 4, 0);
    wr32(reset.va, 0, 1u << 9 | 7u << 6);   // GPIO, output, 16 mA
    IOSleep(20);
    wr32(reset.va, 4, 1u << 1);
    IOSleep(20);
    micStage(3);
    if (!micPrm()) {micFail(3); return false;}
    micStage(4);
    // TX SoundWire pins: clock gpio0, data gpio1/2/14 (function 1, 2 mA, data with bus hold), slew rate 1.
    Region lpi, slew;
    if (!mapRegion(kLpassTlmm, 0xf000, lpi) || !mapRegion(kLpassSlew & ~0xfffULL, 0x1000, slew)) {micFail(4); return false;}
    static const struct {unsigned pin, slew; uint32_t pull;} pins[] = {{0, 0, 0}, {1, 2, 2}, {2, 4, 2}, {14, 6, 2}};
    uint32_t sv = rd32(slew.va, (uint32_t)(kLpassSlew & 0xfff));
    for (auto &p : pins) {
        wr32(lpi.va, p.pin * 0x1000, 1u << 2 | p.pull);
        sv = (sv & ~(3u << p.slew)) | 1u << p.slew;
    }
    wr32(slew.va, (uint32_t)(kLpassSlew & 0xfff), sv);
    micStage(5);
    // VA macro frame sync generator (the TX macro's fsgen), then the TX macro MCLK and its SoundWire clock with a reset pulse.
    Region va, tx;
    if (!mapRegion(kVaMacro, 0x1000, va) || !mapRegion(kTxMacro, 0x1000, tx)) {micFail(5); return false;}
    wr32(va.va, 0x0, rd32(va.va, 0x0) | 1);
    wr32(va.va, 0x4, rd32(va.va, 0x4) | 3);
    wr32(va.va, 0x4, (rd32(va.va, 0x4) & ~2u) | 1);
    wr32(va.va, 0x80, rd32(va.va, 0x80) | 2);
    // On SM8550 the VA macro also gates the SoundWire master clock (its SWR control, reset pulse around the enable) and sets the mic
    // clock dividers to 1; i508 started the master with only the TX macro's gate and the master took no writes.
    for (uint32_t r = 0xd0; r <= 0xd8; r += 4) wr32(va.va, r, (rd32(va.va, r) & ~0xeeu) | 0xccu);
    wr32(va.va, 0x8, rd32(va.va, 0x8) | 2);
    wr32(va.va, 0x8, rd32(va.va, 0x8) | 1);
    wr32(va.va, 0x8, rd32(va.va, 0x8) & ~2u);
    wr32(tx.va, 0x90, rd32(tx.va, 0x90) | 1);
    wr32(tx.va, 0x0, rd32(tx.va, 0x0) | 1);
    wr32(tx.va, 0x4, rd32(tx.va, 0x4) | 1);
    wr32(tx.va, 0x8, rd32(tx.va, 0x8) | 2);
    wr32(tx.va, 0x8, rd32(tx.va, 0x8) | 1);
    wr32(tx.va, 0x8, rd32(tx.va, 0x8) & ~2u);
    native_mic_stats[10] = (int32_t)rd32(tx.va, 0x0);
    native_mic_stats[11] = (int32_t)rd32(va.va, 0x0);
    micStage(6);
    // The TX SoundWire master as the bus driver starts it: 50 rows x 16 columns, auto enumeration, no-ping limit, CPU execution
    // environment, clock start, 7 command retries, enable, pulse interrupts.
    if (!mapRegion(kSwrTx, 0x10000, gSwr.r)) {micFail(6); return false;}
    const uintptr_t m = gSwr.r.va;
    native_mic_stats[3] = (int32_t)rd32(m, 0x00);
    if ((rd32(m, 0x00) >> 24) != 2) {micFail(7); return false;}
    wr32(m, 0x008, 1);          // soft reset, as the bus driver does before it starts the master again
    IOSleep(1);
    wr32(m, 0x101c, 1u << 3 | 7u);
    wr32(m, 0x500, 1);
    wr32(m, 0x1048, (rd32(m, 0x1048) & ~(0x1fu << 17)) | 0x1fu << 17);
    wr32(m, 0x018, 1);
    wr32(m, 0x5060, 1);
    wr32(m, 0x314, 7u | 1u << 31);
    wr32(m, 0x004, 1);
    wr32(m, 0x004, 2);
    wr32(m, 0x5008, 0xffffffffu);
    wr32(m, 0x5004, 0x1ffff);
    wr32(m, 0x004, 3);
    for (unsigned i = 0; i < 500 && !(rd32(m, 0x5064) & 1); ++i) IOSleep(1);
    {
        const uint32_t txr[4] = {0x0, 0x4, 0x8, 0x90}, var[3] = {0x0, 0x8, 0x80}, mr[7] = {0x4, 0x18, 0x1044, 0x104c, 0x5060, 0x5064, 0x5000};
        unsigned k = 0;
        for (uint32_t r : txr) native_mic_regs[k++] = rd32(tx.va, r);
        for (uint32_t r : var) native_mic_regs[k++] = rd32(va.va, r);
        for (uint32_t r : mr) native_mic_regs[k++] = rd32(m, r);
        native_mic_regs[k++] = rd32(lpi.va, 0);
        native_mic_regs[k++] = rd32(slew.va, (uint32_t)(kLpassSlew & 0xfff));
    }
    native_mic_stats[4] = (int32_t)rd32(m, 0x5064);
    native_mic_stats[13] = (int32_t)rd32(m, 0x100);
    micStage(7);
    // The codec attaches and is enumerated (device number 1).
    uint32_t slaves = 0;
    for (unsigned i = 0; i < 200 && !((slaves = rd32(m, 0x1090)) & 0x3fffcu); ++i) IOSleep(5);
    native_mic_stats[5] = (int32_t)slaves;
    native_mic_stats[12] = (int32_t)rd32(m, 0x5000);
    native_mic_stats[6] = (int32_t)rd32(m, 0x530 + 8);
    native_mic_stats[7] = (int32_t)rd32(m, 0x534 + 8);
    if (!(slaves & 0x3fffcu)) {micFail(8); return false;}
    unsigned dev = 1;
    while (dev < 11 && ((slaves >> (2 * dev)) & 3) == 0) ++dev;
    micStage(8);
    uint8_t efuse = 0, id0 = 0, id1 = 0;
    if (swrRead((uint8_t)dev, 0x34b0, &efuse)) native_mic_stats[8] = efuse;
    if (swrRead((uint8_t)dev, 0x50, &id0) && swrRead((uint8_t)dev, 0x51, &id1)) native_mic_stats[9] = id0 | id1 << 8;   // SCP_DevId_0/1
    native_mic_stats[15] = (int32_t)(nowMs() - t0);
    micStage(9);
    micStream((uint8_t)dev, tx.va, m);
    return true;
}

bool publishService() {
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    native_audio_service_static_init();
    if (OSMetaClass::postModLoad(handle)) return false;
    sheng_audio_service_start();
    return true;
}

void audioThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng-audio");
    const uint64_t start = nowMs();
    stage(10);
    if (!setupBuffer() || !setupShared()) {fail(10); return;}
    if (!setupSmmu()) {fail(11); return;}
    stage(20);
    if (!setupPins()) {fail(20); return;}
    stage(30);
    if (!setupClocks()) {fail(30); return;}
    if (!setupBus(0) || !setupBus(1)) {fail(31); return;}
    stage(40);
    if (!setupAmps()) {fail(40); return;}
    stage(50);
    static const ShengAdspClient client = {dspState, dspReceive};
    gAudio.channel = sheng_adsp_channel("adsp_apps", kIntentSize, kIntentCount, 0, &client, nullptr);
    if (!gAudio.channel) {fail(50); return;}
    if (!waitUp(60000)) {fail(51); return;}
    stage(51);
    if (!dspReady()) return;
    stage(52);
    if (!openGraphs()) return;
    stage(56);
    if (gAudio.toneMs) playTone();
    if (gAudio.mode & 4) micBringUp();
    if (!publishService()) {fail(58); return;}
    __atomic_store_n(&gAudio.ctl->state, gAudio.ctl->state | ShengAudioOutputReady, __ATOMIC_RELEASE);
    __atomic_store_n(&gAudio.serving, 1u, __ATOMIC_RELEASE);
    native_audio_stats[15] = (int32_t)(nowMs() - start);
    stage(60);
    if (gAudio.mode & 8) {
        thread_t thread;
        if (kernel_thread_start(lockSoundThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    }
    serve(0);
}
}  // namespace

// The plug-in's record of unanswered properties (ShengAudioDiag in the control page), for the log.
extern "C" const ShengAudioDiag *sheng_audio_diag(void) {
    return gAudio.ctl ? reinterpret_cast<const ShengAudioDiag *>(reinterpret_cast<const uint8_t *>(gAudio.ctl) + SHENG_AUDIO_DIAG_OFFSET) : nullptr;
}

extern "C" IOBufferMemoryDescriptor *sheng_audio_memory(uint32_t type) {
    if (!__atomic_load_n(&gAudio.serving, __ATOMIC_ACQUIRE)) return nullptr;
    IOBufferMemoryDescriptor *m = type == ShengAudioMemoryControl ? gAudio.control.get() : type == ShengAudioMemoryOutput ? gAudio.outRing.get() :
                                  type == ShengAudioMemoryInput ? gAudio.inRing.get() : nullptr;
    if (m) m->retain();
    return m;
}

extern "C" int sheng_audio_request(uint32_t selector) {
    if (!__atomic_load_n(&gAudio.serving, __ATOMIC_ACQUIRE)) return -1;
    switch (selector) {
    case ShengAudioStartOutput: __atomic_or_fetch(&gAudio.want, ShengAudioOutputRunning, __ATOMIC_ACQ_REL); break;
    case ShengAudioStopOutput: __atomic_and_fetch(&gAudio.want, ~ShengAudioOutputRunning, __ATOMIC_ACQ_REL); break;
    case ShengAudioStartInput: __atomic_or_fetch(&gAudio.want, ShengAudioInputRunning, __ATOMIC_ACQ_REL); break;
    case ShengAudioStopInput: __atomic_and_fetch(&gAudio.want, ~ShengAudioInputRunning, __ATOMIC_ACQ_REL); break;
    default: return -1;
    }
    IOLockWakeup(gAudio.lock, &gAudio.ringCount, false);
    return 0;
}

extern "C" void sheng_audio_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("sheng-audio", &mode, sizeof mode) || !mode || mode > 15 || native_audio_stats[0]) return;
    gAudio.mode = mode;
    gAudio.toneMs = (mode & 2) ? 1500 : 0;
    gAudio.atten = 30;
    PE_parse_boot_argn("sheng-audio-tone-ms", &gAudio.toneMs, sizeof gAudio.toneMs);
    PE_parse_boot_argn("sheng-audio-atten", &gAudio.atten, sizeof gAudio.atten);
    native_audio_stats[0] = 1;
    if (!sheng_adsp_channel || !sheng_adsp_send) {fail(1); return;}
    gAudio.lock = IOLockAlloc();
    gAudio.ringLock = IOSimpleLockAlloc();
    if (!gAudio.lock || !gAudio.ringLock) {fail(2); return;}
    thread_t thread;
    if (kernel_thread_start(audioThread, nullptr, &thread) != KERN_SUCCESS) {fail(3); return;}
    thread_deallocate(thread);
}
