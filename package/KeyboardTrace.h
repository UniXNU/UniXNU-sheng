// SPDX-License-Identifier: BSD-2-Clause-Patent
// The keyboard controller's last frames, for the log line KeyboardProbe.cpp writes when the controller reports the cover detached or
// attached. The WN8030 relays the cover's keyboard, touchpad and accelerometer frames over I2C; whether those were still arriving just
// before a detach (and the controller's own I2C health at that moment) separates a link to the cover that dropped from a controller
// that stopped. Kinds print as one letter each, oldest first: - invalid, k keyboard, c consumer, t touchpad, a attach, x accelerometer,
// u authentication, o other. Ages are milliseconds before the summary (the count may wrap). Host-tested by tests/keyboard_trace_host.c.
#ifndef SHENG_KEYBOARD_TRACE_H
#define SHENG_KEYBOARD_TRACE_H
#include <stdint.h>

#define KB_TRACE_FRAMES 16u
#define KB_TRACE_NEVER 0xffffffffu
enum kb_trace_kind {KbTraceInvalid, KbTraceKeyboard, KbTraceConsumer, KbTraceTouchpad, KbTraceAttach, KbTraceAccel, KbTraceAuth, KbTraceOther};
struct kb_trace {uint32_t at[KB_TRACE_FRAMES]; uint8_t kind[KB_TRACE_FRAMES]; uint32_t pushed;};
// count: frames held; since_*: ms since the newest frame, input frame (keyboard, consumer, touchpad), accelerometer frame held, or
// KB_TRACE_NEVER; kinds: their letters, oldest first.
struct kb_trace_summary {uint32_t count, since_any, since_input, since_accel; char kinds[KB_TRACE_FRAMES + 1];};

static inline void kb_trace_push(struct kb_trace *t, uint32_t now_ms, enum kb_trace_kind kind)
{
    const uint32_t slot = t->pushed % KB_TRACE_FRAMES;
    t->at[slot] = now_ms;
    t->kind[slot] = (uint8_t)kind;
    ++t->pushed;
}

static inline void kb_trace_summarize(const struct kb_trace *t, uint32_t now_ms, struct kb_trace_summary *s)
{
    static const char letters[] = "-kctaxuo";
    s->count = t->pushed < KB_TRACE_FRAMES ? t->pushed : KB_TRACE_FRAMES;
    s->since_any = s->since_input = s->since_accel = KB_TRACE_NEVER;
    for (uint32_t i = 0; i < s->count; ++i) {
        const uint32_t slot = (t->pushed - s->count + i) % KB_TRACE_FRAMES;
        const uint8_t kind = t->kind[slot];
        const uint32_t age = now_ms - t->at[slot];
        s->kinds[i] = kind < sizeof letters - 1 ? letters[kind] : '?';
        s->since_any = age;
        if (kind == KbTraceKeyboard || kind == KbTraceConsumer || kind == KbTraceTouchpad) s->since_input = age;
        if (kind == KbTraceAccel) s->since_accel = age;
    }
    s->kinds[s->count] = 0;
}

#endif
