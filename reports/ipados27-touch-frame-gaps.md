# Touch and frame timing on sheng

Tests used iPadOS 27.0.1 build 24A446 on a Xiaomi Pad 6S Pro. Both images were started with `fastboot boot`. The hardware tests did not qualify App Switcher gestures or keyboard flicks.

| Run | Board revision | Boot image SHA-256 | Observation |
| --- | --- | --- | --- |
| sch1, boot 532856a9 | `8fcf9a4e0f3c5a23a820f85fd25189e2c7e83f87` | `445f10c4c6a257f48551b267b9c0d740c17150f3ca5a2e2a51db24cfd1ca2a94` | The tester reported limited improvement and an idle desktop freeze. The panic flush recorded state 5. |
| sch2, boot 5cfa7b18a | `8ec842f38c21ef1d73e12000fae16df7442e0c73` | `15ff435d078f17bdad4bda870b6a85b268b5284217c93a05e227d3b785be27a9` | Both App Switcher gestures still failed and keyboard flicks recurred. The desktop responded before a manual reboot. |

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

## Idle desktop panic

The final sch1 panic recorded `turnstile.c:1470` and `thread->inheritor == TURNSTILE_INHERITOR_NULL`. With native slide 0x3920000, the interrupted stack includes `gpuIrq`, `native_adreno_preempt_irq + 0x54`, `lck_mtx_unlock_contended` and the waitq wakeup path. The saved return address immediately follows the call to `lck_mtx_unlock`. A GPU hard interrupt released a contended mutex while the interrupted thread already had a pending turnstile inheritor.

Sch2 schedules CP preemption completion on a serialized thread call. Disassembly of the linked image confirms that the hard handler queues the call and only the worker calls the original preemption routine. The tester manually rebooted a responsive desktop at log time 360 seconds. The exact-stack sampler was disabled for this comparison. The run does not establish long-duration stability.

## Outstanding observations

Identify digitizer child events at dispatch and receipt in backboardd and SpringBoard. The bottom-edge classification and the keyboard decision both occur above the recorded contact stream. Pair the actual CoreAnimation display-link callbacks with SpringBoard commits and backboardd submissions to distinguish late wakeups from late production. The two captures do not establish one cause for every frame gap.

## Reproduction

Use `scripts/read-sheng-log.py` to recover the diagnostic export, `scripts/decode-sheng-touchpath.py` with `--jsonl` to inspect the contact sequence and `python3 scripts/sheng-latency-report.py RUN/diag --frames-only` to reproduce the cadence and flip metrics. Omit `--frames-only` to include the GPU pass metrics. Use the cache slide recorded for each individual boot when symbolizing.
