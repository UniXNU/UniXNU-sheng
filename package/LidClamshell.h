// SPDX-License-Identifier: BSD-2-Clause-Patent
// macOS's power manager follows the keyboard cover's lid (KeyboardProbe.cpp publishLid). Told that the clamshell closed, the root
// domain notifies its interest clients, and WindowServer turns the built-in display off. It also sleeps the system when sleep is allowed:
// the board's device tree sets /defaults sleep-disabled, which only platform sleep (boot-arg platform-sleep=1, ShengSleep.cpp) lifts.
// Told that it opened (as user input, which turns a dark wake full), the display comes back. Only changes are told: the hall sensor is
// also polled once a second, and the root domain takes the lid as open until told otherwise, so an open lid at start is not told.
// Host-tested by tests/lid_clamshell_host.c.
#ifndef SHENG_LID_CLAMSHELL_H
#define SHENG_LID_CLAMSHELL_H
#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

enum lid_clamshell_message {LidClamshellNone, LidClamshellClosed, LidClamshellOpened};
struct lid_clamshell {int8_t told;};   // what the power manager was last told: -1 nothing, 0 open, 1 closed
#define LID_CLAMSHELL_INIT {-1}

// The message for the lid as read now, if any.
static inline enum lid_clamshell_message lid_clamshell_step(struct lid_clamshell *s, bool closed)
{
    const int8_t now = closed ? 1 : 0;
    if (s->told == now) return LidClamshellNone;
    if (s->told < 0 && !closed) {s->told = 0; return LidClamshellNone;}
    s->told = now;
    return closed ? LidClamshellClosed : LidClamshellOpened;
}

// The backlight level written to the panel (AppleARMBacklight.cpp sheng_backlight_lid): none while the lid is closed. Told the clamshell
// closed, macOS's WindowServer passes it to the framebuffer (logged "clamshell state:2 sent to IOMFB") and leaves the panel to the
// system sleep that follows on a Mac; the panel has no power switch of its own, so the backlight driver darkens it itself, with or
// without that sleep. The level the system asked for is kept and published, and comes back when the lid opens.
static inline uint32_t lid_backlight_level(uint32_t level, bool closed)
{
    return closed ? 0 : level;
}

// Whether the panel is dark: the lid closed, or the system asleep (platform sleep: the panel has no power switch of its own, so its
// backlight is what goes off while the system sleeps, and stays off after the wake until lid_backlight_wake_ready).
static inline bool lid_backlight_dark(bool lid_closed, bool asleep) {return lid_closed || asleep;}

// Woken from platform sleep, the panel stays dark until the compositor presents again. The system's has-powered-on notification comes
// before WindowServer turns the display on (about 25 ms) and before its first frames (about 0.25 s): lit then, the panel showed the
// framebuffer as the display pipeline found it, black and then colour bars for under a second. The frames are those the display shows
// (NativeLegacyDisplay native_legacy_swapstat: copies, flips and latches); LID_BACKLIGHT_WAKE_FRAMES new ones light it, and so does
// LID_BACKLIGHT_WAKE_TIMEOUT_MS without any, so that a screen with nothing to redraw is never left dark. Without those counts
// (counted false), a fixed LID_BACKLIGHT_WAKE_BLIND_MS.
#define LID_BACKLIGHT_WAKE_FRAMES 2u
#define LID_BACKLIGHT_WAKE_TIMEOUT_MS 1500u
#define LID_BACKLIGHT_WAKE_BLIND_MS 600u
static inline bool lid_backlight_wake_ready(bool counted, uint32_t frames_at_wake, uint32_t frames_now, uint64_t ms_since_wake)
{
    if (!counted) return ms_since_wake >= LID_BACKLIGHT_WAKE_BLIND_MS;
    return (uint32_t)(frames_now - frames_at_wake) >= LID_BACKLIGHT_WAKE_FRAMES || ms_since_wake >= LID_BACKLIGHT_WAKE_TIMEOUT_MS;
}

#endif
