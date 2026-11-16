// SPDX-License-Identifier: BSD-2-Clause-Patent
// The touchscreen follows the keyboard cover's lid (macOS). Closed over the screen, the cover lies on the glass and the panel reports
// up to ten contacts that come and go about 120 times a second; macOS's WindowServer takes no touchscreen events ("Invalid event
// type: 11") and its gesture handling stops answering under such a stream. While the lid is closed the first frame is published with
// no contacts, so every contact lifts, and later frames are dropped; opening the cover passes frames again. KeyboardProbe.cpp owns the
// lid's hall sensor (GPIO 171) and publishes it in native_keyboard_lid_closed once its thread serves the cover, so a cover that never
// comes up leaves the touchscreen as it was. Host-tested by tests/touch_lid_host.c.
#ifndef SHENG_TOUCH_LID_H
#define SHENG_TOUCH_LID_H
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

enum touch_lid_action {TouchLidPass, TouchLidRelease, TouchLidDrop};
struct touch_lid_gate {bool released;};
#define TOUCH_LID_GATE_INIT {false}

// The keyboard probe's word: 1 while the lid sensor sees the cover closed over the screen.
static inline bool touch_lid_closed_word(uint32_t word) {return word == 1;}

// One touch frame: what to do with it given the lid now.
static inline enum touch_lid_action touch_lid_gate_step(struct touch_lid_gate *gate, bool closed)
{
    if (!closed) {
        gate->released = false;
        return TouchLidPass;
    }
    if (!gate->released) {
        gate->released = true;
        return TouchLidRelease;
    }
    return TouchLidDrop;
}

#endif
