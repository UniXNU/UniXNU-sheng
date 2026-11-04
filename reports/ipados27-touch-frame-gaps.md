# Touch and frame timing on sheng

Tests used iPadOS 27.0.1 build 24A446 on a Xiaomi Pad 6S Pro. The four images were started with `fastboot boot`. The hardware tests did not qualify App Switcher gestures or keyboard flicks.

| Run | Board revision | Boot image SHA-256 | Observation |
| --- | --- | --- | --- |
| sch1, boot 532856a9 | `8fcf9a4e0f3c5a23a820f85fd25189e2c7e83f87` | `445f10c4c6a257f48551b267b9c0d740c17150f3ca5a2e2a51db24cfd1ca2a94` | The tester reported limited improvement and an idle desktop freeze. The panic flush recorded state 5. |
| sch2, boot 5cfa7b18a | `8ec842f38c21ef1d73e12000fae16df7442e0c73` | `15ff435d078f17bdad4bda870b6a85b268b5284217c93a05e227d3b785be27a9` | Both App Switcher gestures still failed and keyboard flicks recurred. The desktop responded before a manual reboot. |
| sch3, boot da1dbd496 | `948a8b8` (profile code) | `060d6c256f1acc18af5d54d40762cdcddf398fc370ae8df45029002f87d9c776` | The tester reached the desktop and exercised it. A complete 240-second diagnostic snapshot was recovered at log uptime 276 seconds. The reader returned the tablet to fastboot. |
| sch4, boot 50d4021e7 | `fe636ce` (launch policy) | `3f42a416c4f9a6a5fe123c65eafe93e8660ba014c6927f416d0e14bcfde21ea4` | The tester could open Settings. All app icon artwork on the photographed Home Screen and Dock was missing. |

## Sampling boundaries

The 240-second sch1 snapshot retained 6,195 HID timestamps, 8,621 frequency windows and 60,638 thread samples. The 360-second sch2 snapshot used `sheng-tlog=1` and `cs-exact=0`. Manual reboot preceded publication of the final export length. Read-only recovery from the first diagnostic half recovered a complete 17-file stream. The embedded boot arguments, ring formats, lengths and cross-ring timestamps identify sch2. The earlier competing boot returned after one log slot and provided no interaction data.

TPR1 stored 16,384 of 18,988 contact rows. The oldest 2,604 rows, including the beginning of the test sequence, were overwritten. The independent TCH1 ring retained all 3,962 report timestamps. TPR1 covered 2,975 observed contact frames and 2,896 successful HID report attempts. The median and 99th percentile from the most recent IRQ to SPI completion were 2.52 and 2.60 ms. From SPI completion to return from publishing a frame with a HID report, they were 0.13 and 0.30 ms. These timestamps end in the kernel. Delivery to backboardd, UIKit gesture recognition and the keyboard's selected character were not recorded.

## Gestures and typing

- The user reported zero successful bottom-edge and multi-finger attempts in sch2. The ring did not retain the initial bottom-edge test in sufficient detail to classify those starts.
- Four retained five-finger sequences had 186, 63, 155 and 197 five-contact frames. The five IDs remained distinct. In one contraction, median nearest-neighbour distance fell from 321 to 235 panel px. During a subsequent 250 ms pause, every finger stayed within a 1.75 px diameter. Other retained five-finger sequences moved apart. The host replay that previously collapsed five peaks into one is covered by the component regression test.
- In a 12.7-second typing sequence, all 57 contacts had a successful release and all 56 adjacent pairs separated into distinct paths. Downward output displacement from each initial contact had a median of 0 px, a 95th percentile of 10.4 px and a maximum of 16.6 px. The UI's key-flick threshold and the specific tap that selected an alternate character remain unknown.
- The per-report synchronous deltas of the HID edge counters were zero throughout the retained trajectory. Those deltas cannot distinguish asynchronous event dispatch from an inactive hook. The tester confirmed palm or multi-finger contact during the final high-contact-count burst. This burst cannot identify phantom touches on an unattended screen.

## SpringBoard frame gaps

### Before and after the touch change

