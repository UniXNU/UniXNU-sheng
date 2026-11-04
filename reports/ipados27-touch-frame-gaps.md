# Touch and frame timing on sheng

Measurements are from a Xiaomi Pad 6S Pro running iPadOS 27.0.1 build 24A446. A flip interval is the time between consecutive display flips in `kernel.rings.txt`. A long interval can include idle time. The continuous-input measurements below require a HID report at least every 16.7 ms, including at both ends of the interval.

## Icon rendering and memory

With `sheng-lite=1`, launchd starts `iconservicesagent` on demand. The job sets `FORCE_SOFTWARE_RENDERER=0` and `CI_ENABLE_METAL_GPU=0`. IconRendering uses the Adreno 740 Metal device. Core Image has Metal disabled for this process. The stock J820 Jetsam profile sets a 15 MiB inactive hard limit and a 1,224 MiB active soft limit.

| Hardware observation | Evidence |
| --- | --- |
| GPU rendering | On boot `41dcdcce`, PID 168 obtained a Metal device and command queue. Its first eight completed command buffers contained 149 draws. All 495 GPU submissions retired. |
| Cold icon cache | On `ico4-lock-fix` boot `41ee75ce`, Settings and App Store displayed complete Home Screen icons. Their rebuilt 68 pt cache entries matched the complete reference artwork byte for byte. |
| Settings artwork | The same cache contained transparent 56 × 56 px entries for Wi-Fi, Battery and About. Wi-Fi also had a populated 120 × 120 px entry. The cache records variants independently of the one displayed. |
| Process lifetime | On `41ee75ce`, all 488 GPU submissions from PID 139 retired. Launchd recorded one Jetsam termination for the per-process memory limit after 173.752 s of process life. |
| Startup cache | On boot `3fc07c73`, PID 1 exited with `No service cache`. The mounted `launchd.plist` contained the expected 1,327,067 bytes followed by 30 bytes from a longer file. On `41ee75ce` the file matched the image in length and SHA-256. |

The cold-cache image SHA-256 is `c2f67213c102d8c95f3249eacd3a0a573bf859f08bf876c26c7e90bfb5973a1d`.

## SpringBoard waits and frame intervals

On boot `da1dbd496`, SpringBoard's main thread spent 4.644 s in a synchronous XPC call to `iconservicesagent` PID 387. The wait overlapped 4.444 s of a 4.583 s flip interval. The preceding agent process exited on SIGSEGV in Core Image 1.254 s before the call began.

The comparison workload used Home Screen page changes, Settings launches and App Library. Both 240-second captures used the same GPU addon and touch driver. With the service disabled, the Home Screen and Dock displayed icon backgrounds without app artwork.

| Metric | Agent enabled, `da1dbd496` | Agent disabled, `50d4021e7` |
| --- | ---: | ---: |
| Observed agent processes / SIGSEGV exits | 6 / 5 | 0 / 0 |
| Continuous-input flip interval, median / P95 / P99 (ms) | 15.358 / 28.067 / 43.232 | 9.631 / 28.006 / 46.384 |
| Longest continuous-input flip interval (ms) | 131.501 | 75.483 |
| Full-screen-width GPU pass, P95 (ms) | 4.819 | 4.461 |
| HID report interval, median / P99 (ms) | 8.331 / 8.531 | 8.333 / 8.535 |

Individual long frames have distinct causes. In a 242.143 ms interval on boot `532856a9`, 29 HID reports arrived and backboardd submitted at +238.523 ms. In a separate 333.145 ms interval, a 64 MiB allocation inside a backboardd command took 184.016 ms, including 178.904 ms of kernel page wiring.

## CPU profile

With `cs-exact=1 cs-profile=1 sheng-cpufreq=4`, the interrupted CPU records SpringBoard's main-thread PC, LR and FP. Boot `da1dbd496` yielded 6,680 exact samples and 6,679 ready PMU readings. The sampler spent 82.19 ms on 8,019 attempts, or 0.0582% of the observed wall time.

The performance and prime domains requested their maximum frequencies in all 82 governor windows across five busy flip intervals of at least 100 ms. In the 131.501 ms interval with continuous input, the eight EPSS readings were 2,707 and 2,956 MHz respectively. The stack included `-[SBIconListView configureIconView:forIcon:]` in 5 of 171 long-frame samples and 0 of 523 short-frame samples.

| Busy flip intervals | Same-thread, same-core sample pairs | EL0 instructions per cycle |
| --- | ---: | ---: |
| At least 100 ms | 78 | 0.714 |
| Under 30 ms | 130 | 0.838 |

Busy intervals have more than 60% main-thread CPU use and recount coverage of at least 65% of the interval. PMU pairs span at most 20 ms with an unchanged context-switch count.

## Touch delivery

| Metric | `333b621dc`, driver `a1a6f2b` | `532856a9`, driver `3b44b1b` | `5cfa7b18a`, driver `3b44b1b` |
| --- | ---: | ---: | ---: |
| HID report interval, median / P99 (ms) | 8.326 / 8.563 | 8.334 / 8.574 | 8.334 / 8.526 |
| Flip intervals over 50 ms with any HID activity | 3.89% | 5.11% | 5.83% |
| Continuously covered intervals of at least 100 ms | 2 / 1,379 | 15 / 2,375 | 11 / 1,630 |

All three captures used fixed 120 Hz and governor mode 4. Workloads, GPU binaries and sampling settings varied. HID cadence excludes gaps above 50 ms.

On `5cfa7b18a`, IRQ to SPI completion took 2.52 ms at the median and 2.60 ms at P99. SPI completion to return from HID publication took 0.13 and 0.30 ms respectively. Five contact IDs stayed distinct during a 250 ms pause. All 57 rapid typing contacts released. Bottom-edge swipes and multi-finger pinches still failed to open App Switcher, and some rapid taps selected alternate characters.

## Kernel stack sampling and GPU interrupts

The blocked-thread sampler holds the scheduler lock and checks that the thread is waiting, non-runnable and not terminating. It validates the saved state and each frame against the thread's kernel stack. On lock-screen panic `6ba5a54e`, backboardd was both waiting and runnable, with a null saved-state pointer. The guard in `571fcc9` skips pointer authentication in this state.

The GPU hard interrupt queues CP preemption completion on a serialized thread call. The worker takes and releases the preemption mutex. The idle desktop panic on `532856a9` occurred when the hard interrupt released that mutex with a pending turnstile inheritor.

## Reading a capture

Run these commands in the sheng device repository. Set `RUN` to the directory produced by the log reader.

```sh
python3 scripts/decode-sheng-touchpath.py "$RUN/diag/files/kernel.touchpath.bin" --jsonl paths.jsonl
python3 scripts/sheng-latency-report.py "$RUN/diag" --frames-only
python3 scripts/sheng-springboard-profile.py "$RUN/diag/files/kernel.latency.bin"
```
