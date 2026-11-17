# macOS sleep and wake

The sheng integration uses the ArmCpu platform-sleep interface and
`patches/xnu-12377/sheng-platform-sleep.patch`. Select `platform-sleep=1` to
register the board wake sources. `platform-sleep-wake-after=N` adds a timer wake.

| Function | Behavior |
| --- | --- |
| CPU suspend | Secondary cores use PSCI CPU_OFF. The boot core waits with the board wake sources enabled. |
| Resume | The driver restores interrupts and timer state, sends self-IPIs and bounds the resume signaling wait. |
| Watchdog | A board tick feeds the inherited firmware watchdog every 10 seconds during sleep. |
| Lid and power key | The board reports the wake source and requests a full user wake. |
| Backlight | Restore after two presented frames, at most 1500 ms; use 600 ms when frame counts are unavailable. |
| Touchpad | A report gap above 60 ms resets the contact position and accumulated motion. |
| Touchscreen | Closing the cover releases contacts and gates reports until the cover opens. |
| Previous log | `kern.unixnu_prev_log` exposes the bounded copy retained when the current UFS log writer starts. |

## Verification

Existing host checks passed: ArmCpu sleep decisions and deadlines, GIC MMIO
instruction forms at `-Os` and `-O2`, board wake sources, backlight readiness,
touchscreen lid gating, keyboard trace, keyboard HID, WN8030 firmware commands,
previous-log decoding and launch policy. Modified driver and board sources
compiled against the configured macOS XNU IOKit, startup and BSD records.
The complete local kernel built and prelinked successfully as
`6fa125bb57e9ed1e832aa465e3181556a52999749bd33c646ef2211e29ffde3e`.
Full image CI: [44039582833](https://github.com/UniXNU/UniXNU-sheng/actions/runs/44039582833).

The source author's device records include a
[60001 ms timer wake](https://github.com/zus12873/UniXNU/commit/1f7c45387aa66ee735efcf8d16db2dc40b76bc69),
a [power-key wake](https://github.com/zus12873/UniXNU/commit/b08b4087a20f07d25a7d4728b300f62bce8c5885)
and a [lid wake](https://github.com/zus12873/UniXNU/commit/db511b2f8789095ae0753ca1f69bc11637aa0a5e).
Those records use the author's 12 GiB unit. Physical sleep and wake verification
on the 8 GiB unit is pending.

Integration: [ArmCpu #7](https://github.com/UniXNU/ArmCpu/pull/7),
[UniXNU #299](https://github.com/UniXNU/UniXNU/pull/299) and
[UniXNU-sheng #127](https://github.com/UniXNU/UniXNU-sheng/pull/127).