Claude's earlier SpringBoard analysis used boot `333b621dc`, recorded on November 3 as `seven-trim/run5`. The retained `boot-lazy1.img` SHA-256 is `bdf3217018a7385d7e9d2ca4936ccfb41e70d81f07fe7da57b2e5568efaf73ed`. The selected Novatek revision was `a1a6f2b`. Both November 4 boots used `3b44b1b`. All three configured fixed 120 Hz and governor mode 4. Lazy1 and sch1 used the same boot arguments apart from the build tag. The Metal plugin and Turnip library embedded in lazy1 both differ from those in the two later images. The gestures and diagnostic sampling also differed between runs.

| Hardware metric | lazy1, before | sch1, after | sch2, after |
| --- | ---: | ---: | ---: |
| HID reports retained | 2,921 | 6,195 | 3,962 |
| Adjacent HID reports, P50 / P99 (ms) | 8.326 / 8.563 | 8.334 / 8.574 | 8.334 / 8.526 |
| Flips with HID activity, interval P50 / P99 (ms) | 14.544 / 100.975 | 17.580 / 129.162 | 15.876 / 106.805 |
| Flip intervals over 50 ms with HID activity | 58 / 1,491 (3.89%) | 125 / 2,446 (5.11%) | 97 / 1,663 (5.83%) |
| Continuously covered intervals of at least 100 ms | 2 / 1,379 (0.15%) | 15 / 2,375 (0.63%) | 11 / 1,630 (0.67%) |
| Full-screen-width GPU pass, P95 (ms) | 5.386 | 4.601 | 4.656 |

The HID cadence calculation excludes inter-report gaps above 50 ms. The original flip metric includes any interval containing at least one HID report. The continuous subset requires at least one report and a maximum gap of 16.7 ms between consecutive reports and at either flip boundary. A stationary contact may qualify. GPU pass durations use the existing report's touch windows and width threshold of 3,000 px. A pass covers part of a frame. In two additional pre-change captures, `seven-trim/run1` and `run3`, the fraction over 50 ms was 5.20% and 2.77%. The captures show similar HID cadence and persistent long flip intervals. They cannot isolate the effect of changing the touch driver. The pre-change image did not record paired IRQ and HID timestamps for a driver-latency comparison.

### Where time was spent

The flip event is event 4 in `kernel.rings.txt`. A flip interval containing any touch can include idle time before the first touch or after lift. The following intervals provide continuous input coverage.

| Run and start (uptime s) | Flip interval | Observed input | Observed rendering |
| --- | ---: | --- | --- |
| sch2 1454.762454 | 100.273 ms | Five IDs throughout 12 reports, 11 moving frames. One moved 25.1 panel px. | SpringBoard main thread used approximately 14% of the sampled span. The only GPU submission in the interval came from backboardd at +99.425 ms and retired at +99.944 ms. |
| sch2 1411.645516 | 143.153 ms | 17 HID timestamps with a maximum gap of 8.39 ms. The underlying coordinates had already aged out. | SpringBoard main thread used approximately 84%. Median performance/prime cluster readbacks were 2,707/2,956 MHz. |
| sch1 273.109302 | 242.143 ms | 29 HID timestamps with a maximum gap of 8.44 ms. | SpringBoard main thread used approximately 5%. Backboardd submitted the next buffer at +238.523 ms. |

SpringBoard CPU shares use monotonic main-thread recount samples spanning at least 65% of the interval. In sch1, 8,536 SpringBoard main-thread samples and 9,842 backboardd samples captured exact user state. The sch1 cache slide read from the corresponding target records was 0xd6c000. Sampled busy intervals showed dispersed execution in Objective-C dispatch, retain operations and framework code. The frequent instruction addresses did not isolate a single dominant function.

The sch2 display summary recorded 120 Hz, one timer startup and one reported refresh configuration. It recorded zero timer arms and sends, with the vsync ring allocated. This measures the port's IOMFB client notification path. On 24A446 the original IOMFB type 5 sender and the driver read the same client-list offset, 0x1ab8. QuartzCore can also schedule through CADisplayTimer. Neither per-client callback was traced. The observed gaps rule out extended silence in the touch driver for the continuously covered examples. Neither the vsync counters nor HID returns establish delivery of a display callback or touch event to SpringBoard.

