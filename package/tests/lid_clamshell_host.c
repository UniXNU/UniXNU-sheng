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

int main(void)
{
    open_at_start_is_not_told();
    each_change_is_told_once();
    closed_at_start_is_told();
    quick_bounces_alternate();
    closed_lid_darkens_the_backlight();
    printf("checks %u\n", checks);
    return 0;
}
