// SPDX-License-Identifier: BSD-2-Clause-Patent
// sheng's wake sources for platform sleep (boot-arg platform-sleep=1; drivers/ArmCpu/ArmCpuSleep.h), the logic ShengSleep.cpp and
// ButtonProbe.cpp apply while the boot core waits alone with interrupts masked:
// - the lid: the hall sensor on TLMM GPIO 171 (active low: 0 closed), on the TLMM summary interrupt, SPI 208 (INTID 240). Asleep, every
//   other pin that interrupts the application processor has its INTR_CFG enable off, so that only the lid can raise the summary. When
//   the lid was closed as the sleep began (or is closed when the wait is prepared), the lid pin is level high while asleep: a lid opened
//   before the wait started keeps the interrupt pending, so the wait ends at once.
// - the power key: PMK8550 PON interrupt bit 7, on the SPMI arbiter's interrupt (PDC pin 1, SPI 481, INTID 513). Pressed in its
//   real-time status or in its latched status (a short press may be over before the check runs).
// The press that woke the system and its release are not reported to macOS. Host-tested by tests/sheng_sleep_host.c.
#ifndef SHENG_SLEEP_H
#define SHENG_SLEEP_H
#include <stdint.h>

#define SHENG_SLEEP_GPIO_COUNT 210u
#define SHENG_SLEEP_LID_GPIO 171u
// gpio-reserved-ranges = <36 4> (sm8550-xiaomi-sheng.dts): secure pins, where any access aborts.
#define SHENG_SLEEP_RESERVED_FIRST 36u
#define SHENG_SLEEP_RESERVED_COUNT 4u
#define SHENG_SLEEP_LID_INTID (32u + 208u)
#define SHENG_SLEEP_POWER_INTID (32u + 481u)
// INTR_CFG: bit 0 enable, 1 polarity, 3:2 detection (0 level, 1 rising, 2 falling, 3 both); bit 4 raw status and 7:5 target untouched.
#define SHENG_SLEEP_INTR_ENABLE 1u
#define SHENG_SLEEP_INTR_POLARITY (1u << 1)
#define SHENG_SLEEP_INTR_DETECTION (3u << 2)
// INTR_CFG bits 7:5, the processor the pin interrupts: 3 the application processor (KeyboardProbe.cpp sets it so).
#define SHENG_SLEEP_INTR_TARGET(cfg) (((cfg) >> 5) & 7u)
#define SHENG_SLEEP_TARGET_AP 3u
#define SHENG_SLEEP_POWER_BIT 0x80u
// armcpu_sleep_stats.last_reason (ArmCpuSleep.h: ArmcpuWakeSource, ArmcpuWakeTimer, ArmcpuWakeStorm).
#define SHENG_SLEEP_ARMCPU_SOURCE 1u
#define SHENG_SLEEP_ARMCPU_TIMER 2u
#define SHENG_SLEEP_ARMCPU_STORM 3u

enum sheng_wake_source {ShengWakeNone, ShengWakeLid, ShengWakePower};

static inline int sheng_sleep_pin_usable(uint32_t gpio)
{
    return gpio < SHENG_SLEEP_GPIO_COUNT &&
           !(gpio >= SHENG_SLEEP_RESERVED_FIRST && gpio < SHENG_SLEEP_RESERVED_FIRST + SHENG_SLEEP_RESERVED_COUNT);
}

// The INTR_CFG values while asleep: the enable off on every pin that interrupts the application processor, except the lid's, which is
// left as it is (sheng_sleep_lid_cfg sets it). Pins routed to another processor are that processor's and stay as they are.
static inline void sheng_sleep_tlmm_keep(const uint32_t cfg[SHENG_SLEEP_GPIO_COUNT], uint32_t out[SHENG_SLEEP_GPIO_COUNT])
{
    for (uint32_t i = 0; i < SHENG_SLEEP_GPIO_COUNT; ++i) {
        const int ours = i != SHENG_SLEEP_LID_GPIO && SHENG_SLEEP_INTR_TARGET(cfg[i]) == SHENG_SLEEP_TARGET_AP;
        out[i] = ours ? cfg[i] & ~SHENG_SLEEP_INTR_ENABLE : cfg[i];
    }
}

// Clear the lid pin's latched status before every sleep wait.
static inline int sheng_sleep_clear_lid_status(int closed) {(void)closed; return 1;}

// The lid pin's INTR_CFG while asleep: level high and enabled when the lid was closed as the sleep began, else unchanged.
static inline uint32_t sheng_sleep_lid_cfg(uint32_t cfg, int closed_when_sleep_began)
{
    if (!closed_when_sleep_began) return cfg;
    return (cfg & ~SHENG_SLEEP_INTR_DETECTION) | SHENG_SLEEP_INTR_POLARITY | SHENG_SLEEP_INTR_ENABLE;
}

// The lid pin's IO register: bit 0 the level, high when open.
static inline int sheng_sleep_lid_wakes(uint32_t lid_io) {return (lid_io & 1u) != 0;}

static inline int sheng_sleep_power_wakes(uint8_t rt_sts, uint32_t latched)
{
    return ((rt_sts | latched) & SHENG_SLEEP_POWER_BIT) != 0;
}

// The power state the buttons thread reports. While *pending (the press woke the system), the press and its release are dropped; the
// first look at a released key ends it.
static inline int sheng_sleep_swallow(int *pending, int pressed)
{
    if (!*pending) return pressed;
    if (!pressed) *pending = 0;
    return 0;
}

// For the wake log line: a source the board accepted, else the wake-after timer, a storm (a line kept firing and nothing was left to
// wake the system), or nothing (a sleep that did not wait).
static inline const char *sheng_sleep_woke_by(uint32_t armcpu_reason, int source)
{
    if (source == ShengWakeLid) return "lid";
    if (source == ShengWakePower) return "power";
    if (armcpu_reason == SHENG_SLEEP_ARMCPU_TIMER) return "timer";
    return armcpu_reason == SHENG_SLEEP_ARMCPU_STORM ? "storm" : "nothing";
}

#endif