At sch1 uptime 277.937616 s, 40 HID reports arrived during a 333.145 ms flip interval. A backboardd command was created 0.1 ms into the gap. Between its 34th and 35th passes, a synchronous 64 MiB allocation took 184.016 ms, with 178.904 ms in kernel page wiring. A background allocation of the same size and pool overlapped. SpringBoard's sampled stacks included a CoreAnimation transaction commit. In contrast, in the 242.143 ms sch1 interval above, the preceding backboardd command had already finished before the gap began, and the next command was created only at +233.6 ms. SpringBoard's 44 blocked samples were in the Mach message receive path. The late command creation and the delay inside an existing command require separate investigation.

### Governor and interrupted SpringBoard stacks

For this analysis a busy long interval is at least 100 ms between consecutive flips with at least one HID report and SpringBoard main-thread CPU use above 60%. The first and last valid main-thread recount samples must span at least 65% of the interval. Controls use the same gates with a flip interval below 30 ms. Times, CPU shares and sample counts refer to those intervals only.

| Capture | Busy long intervals | Governor windows | Both performance domains at their maximum requested levels | Median EPSS readbacks, performance / prime |
| --- | ---: | ---: | ---: | ---: |
| sch1 | 12 | 126 | 125 | 2,592 / 2,956 MHz |
| sch2 | 3 | 24 | 24 | 2,707 / 2,956 MHz |

Maximum governor votes were 2,803 MHz for CPUs 3–6 and 2,956 MHz for CPU 7. In sch1, among windows where at least one CPU in the corresponding domain was 50% busy, the EPSS readback fell below 80% of the vote in 34 of 125 performance-domain windows and 14 of 102 prime-domain windows. Matched short frames also showed this behavior (253 of 803 and 86 of 694). The 333.145 ms sch1 interval stayed at 2,707 / 2,956 MHz for all 20 readings. In the sole lower-vote window across the two boots, the next HID report came 4.405 ms after that reading. The next governor poll raised both votes and the following poll observed 2,707 / 2,956 MHz. The per-domain register is an instantaneous EPSS reading. These two boots did not record which CPU took an exact sample.

The 12 sch1 long intervals yielded 290 user-mode interrupts of the SpringBoard main thread, compared with 2,168 in 665 similarly busy short intervals. Symbolization used the 24A446 cache, its sch1 slide 0xd6c000, and the function bounds of each unslid address. The interrupted PC was in `objc_msgSend` in 45 of 290 versus 292 of 2,168. The stack included `-[UIView(Internal) _didMoveFromWindow:toWindow:]` in 10 of 290 (four gaps) versus 11 of 2,168, and `-[FBScene updateSettings:withTransitionContext:completion:]` in 6 of 290 (six gaps) versus 8 of 2,168. Of the long-frame PCs, 28 did not resolve to a named function. Caller counts include each function at most once per sample. These are conditional sample shares, not per-function elapsed times. The handlers capture the interrupted PC, LR and FP. The later userspace FP walk is best effort. Sch2 collected no exact samples.

The generated sheng XNU configuration defines `CONFIG_CPU_COUNTERS 0`. Neither capture supplies retired instructions or CPU cycles. IPC and a quantitative comparison with an Apple CPU cannot be derived from these traces.

## Main-thread profile on sch3

The recovered 16-file stream contained 231,686 latency records with zero reported drops or overwrites, 57,108 client samples, 8,485 governor windows and 2,345 HID timestamps. The image retained sch2's touch object and GPU addon. Boot arguments included `cs-profile=1 cs-exact=1 sheng-cpufreq=4`. SpringBoard PID 30, main TID 320, used cache slide 0x458000. Client records with peer −2 identified 6,680 exact EL0 samples. The interrupted CPU was in 3–6 or 7 for 6,678 of them. Only SpringBoard received profiling IPIs. Across 125 complete cost windows the sampler spent 82.19 ms on 8,019 attempts, 0.0582% of the observed wall time. The largest measured attempt was 69.1 µs. This measures the sampler-side cross-call path.

### Mach waits

