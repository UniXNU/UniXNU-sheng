/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_KTZ8866_DIAGNOSTICS_H
#define SHENG_KTZ8866_DIAGNOSTICS_H
#include <stdint.h>

/* Fixed, read-only sequence. ValidMask bits 0..4 follow this order, not register numbers. */
enum {KtzRegisterCount = 5, KtzAllValid = 0x1f, KtzBrightnessValid = 0x18};
enum {KtzCancelled = -1, KtzUnusable = -2, KtzStartFailed = -3, KtzNotSampled = -4};
static const uint8_t ktz_snapshot_registers[KtzRegisterCount] = {0x02, 0x03, 0x15, 0x04, 0x05};
typedef struct {
    uint32_t sequence, validMask, errors, target;
    int32_t error, lastSuccessfulWrite;
    uint8_t raw[KtzRegisterCount], next, active;
} KtzSnapshot;

static inline void ktz_snapshot_init(KtzSnapshot *s, int usable) {
    const KtzSnapshot empty = {0, 0, 0, 0, usable ? KtzNotSampled : KtzUnusable, -1, {0}, 0, 0};
    *s = empty;
}
static inline int ktz_snapshot_begin(KtzSnapshot *s, uint32_t target) {
    if (s->active) return 0;
    ++s->sequence;
    s->target = target; s->validMask = 0; s->error = 0; s->next = 0; s->active = 1;
    return 1;
}
/* One completion per GENI register transfer. A failure ends this sample, without retrying it. */
static inline void ktz_snapshot_complete(KtzSnapshot *s, int32_t error, uint8_t data) {
    if (!s->active || s->next >= KtzRegisterCount) return;
    if (error) {s->error = error; ++s->errors; s->active = 0; return;}
    s->raw[s->next] = data;
    s->validMask |= 1u << s->next;
    if (++s->next == KtzRegisterCount) s->active = 0;
}
/* A changed request invalidates the whole sample, including any earlier successful reads. */
static inline int ktz_snapshot_cancel(KtzSnapshot *s) {
    if (!s->active) return 0;
    s->active = 0; s->validMask = 0; s->error = KtzCancelled;
    return 1;
}
static inline int32_t ktz_snapshot_level(const KtzSnapshot *s) {
    return (s->validMask & KtzBrightnessValid) == KtzBrightnessValid
        ? (int32_t)(((uint32_t)s->raw[4] << 3) | (s->raw[3] & 7)) : -1;
}

/* native_backlight_ktz[2][KtzDiagCount], C linkage in AppleARMBacklight.cpp, weak-imported by BootLog.c.
 * One work-loop writer per row. Epoch is an even/odd publication guard, Sequence is the sample number.
 * Raw values and ReadbackLevel are -1 when invalid. LastSuccessfulWrite is -1 until a successful MSB
 * write, never a startup read or e.written's give-up value. Errors counts failed diagnostic transfers;
 * Error is 0, a positive Geni error, or the negative diagnostic status above. */
enum {
    KtzDiagEpoch, KtzDiagSequence, KtzDiagValidMask, KtzDiagError, KtzDiagErrors,
    KtzDiagCfg1, KtzDiagCfg2, KtzDiagCurrent, KtzDiagLsb, KtzDiagMsb,
    KtzDiagReadback, KtzDiagTarget, KtzDiagLastWrite, KtzDiagActive, KtzDiagUsable, KtzDiagCount
};
static inline void ktz_diag_store(volatile int32_t *row, const KtzSnapshot *s, int usable) {
    int32_t values[KtzDiagCount] = {0};
    values[KtzDiagSequence] = (int32_t)s->sequence; values[KtzDiagValidMask] = (int32_t)s->validMask;
    values[KtzDiagError] = s->error; values[KtzDiagErrors] = (int32_t)s->errors;
    for (unsigned i = 0; i < KtzRegisterCount; ++i)
        values[KtzDiagCfg1 + i] = (s->validMask & (1u << i)) ? s->raw[i] : -1;
    values[KtzDiagReadback] = ktz_snapshot_level(s); values[KtzDiagTarget] = (int32_t)s->target;
    values[KtzDiagLastWrite] = s->lastSuccessfulWrite; values[KtzDiagActive] = s->active;
    values[KtzDiagUsable] = usable != 0;
    __atomic_add_fetch(&row[KtzDiagEpoch], 1, __ATOMIC_ACQ_REL);
    for (unsigned i = 1; i < KtzDiagCount; ++i) __atomic_store_n(&row[i], values[i], __ATOMIC_RELAXED);
    __atomic_add_fetch(&row[KtzDiagEpoch], 1, __ATOMIC_RELEASE);
}
/* BootLog never waits for a publisher, including at panic: at most two attempts. */
static inline int ktz_diag_load(const volatile int32_t *row, int32_t values[KtzDiagCount]) {
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const int32_t epoch = __atomic_load_n(&row[KtzDiagEpoch], __ATOMIC_ACQUIRE);
        if (epoch & 1) continue;
        for (unsigned i = 1; i < KtzDiagCount; ++i) values[i] = __atomic_load_n(&row[i], __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (__atomic_load_n(&row[KtzDiagEpoch], __ATOMIC_RELAXED) == epoch) {
            values[KtzDiagEpoch] = epoch;
            return epoch != 0; /* An uninitialized row is not a measured all-zero sample. */
        }
    }
    return 0;
}
#endif
