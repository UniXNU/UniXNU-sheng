// SPDX-License-Identifier: BSD-2-Clause-Patent
// The first step of audio on the sheng board (boot argument sheng-audio=1 or 2; needs sheng-adsp=2): registers the audio packet router
// channel "adsp_apps" with the audio DSP and asks the DSP's audio framework manager whether the framework is ready (ShengGpr.c). With 1
// the DSP opens the channel and this side answers; with 2 this side opens it. Playback itself (the graph of the codec and the six
// speaker amplifiers, and the iOS audio driver above it) builds on this channel and is not here yet.
//
// Design: everything runs on one work loop. The DSP thread's callbacks only copy a message into a small ring and signal an
// IOInterruptEventSource (no provider: a software event), so the DSP thread never waits. One query is in flight; a 1 s timer gives up on
// it, and a "not ready" answer is asked again after 100 ms, up to 100 queries.
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "ShengAdsp.h"
#include "ShengGpr.h"
}

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *) __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));
// [0] stage (1 started, 2 channel registered, 3 channel up, 4 query sent, 5 framework ready; negative: the step that failed, -6 gave
// up), [1] queries, [2] answers, [3] last status, [4] refusals, [5] timeouts, [6] ignored packets, [7] ms from channel up to ready,
// [8] channel ups, [9] channel downs, [10] last opcode received, [11] messages dropped (ring full), [12] send refusals, [13] mode
extern "C" {volatile int32_t native_audio_stats[16];}

namespace {
constexpr uint32_t kAnswerMs = 1000, kRetryMs = 100, kMaxQueries = 100, kRing = 4, kMessage = 64;
constexpr uint32_t kIntentSize = 512, kIntentCount = 20;

struct Audio {
    IOWorkLoop *loop;
    IOTimerEventSource *answer, *retry;
    IOInterruptEventSource *inbox;
    IOSimpleLock *ringLock;
    struct {uint32_t length; uint8_t data[kMessage];} ring[kRing];
    uint32_t ringHead, ringCount;
    volatile uint32_t up, changed;
    bool wasUp;
    uint64_t upAt;
    GprSpfProbe spf;
    ShengAdspChannel *channel;
};
Audio gAudio;

void bump(int index) {native_audio_stats[index] = native_audio_stats[index] + 1;}
void fail(int step) {native_audio_stats[0] = -step;}

// DSP thread: copy and signal, nothing else.
void dspState(void *, int up) {
    __atomic_store_n(&gAudio.up, up ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gAudio.changed, 1u, __ATOMIC_RELEASE);
    gAudio.inbox->interruptOccurred(nullptr, nullptr, 0);
}
void dspReceive(void *, const uint8_t *data, uint32_t length) {
    IOSimpleLockLock(gAudio.ringLock);
    if (gAudio.ringCount < kRing) {
        auto &slot = gAudio.ring[(gAudio.ringHead + gAudio.ringCount) % kRing];
        slot.length = length < kMessage ? length : kMessage;  // the readiness answers are short; longer packets are only counted
        memcpy(slot.data, data, slot.length);
        gAudio.ringCount++;
    } else {
        bump(11);
    }
    IOSimpleLockUnlock(gAudio.ringLock);
    gAudio.inbox->interruptOccurred(nullptr, nullptr, 0);
}

void record() {
    native_audio_stats[1] = (int32_t)gAudio.spf.queries;
    native_audio_stats[2] = (int32_t)gAudio.spf.answers;
    native_audio_stats[3] = (int32_t)gAudio.spf.last_status;
    native_audio_stats[4] = (int32_t)gAudio.spf.errors;
    native_audio_stats[5] = (int32_t)gAudio.spf.timeouts;
    native_audio_stats[6] = (int32_t)gAudio.spf.ignored;
    native_audio_stats[10] = (int32_t)gAudio.spf.last_opcode;
}
void ask() {
    uint8_t out[GPR_HEADER_BYTES];
    const uint32_t n = gpr_spf_next(&gAudio.spf, out);
    if (!n) return;
    if (sheng_adsp_send(gAudio.channel, out, n)) bump(12);  // the answer timer counts it as unanswered
    if (native_audio_stats[0] == 3) native_audio_stats[0] = 4;
    gAudio.answer->setTimeoutMS(kAnswerMs);
    record();
}
void outcome(int result) {
    if (result == SpfIsReady) {
        gAudio.answer->cancelTimeout();
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - gAudio.upAt, &ns);
        native_audio_stats[7] = (int32_t)(ns / 1000000);
        native_audio_stats[0] = 5;
    } else if (result == SpfDone) {
        gAudio.answer->cancelTimeout();
        fail(6);
    } else if (result == SpfNotReady || result == SpfError) {
        gAudio.answer->cancelTimeout();
        gAudio.retry->setTimeoutMS(kRetryMs);
    }
    record();
}

void inboxFired(OSObject *, IOInterruptEventSource *, int) {
    if (__atomic_exchange_n(&gAudio.changed, 0u, __ATOMIC_ACQUIRE)) {
        const bool up = __atomic_load_n(&gAudio.up, __ATOMIC_ACQUIRE) != 0;
        if (up && !gAudio.wasUp) {
            bump(8);
            gAudio.upAt = mach_absolute_time();
            gpr_spf_init(&gAudio.spf, GPR_PORT_APM, kMaxQueries);
            native_audio_stats[0] = 3;
            ask();
        } else if (!up && gAudio.wasUp) {
            bump(9);
            gAudio.answer->cancelTimeout();
            gAudio.retry->cancelTimeout();
            native_audio_stats[0] = 2;
        }
        gAudio.wasUp = up;
    }
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
        if (!length) break;
        if (gAudio.wasUp) outcome(gpr_spf_receive(&gAudio.spf, data, length));
    }
}
void answerFired(OSObject *, IOTimerEventSource *) {
    const int result = gpr_spf_timeout(&gAudio.spf);
    if (result == SpfDone) {fail(6); record(); return;}
    record();
    if (gAudio.wasUp) ask();
}
void retryFired(OSObject *, IOTimerEventSource *) {
    if (gAudio.wasUp) ask();
}
}  // namespace

extern "C" void sheng_audio_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("sheng-audio", &mode, sizeof mode) || (mode != 1 && mode != 2) || native_audio_stats[0]) return;
    native_audio_stats[13] = (int32_t)mode;
    native_audio_stats[0] = 1;
    if (!sheng_adsp_channel || !sheng_adsp_send) {fail(1); return;}
    gAudio.loop = IOWorkLoop::workLoop();
    gAudio.ringLock = IOSimpleLockAlloc();
    if (!gAudio.loop || !gAudio.ringLock) {fail(2); return;}
    gAudio.answer = IOTimerEventSource::timerEventSource(gAudio.loop, answerFired);
    gAudio.retry = IOTimerEventSource::timerEventSource(gAudio.loop, retryFired);
    gAudio.inbox = IOInterruptEventSource::interruptEventSource(gAudio.loop, inboxFired);
    if (!gAudio.answer || !gAudio.retry || !gAudio.inbox || gAudio.loop->addEventSource(gAudio.answer) ||
        gAudio.loop->addEventSource(gAudio.retry) || gAudio.loop->addEventSource(gAudio.inbox)) {fail(2); return;}
    static const ShengAdspClient client = {dspState, dspReceive};
    gAudio.channel = sheng_adsp_channel("adsp_apps", kIntentSize, kIntentCount, mode == 2, &client, nullptr);
    if (!gAudio.channel) {fail(3); return;}
    native_audio_stats[0] = 2;
}