The capture retained 16,442 completed main-thread Mach calls of at least 250 µs and one call still waiting at export. For direct calls with a resolved user peer, backboardd accounted for 283 calls (median receive time 0.363 ms, maximum 358.180 ms), runningboardd for 223 (median 1.795 ms, maximum 4.736 ms) and launchd for 87 (median 0.472 ms, maximum 0.650 ms). The 358.180 ms backboardd wait came from `CA::Context::commit_transaction`, request ID 40209. There were two flips and zero HID reports while SpringBoard awaited this reply.

At uptime 3237.363240 s SpringBoard entered an explicit synchronous XPC receive. Its destination was unresolved at entry. The same blocked thread's stack contained `xpc_connection_send_message_with_reply_sync` and `NSXPCConnection`. Samples at +3.377 and +4.510 s resolved its remote port to PID 387, which the kernel process identity record named `iconservicesagent`. The call returned successfully after 4.644428 s. It overlapped 4.444334 s of a 4.583440 s flip interval. During that interval 85 HID reports arrived in bursts, with a maximum uncovered span of 1.267 s.

Five separate iconservicesagent PIDs (123, 359, 366, 373 and 377) faulted at the same CoreImage instruction. PID 377 exited on SIGSEGV 1.254 s before the XPC wait began, and PID 387 appeared during the wait. At unslid PC `0x1a575982c` the 24A446 instruction `str d0, [x8, #0x28]` wrote to address 0x28 with x8 zero. It lies in the block inside `CI::ProgramNode::create_program_and_arguments`. The blocked stack and remote port identify the service involved in this stall. The trace does not separate launch delay from time spent servicing the request. The code path that supplied the null program state and a fix for the service still require verification.

### Running CPU and instruction throughput

| Group | Intervals between exact samples with the same running thread, CPU and context-switch count | Retired instructions per EL0 cycle |
| --- | ---: | ---: |
| All qualifying pairs | 1,521 | 0.803 |
| Within the five busy flip intervals of 100 ms or longer | 78 | 0.714 |
| Within the 191 similarly busy flips under 30 ms | 130 | 0.838 |

The board read two architectural PMU events on 6,679 exact samples. One sample on CPU 0 reported occupied counters. Each aggregate is a ratio of summed instruction and cycle deltas. Both endpoints must report a ready PMU and the sample spacing must be at most 20 ms. Per-core aggregate IPC was 0.809 on CPU 3, 0.793 on 4, 0.738 on 5, 0.798 on 6 and 0.935 on 7. These are selected execution windows on sheng. Establishing the hardware difference from an Apple CPU would require a comparable measurement on that device.

Across the five busy long flips, both performance domains requested their maximum allowed frequencies in all 82 governor windows. The one continuously HID-covered case spanned 131.501 ms, with 15 reports and SpringBoard main-thread CPU share 74.3%. All eight governor windows read 2,707 / 2,956 MHz. The main thread spent about 31.0 ms in undirected Mach receives, 4.58 ms in receives directed to runningboardd and 0.28 ms in receives directed to backboardd. Six uninterrupted PMU intervals gave 0.761 instructions per cycle.

The five long flips contained 171 exact SpringBoard samples, compared with 523 in similarly busy short flips. `objc_msgSend` was the interrupted instruction's function in 29/171 versus 69/523. The stack included `+[UIView(Animation) performWithoutAnimation:]` in 10/171 versus 1/523 and `-[SBIconListView configureIconView:forIcon:]` in 5/171 versus 0/523. Function hits overlap and give no exclusive elapsed time. The action boundaries were not marked on the device. The GPU and touch binaries match sch2, but this workload and sampler configuration differ from the earlier touch tests.

## Disabling iconservicesagent on sch4

Both boots used the same tablet, OS, 240-second export and SpringBoard sampling configuration. The tester repeated Home Screen page switches, opening Settings and returning home, and opening and closing App Library. The on-device process samples identify Settings (Preferences) on both boots. Image verification confirmed the same GPU addon and Novatek object. The boot line changed only the build tag and `sheng-lite=1` to `sheng-lite=2`. Comparing the two generated launchd plists byte for byte after removing the extra key verified that only `com.apple.iconservices.iconservicesagent` was added to the disabled set. Sch4 completed startup, which checks for a successful policy write before starting launchd.

