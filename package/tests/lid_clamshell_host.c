// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of what macOS's power manager is told about the keyboard cover's lid (package/LidClamshell.h).
#include <stdio.h>
#include <stdlib.h>
#include "../LidClamshell.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

static void open_at_start_is_not_told(void)
{
    struct lid_clamshell s = LID_CLAMSHELL_INIT;
    CHECK(lid_clamshell_step(&s, false) == LidClamshellNone);
    CHECK(lid_clamshell_step(&s, false) == LidClamshellNone);
}

static void each_change_is_told_once(void)
{
    struct lid_clamshell s = LID_CLAMSHELL_INIT;
    CHECK(lid_clamshell_step(&s, false) == LidClamshellNone);
    CHECK(lid_clamshell_step(&s, true) == LidClamshellClosed);
    CHECK(lid_clamshell_step(&s, true) == LidClamshellNone);      // the once-a-second poll reads the same state
    CHECK(lid_clamshell_step(&s, false) == LidClamshellOpened);
    CHECK(lid_clamshell_step(&s, false) == LidClamshellNone);
    CHECK(lid_clamshell_step(&s, true) == LidClamshellClosed);    // closed again
}

static void closed_at_start_is_told(void)
{
    struct lid_clamshell s = LID_CLAMSHELL_INIT;
    CHECK(lid_clamshell_step(&s, true) == LidClamshellClosed);
    CHECK(lid_clamshell_step(&s, false) == LidClamshellOpened);
}

static void quick_bounces_alternate(void)
{
    struct lid_clamshell s = LID_CLAMSHELL_INIT;
    for (int i = 0; i < 10; ++i) {
        CHECK(lid_clamshell_step(&s, true) == LidClamshellClosed);
        CHECK(lid_clamshell_step(&s, false) == LidClamshellOpened);
    }
}

static void closed_lid_darkens_the_backlight(void)
{
    CHECK(lid_backlight_level(1500, false) == 1500);
    CHECK(lid_backlight_level(1500, true) == 0);
    CHECK(lid_backlight_level(0, false) == 0);
    CHECK(lid_backlight_level(2047, true) == 0);
}

static void backlight_dark_table(void)
{
    CHECK(!lid_backlight_dark(false, false));
    CHECK(lid_backlight_dark(true, false));
    CHECK(lid_backlight_dark(false, true));
    CHECK(lid_backlight_dark(true, true));
}

// The system going to sleep darkens the panel with the lid open too (the menu's Sleep), and waking brings the level back.
static void asleep_darkens_even_with_lid_open(void)
{
    CHECK(lid_backlight_level(1500, lid_backlight_dark(false, true)) == 0);
    CHECK(lid_backlight_level(1500, lid_backlight_dark(false, false)) == 1500);
}

// Woken, the panel stays dark until the compositor has presented frames again: lit at the wake it showed the framebuffer as the
// display pipeline found it (black, then colour bars for under a second) before the desktop.
static void woken_panel_waits_for_frames(void)
{
    CHECK(!lid_backlight_wake_ready(true, 100, 100, 0));
    CHECK(!lid_backlight_wake_ready(true, 100, 101, 50));
    CHECK(lid_backlight_wake_ready(true, 100, 102, 60));
    CHECK(lid_backlight_wake_ready(true, 100, 140, 300));
}

// No frames at all (nothing on the screen changes): lit after the timeout, never left dark.
static void woken_panel_times_out(void)
{
    CHECK(!lid_backlight_wake_ready(true, 7, 7, LID_BACKLIGHT_WAKE_TIMEOUT_MS - 1));
    CHECK(lid_backlight_wake_ready(true, 7, 7, LID_BACKLIGHT_WAKE_TIMEOUT_MS));
    CHECK(lid_backlight_wake_ready(true, 7, 8, 60000));
}

// The display's counters wrap.
static void frame_count_wraps(void)
{
    CHECK(lid_backlight_wake_ready(true, 0xffffffffu, 1u, 10));
    CHECK(!lid_backlight_wake_ready(true, 0xffffffffu, 0u, 10));
}

// Without the display's frame counts (another framebuffer): a fixed wait.
static void without_frame_counts(void)
{
    CHECK(!lid_backlight_wake_ready(false, 0, 50, LID_BACKLIGHT_WAKE_BLIND_MS - 1));
    CHECK(lid_backlight_wake_ready(false, 0, 0, LID_BACKLIGHT_WAKE_BLIND_MS));
}

int main(void)
{
    open_at_start_is_not_told();
    each_change_is_told_once();
    closed_at_start_is_told();
    quick_bounces_alternate();
    closed_lid_darkens_the_backlight();
    backlight_dark_table();
    asleep_darkens_even_with_lid_open();
    woken_panel_waits_for_frames();
    woken_panel_times_out();
    frame_count_wraps();
    without_frame_counts();
    printf("checks %u\n", checks);
    return 0;
}
