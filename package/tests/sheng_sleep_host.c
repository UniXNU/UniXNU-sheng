// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host tests of sheng's platform sleep wake sources (package/ShengSleep.h): the TLMM interrupt enables kept while asleep, the lid's
// detection, the power key's check and the wake press the buttons thread does not report.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../ShengSleep.h"

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

// The lid pin as KeyboardProbe sets it: target AP (3 << 5), raw status (bit 4), both edges (3 << 2), polarity (bit 1), enabled.
#define LID_CFG 0x7fu

static void fill(uint32_t cfg[SHENG_SLEEP_GPIO_COUNT])
{
    for (uint32_t i = 0; i < SHENG_SLEEP_GPIO_COUNT; ++i) cfg[i] = 0x60u | ((i % 4u) << 2) | ((i & 1u) << 1) | 1u;
    cfg[SHENG_SLEEP_LID_GPIO] = LID_CFG;
}

static void tlmm_keep_only_lid(void)
{
    uint32_t cfg[SHENG_SLEEP_GPIO_COUNT], out[SHENG_SLEEP_GPIO_COUNT];
    fill(cfg);
    sheng_sleep_tlmm_keep(cfg, out);
    for (uint32_t i = 0; i < SHENG_SLEEP_GPIO_COUNT; ++i) {
        if (i == SHENG_SLEEP_LID_GPIO) {
            CHECK(out[i] == cfg[i]);
        } else {
            CHECK((out[i] & SHENG_SLEEP_INTR_ENABLE) == 0);
            CHECK((out[i] | SHENG_SLEEP_INTR_ENABLE) == cfg[i]);   // polarity, detection, target unchanged
        }
    }
}

// A pin routed to another processor (INTR_CFG target bits 7:5 not 3, the application processor) is that processor's: left as it is.
static void tlmm_keep_leaves_other_targets(void)
{
    uint32_t cfg[SHENG_SLEEP_GPIO_COUNT], out[SHENG_SLEEP_GPIO_COUNT];
    fill(cfg);
    cfg[10] = (0u << 5) | (1u << 2) | 1u;               // target 0
    cfg[11] = (4u << 5) | (3u << 2) | 1u;               // target 4
    sheng_sleep_tlmm_keep(cfg, out);
    CHECK(out[10] == cfg[10] && out[11] == cfg[11]);
    CHECK((out[12] & SHENG_SLEEP_INTR_ENABLE) == 0);    // an application processor pin next to them is still turned off
}

static void tlmm_two_rounds(void)
{
    uint32_t cfg[SHENG_SLEEP_GPIO_COUNT], copy[SHENG_SLEEP_GPIO_COUNT], a[SHENG_SLEEP_GPIO_COUNT], b[SHENG_SLEEP_GPIO_COUNT];
    fill(cfg);
    memcpy(copy, cfg, sizeof cfg);
    sheng_sleep_tlmm_keep(cfg, a);
    sheng_sleep_tlmm_keep(cfg, b);
    CHECK(memcmp(a, b, sizeof a) == 0);
    CHECK(memcmp(cfg, copy, sizeof cfg) == 0);
}

static void reserved_pins_not_usable(void)
{
    CHECK(sheng_sleep_pin_usable(35));
    for (uint32_t i = 36; i < 40; ++i) CHECK(!sheng_sleep_pin_usable(i));
    CHECK(sheng_sleep_pin_usable(40));
    CHECK(sheng_sleep_pin_usable(SHENG_SLEEP_LID_GPIO));
    CHECK(sheng_sleep_pin_usable(SHENG_SLEEP_GPIO_COUNT - 1));
    CHECK(!sheng_sleep_pin_usable(SHENG_SLEEP_GPIO_COUNT));
}

static void lid_open_wakes(void) {CHECK(sheng_sleep_lid_wakes(1u) == 1); CHECK(sheng_sleep_lid_wakes(3u) == 1);}
static void lid_closed_sleeps(void) {CHECK(sheng_sleep_lid_wakes(0u) == 0); CHECK(sheng_sleep_lid_wakes(2u) == 0);}

// Closed when the sleep began: level high while asleep, so a lid already open when the wait starts holds the interrupt pending.
static void lid_level_high_when_closed_at_sleep(void)
{
    CHECK(sheng_sleep_lid_cfg(LID_CFG, 1) == 0x73u);   // detection 0 (level), polarity high, enabled; target and raw status kept
    CHECK(sheng_sleep_lid_cfg(0x6du, 1) == 0x63u);
    CHECK(sheng_sleep_lid_cfg(0x6cu, 1) == 0x63u);     // enabled even if it was not
}

