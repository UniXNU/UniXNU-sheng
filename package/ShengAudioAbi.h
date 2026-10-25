/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* The interface between the sheng audio service (AudioProbe.cpp, IOKit class ShengAudio) and the audio server plug-in that iOS loads
 * (hal/ShengAudioPlugIn.c). The plug-in opens the service, maps three memory types and starts and stops each direction.
 *
 * Memory type 0 is the control page; 1 the playback ring; 2 the capture ring. Rings hold 16-bit little-endian interleaved samples,
 * ring_frames frames each. Sample time t lives at frame t % ring_frames. Sample time 0 of a direction is the frame the DSP plays (or
 * captures) at host time anchor_host of that direction; one frame lasts ticks_per_frame_q32 / 2^32 host ticks. The service consumes
 * playback frames lead_frames ahead of the play position and fills capture frames lead_frames behind it, so the plug-in writes at least
 * lead_frames ahead and reads at least lead_frames behind. */
#ifndef SHENG_AUDIO_ABI_H
#define SHENG_AUDIO_ABI_H
#include <stdint.h>

#define SHENG_AUDIO_CLASS "ShengAudio"
#define SHENG_AUDIO_MAGIC 0x53484155u        /* 'SHAU' */
#define SHENG_AUDIO_VERSION 1u
#define SHENG_AUDIO_RATE 48000u
#define SHENG_AUDIO_OUT_CHANNELS 2u
#define SHENG_AUDIO_IN_CHANNELS 1u
#define SHENG_AUDIO_PERIOD_FRAMES 480u        /* 10 ms */
#define SHENG_AUDIO_RING_FRAMES 16384u

enum {ShengAudioMemoryControl = 0, ShengAudioMemoryOutput = 1, ShengAudioMemoryInput = 2};
enum {ShengAudioStartOutput = 0, ShengAudioStopOutput = 1, ShengAudioStartInput = 2, ShengAudioStopInput = 3};
enum {ShengAudioOutputRunning = 1u << 0, ShengAudioInputRunning = 1u << 1, ShengAudioOutputReady = 1u << 2, ShengAudioInputReady = 1u << 3};

typedef struct {
    uint32_t magic, version;
    uint32_t rate, out_channels, in_channels, period_frames, ring_frames, lead_frames;
    volatile uint32_t state;                 /* ShengAudio*Running / *Ready bits */
    uint32_t reserved0;
    uint64_t ticks_per_frame_q32;            /* host ticks per frame, 32.32 fixed point */
    volatile uint64_t out_anchor_host, in_anchor_host;
    volatile uint64_t out_frames, in_frames; /* frames the service consumed / produced so far */
    volatile uint32_t out_late, in_late;     /* periods the service had to send or deliver without fresh data */
    volatile uint32_t out_seq, in_seq;       /* incremented around anchor updates (odd while writing) */
} ShengAudioControl;

/* Second half of the control page: the plug-in records what the audio server asked for and it does not answer (selector, object id
 * << 24 | scope fourcc's first byte), so the kernel log names them without the unified log. */
#define SHENG_AUDIO_DIAG_OFFSET 2048u
#define SHENG_AUDIO_DIAG_SLOTS 32u
typedef struct {
    volatile uint32_t count, loads;
    volatile uint32_t selector[SHENG_AUDIO_DIAG_SLOTS], where[SHENG_AUDIO_DIAG_SLOTS];
} ShengAudioDiag;
/* After it, every distinct property access in the order the audio server made it: selector, then object << 24 | scope's first byte << 16 |
 * kind (1 set, 0 get) | 2 if it failed. */
#define SHENG_AUDIO_TRACE_OFFSET 2560u
#define SHENG_AUDIO_TRACE_SLOTS 128u
typedef struct {
    volatile uint32_t count;
    volatile uint32_t entry[SHENG_AUDIO_TRACE_SLOTS][2];
} ShengAudioTrace;

#endif
