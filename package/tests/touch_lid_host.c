// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of the touchscreen's lid gate (package/TouchLid.h): while the keyboard cover is closed over the screen, the first touch
// frame becomes one frame with no contacts (every contact lifts) and later frames are dropped; opening the cover passes frames again.
#include <stdio.h>
#include <stdlib.h>
#include "../TouchLid.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static void open_passes(void)
{
    struct touch_lid_gate gate = TOUCH_LID_GATE_INIT;
    for (int i = 0; i < 3; ++i) CHECK(touch_lid_gate_step(&gate, false) == TouchLidPass);
}

static void closing_lifts_once_then_drops(void)
{
    struct touch_lid_gate gate = TOUCH_LID_GATE_INIT;
    CHECK(touch_lid_gate_step(&gate, false) == TouchLidPass);
    CHECK(touch_lid_gate_step(&gate, true) == TouchLidRelease);
    for (int i = 0; i < 100; ++i) CHECK(touch_lid_gate_step(&gate, true) == TouchLidDrop);
}

static void opening_passes_and_closing_again_lifts_again(void)
{
    struct touch_lid_gate gate = TOUCH_LID_GATE_INIT;
    CHECK(touch_lid_gate_step(&gate, true) == TouchLidRelease);
    CHECK(touch_lid_gate_step(&gate, true) == TouchLidDrop);
    CHECK(touch_lid_gate_step(&gate, false) == TouchLidPass);
    CHECK(touch_lid_gate_step(&gate, false) == TouchLidPass);
    CHECK(touch_lid_gate_step(&gate, true) == TouchLidRelease);
    CHECK(touch_lid_gate_step(&gate, true) == TouchLidDrop);
}

static void closed_at_first_frame_lifts(void)
{
    // The cover was already closed when the panel sent its first frame: no contact is down yet, the empty frame is harmless.
    struct touch_lid_gate gate = TOUCH_LID_GATE_INIT;
    CHECK(touch_lid_gate_step(&gate, true) == TouchLidRelease);
}

static void lid_state_word(void)
{
    // The keyboard probe publishes the lid as a word; only 1 is closed.
    CHECK(!touch_lid_closed_word(0));
    CHECK(touch_lid_closed_word(1));
    CHECK(!touch_lid_closed_word(2));
}

int main(void)
{
    open_passes();
    closing_lifts_once_then_drops();
    opening_passes_and_closing_again_lifts_again();
    closed_at_first_frame_lifts();
    lid_state_word();
    printf("checks %u\n", checks);
    return 0;
}
