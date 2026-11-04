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

The flip event is event 4 in `kernel.rings.txt`. A flip interval containing any touch can include idle time before the first touch or after lift. The following intervals provide continuous input coverage.

| Run and start (uptime s) | Flip interval | Observed input | Observed rendering |
| --- | ---: | --- | --- |
| sch2 1454.762454 | 100.273 ms | Five IDs throughout 12 reports, 11 moving frames. One moved 25.1 panel px. | SpringBoard main thread used approximately 14% of the sampled span. The only GPU submission in the interval came from backboardd at +99.425 ms and retired at +99.944 ms. |
| sch2 1411.645516 | 143.153 ms | 17 HID timestamps with a maximum gap of 8.39 ms. The underlying coordinates had already aged out. | SpringBoard main thread used approximately 84%. Median performance/prime cluster readbacks were 2,707/2,956 MHz. |
| sch1 273.109302 | 242.143 ms | 29 HID timestamps with a maximum gap of 8.44 ms. | SpringBoard main thread used approximately 5%. Backboardd submitted the next buffer at +238.523 ms. |

SpringBoard CPU shares use monotonic main-thread recount samples spanning at least 65% of the interval. In sch1, 8,536 SpringBoard main-thread samples and 9,842 backboardd samples captured exact user state. The sch1 cache slide read from the corresponding target records was 0xd6c000. Sampled busy intervals showed dispersed execution in Objective-C dispatch, retain operations and framework code. The frequent instruction addresses did not isolate a single dominant function.

The sch2 display summary recorded 120 Hz, one timer startup and one reported refresh configuration. It recorded zero timer arms and sends, with the vsync ring allocated. This measures the port's IOMFB client notification path. On 24A446 the original IOMFB type 5 sender and the driver read the same client-list offset, 0x1ab8. QuartzCore can also schedule through CADisplayTimer. Neither per-client callback was traced. The observed gaps rule out extended silence in the touch driver for the continuously covered examples. Neither the vsync counters nor HID returns establish delivery of a display callback or touch event to SpringBoard.

## Idle desktop panic

The final sch1 panic recorded `turnstile.c:1470` and `thread->inheritor == TURNSTILE_INHERITOR_NULL`. With native slide 0x3920000, the interrupted stack includes `gpuIrq`, `native_adreno_preempt_irq + 0x54`, `lck_mtx_unlock_contended` and the waitq wakeup path. The saved return address immediately follows the call to `lck_mtx_unlock`. A GPU hard interrupt released a contended mutex while the interrupted thread already had a pending turnstile inheritor.

Sch2 schedules CP preemption completion on a serialized thread call. Disassembly of the linked image confirms that the hard handler queues the call and only the worker calls the original preemption routine. The tester manually rebooted a responsive desktop at log time 360 seconds. The exact-stack sampler was disabled for this comparison. The run does not establish long-duration stability.

## Outstanding observations

Identify digitizer child events at dispatch and receipt in backboardd and SpringBoard. The bottom-edge classification and the keyboard decision both occur above the recorded contact stream. Pair the actual CoreAnimation display-link callbacks with SpringBoard commits and backboardd submissions to distinguish late wakeups from late production. The two captures do not establish one cause for every frame gap.

## Reproduction

Use `scripts/read-sheng-log.py` to recover the diagnostic export, `scripts/decode-sheng-touchpath.py` with `--jsonl` to inspect the contact sequence and `scripts/sheng-latency-report.py` to summarize the exported flip intervals. Use the cache slide recorded for each individual boot when symbolizing.
