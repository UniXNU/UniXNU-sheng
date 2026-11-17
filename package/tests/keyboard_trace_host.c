// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the cover's recent-frame trace (package/KeyboardTrace.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../KeyboardTrace.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static void empty_trace(void)
{
    struct kb_trace t; memset(&t, 0, sizeof t);
    struct kb_trace_summary s;
    kb_trace_summarize(&t, 5000, &s);
    CHECK(s.count == 0 && !strcmp(s.kinds, ""));
    CHECK(s.since_any == KB_TRACE_NEVER && s.since_input == KB_TRACE_NEVER && s.since_accel == KB_TRACE_NEVER);
}

static void kinds_oldest_first_with_ages(void)
{
    struct kb_trace t; memset(&t, 0, sizeof t);
    kb_trace_push(&t, 1000, KbTraceAccel);
    kb_trace_push(&t, 1100, KbTraceTouchpad);
    kb_trace_push(&t, 1150, KbTraceKeyboard);
    kb_trace_push(&t, 1190, KbTraceAttach);
    struct kb_trace_summary s;
    kb_trace_summarize(&t, 1200, &s);
    CHECK(s.count == 4 && !strcmp(s.kinds, "xtka"));
    CHECK(s.since_any == 10 && s.since_input == 50 && s.since_accel == 200);
}

static void keeps_the_last_sixteen(void)
{
    struct kb_trace t; memset(&t, 0, sizeof t);
    kb_trace_push(&t, 10, KbTraceAccel);                  // pushed out below
    for (unsigned i = 0; i < KB_TRACE_FRAMES; ++i) kb_trace_push(&t, 100 + i, i & 1 ? KbTraceConsumer : KbTraceTouchpad);
    struct kb_trace_summary s;
    kb_trace_summarize(&t, 200, &s);
    CHECK(s.count == KB_TRACE_FRAMES && !strcmp(s.kinds, "tctctctctctctctc"));
    CHECK(s.since_any == 200 - (100 + KB_TRACE_FRAMES - 1) && s.since_input == s.since_any);
    CHECK(s.since_accel == KB_TRACE_NEVER);               // the only accelerometer frame is no longer held
}

static void every_kind_has_a_letter(void)
{
    struct kb_trace t; memset(&t, 0, sizeof t);
    const enum kb_trace_kind all[] = {KbTraceInvalid, KbTraceKeyboard, KbTraceConsumer, KbTraceTouchpad, KbTraceAttach, KbTraceAccel,
                                      KbTraceAuth, KbTraceOther};
    for (unsigned i = 0; i < sizeof all / sizeof *all; ++i) kb_trace_push(&t, 1, all[i]);
    struct kb_trace_summary s;
    kb_trace_summarize(&t, 1, &s);
    CHECK(!strcmp(s.kinds, "-kctaxuo"));
    CHECK(s.since_input == 0 && s.since_accel == 0);
}

static void the_clock_may_wrap(void)
{
    struct kb_trace t; memset(&t, 0, sizeof t);
    kb_trace_push(&t, 0xfffffff0u, KbTraceAccel);
    struct kb_trace_summary s;
    kb_trace_summarize(&t, 0x10u, &s);
    CHECK(s.since_accel == 0x20u && s.since_any == 0x20u);
}

int main(void)
{
    empty_trace();
    kinds_oldest_first_with_ages();
    keeps_the_last_sixteen();
    every_kind_has_a_letter();
    the_clock_may_wrap();
    printf("checks %u\n", checks);
    return 0;
}