// Open when the sleep began (the menu's Sleep): the lid keeps its edges; closing it does not wake, opening it again does.
static void lid_unchanged_when_open_at_sleep(void)
{
    CHECK(sheng_sleep_lid_cfg(LID_CFG, 0) == LID_CFG);
}

static void lid_open_before_wait_wakes_at_once(void)
{
    const uint32_t cfg = sheng_sleep_lid_cfg(LID_CFG, 1);
    CHECK((cfg & (3u << 2)) == 0 && (cfg & (1u << 1)) && (cfg & SHENG_SLEEP_INTR_ENABLE));   // pending while the line is high
    CHECK(sheng_sleep_lid_wakes(1u) == 1);
}

static void power_pressed_wakes(void) {CHECK(sheng_sleep_power_wakes(0x80u, 0) == 1); CHECK(sheng_sleep_power_wakes(0xc0u, 0x40u) == 1);}
static void power_latched_counts_as_pressed(void) {CHECK(sheng_sleep_power_wakes(0, 0x80u) == 1);}
static void power_idle_sleeps(void)
{
    CHECK(sheng_sleep_power_wakes(0, 0) == 0);
    CHECK(sheng_sleep_power_wakes(0x40u, 0x40u) == 0);   // volume down (RESIN) only
    CHECK(sheng_sleep_power_wakes(0x01u, 0x01u) == 0);
}

static void wake_press_and_release_swallowed(void)
{
    int pending = 1;
    CHECK(sheng_sleep_swallow(&pending, 1) == 0);
    CHECK(pending == 1);
    CHECK(sheng_sleep_swallow(&pending, 1) == 0);   // still held
    CHECK(sheng_sleep_swallow(&pending, 0) == 0);   // its release
    CHECK(pending == 0);
}

static void next_press_reported(void)
{
    int pending = 1;
    (void)sheng_sleep_swallow(&pending, 1);
    (void)sheng_sleep_swallow(&pending, 0);
    CHECK(sheng_sleep_swallow(&pending, 1) == 1);
    CHECK(sheng_sleep_swallow(&pending, 0) == 0);
    CHECK(pending == 0);
}

// The press and release were both over before the thread looked: the first look ends the swallowing.
static void released_before_first_look(void)
{
    int pending = 1;
    CHECK(sheng_sleep_swallow(&pending, 0) == 0);
    CHECK(pending == 0);
    CHECK(sheng_sleep_swallow(&pending, 1) == 1);
}

static void woke_by_names(void)
{
    CHECK(strcmp(sheng_sleep_woke_by(SHENG_SLEEP_ARMCPU_TIMER, ShengWakeNone), "timer") == 0);
    CHECK(strcmp(sheng_sleep_woke_by(SHENG_SLEEP_ARMCPU_SOURCE, ShengWakeLid), "lid") == 0);
    CHECK(strcmp(sheng_sleep_woke_by(SHENG_SLEEP_ARMCPU_SOURCE, ShengWakePower), "power") == 0);
    CHECK(strcmp(sheng_sleep_woke_by(SHENG_SLEEP_ARMCPU_TIMER, ShengWakePower), "power") == 0);   // a source the board accepted wins
    CHECK(strcmp(sheng_sleep_woke_by(0, ShengWakeNone), "nothing") == 0);
    CHECK(strcmp(sheng_sleep_woke_by(3u, ShengWakeNone), "storm") == 0);   // ArmcpuWakeStorm: a line kept firing
}

// The lid pin's status is cleared before every wait, the lid open too: an edge latched while the system was awake (the lid opened or
// closed some time before; KeyboardProbe polls the pin and leaves the status) raised the summary the moment the wait began, and with
// the lid open that is a wake: the system woke 3 ms after it slept (seen on the tablet: sleeps 1, woke by lid).
static void lid_status_cleared_open_or_closed(void)
{
    CHECK(sheng_sleep_clear_lid_status(1));
    CHECK(sheng_sleep_clear_lid_status(0));
}

int main(void)
{
    tlmm_keep_only_lid();
    tlmm_two_rounds();
    tlmm_keep_leaves_other_targets();
    reserved_pins_not_usable();
    lid_open_wakes();
    lid_closed_sleeps();
    lid_level_high_when_closed_at_sleep();
    lid_unchanged_when_open_at_sleep();
    lid_open_before_wait_wakes_at_once();
    power_pressed_wakes();
    power_latched_counts_as_pressed();
    power_idle_sleeps();
    wake_press_and_release_swallowed();
    next_press_reported();
    released_before_first_look();
    woke_by_names();
    lid_status_cleared_open_or_closed();
    printf("%u checks\n", checks);
    return 0;
}
