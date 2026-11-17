# macOS Wi-Fi and sleep wake

Apple80211 named scans advertise their capability and forward requested channels
to the supplicant. The macOS 27.3 result record carries its continuous-clock
last-seen timestamp at offset 0x8d0. BSS ages include sleep time.

The module retains the joined network and expires entries unheard for two
minutes. The board omits results older than 60 seconds, preserves the channel
during authentication and the first 15 seconds of a link, and refreshes settled
links by channel at intervals of at least 20 seconds.

Sleep preparation clears the lid pin's latched status for both lid positions.
SIGP recovery requires a running CPU and preserves shutdown's signal state.

## Verification

Host checks passed for driver lifecycle, cfg80211 control, the real supplicant
with simulated radio operations, scan channels (45), result timestamps (19),
scan policy (99), exported ABI (24 functions), sleep (467) and association.

Full image CI: [44089097226](https://github.com/UniXNU/UniXNU-sheng/actions/runs/44089097226).

The source author's 12 GiB sheng records show scans completing in 7.2–9.0 seconds
and repeated sleep/wake cycles. See [Ath12kWifi #7](https://github.com/UniXNU/Ath12kWifi/pull/7),
[sheng #128](https://github.com/UniXNU/UniXNU-sheng/pull/128) and
[sheng #129](https://github.com/UniXNU/UniXNU-sheng/pull/129).
The complete wake-to-auto-join sequence on the 8 GiB unit remains pending.

Integration: [Ath12kWifi #9](https://github.com/UniXNU/Ath12kWifi/pull/9) and
[sheng #134](https://github.com/UniXNU/UniXNU-sheng/pull/134).