| Metric | sch3, enabled | sch4, disabled |
| --- | ---: | ---: |
| Distinct iconservicesagent PIDs observed / distinct PIDs exiting on SIGSEGV | 6 / 5 | 0 / 0 |
| Longest observed synchronous wait attributed to iconservicesagent | 4,644.428 ms | None observed |
| Longest directed receive from backboardd / median | 358.180 / 0.363 ms | 18.580 / 0.364 ms |
| HID reports | 2,345 | 2,118 |
| Flip intervals with at least one HID report | 1,214 | 1,242 |
| Over 50 ms among those intervals | 27 (2.22%) | 36 (2.90%) |
| Median / P99 for those intervals | 15.947 / 90.789 ms | 9.972 / 93.446 ms |
| Median / P95 / P99 under continuous HID coverage | 15.358 / 28.067 / 43.232 ms | 9.631 / 28.006 / 46.384 ms |
| Maximum under continuous coverage | 131.501 ms | 75.483 ms |
| At least 100 ms under continuous coverage | 1 / 1,138 | 0 / 1,163 |
| Full-screen-width GPU pass P95 | 4.819 ms | 4.461 ms |
| User instructions per cycle during busy long flips (eligible pairs) | 0.714 (78) | 0.732 (20) |
| HID report interval, median / P99 | 8.331 / 8.531 ms | 8.333 / 8.535 ms |

Sch3's longest flip with HID activity was 4.583 s, including 4.444 s of the synchronous wait linked to iconservicesagent. The longest such sch4 interval was 2.511 s. Its first HID report arrived 2.480 s into the interval and SpringBoard spent 2.486 s in undirected Mach receives. The largest sch4 synchronous receive with a resolved user destination was the 18.580 ms backboardd call.

The 216,848 sch4 latency records were retained with zero reported drops or overwrites. The icon service wait and its crash loop did not recur in the captured workload. The tester's photograph shows that every app position on the visible Home Screen page and Dock lacked its normal icon artwork. The tile backgrounds, app names and Dock remained visible, and the tester could still open Settings. Each condition was measured once with human input, and disabling the service removed visible rendering work. The lower median coexisted with a higher P99 and a higher share above 50 ms.

## Idle desktop panic

The final sch1 panic recorded `turnstile.c:1470` and `thread->inheritor == TURNSTILE_INHERITOR_NULL`. With native slide 0x3920000, the interrupted stack includes `gpuIrq`, `native_adreno_preempt_irq + 0x54`, `lck_mtx_unlock_contended` and the waitq wakeup path. The saved return address immediately follows the call to `lck_mtx_unlock`. A GPU hard interrupt released a contended mutex while the interrupted thread already had a pending turnstile inheritor.

Sch2 schedules CP preemption completion on a serialized thread call. Disassembly of the linked image confirms that the hard handler queues the call and only the worker calls the original preemption routine. The tester manually rebooted a responsive desktop at log time 360 seconds. The exact-stack sampler was disabled for this comparison. The run does not establish long-duration stability.

## Outstanding observations

Trace the null CoreImage program state during iconservicesagent restart. Separately, identify digitizer child events at dispatch and receipt in backboardd and SpringBoard. The bottom-edge classification and the keyboard decision both occur above the recorded contact stream. Pair the actual CoreAnimation display-link callbacks with SpringBoard commits and backboardd submissions to distinguish late wakeups from late production. The two captures do not establish one cause for every frame gap.

## Reproduction

Use `scripts/read-sheng-log.py` to recover the diagnostic export, `scripts/decode-sheng-touchpath.py` with `--jsonl` to inspect the contact sequence and `python3 scripts/sheng-latency-report.py RUN/diag --frames-only` to reproduce the cadence and flip metrics. Omit `--frames-only` to include the GPU pass metrics. Use the cache slide recorded for each individual boot when symbolizing.

For sch3 use `python3 scripts/sheng-springboard-profile.py RUN/diag/files/kernel.latency.bin` for per-call timing, PMU pairs and sampler cost.
