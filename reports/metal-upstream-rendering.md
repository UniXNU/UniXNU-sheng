# Metal upstream rendering comparison

Xiaomi Pad 6S Pro, sheng, 8 GiB, macOS 27.3 build 26C5035r, Android boot header v0. The installed Data volume is preserved.

The comparison uses the General pane in System Settings and a 1024 by 896 atlas of 56 icon sources. Each pair contains the high-resolution workspace icon and the low-resolution IconServices rendering. The pixel reference renders the high-resolution glyphs; some low-resolution variants contain the colored background.

| Source | Upstream revision | Result |
| --- | --- | --- |
| Pixel reference, component `26d4902c588d0d0c23993cc4f32ebe9062d5c377` | `555b13d57e09bff70996a88cff64f1275256d5cf` | Complete icon backgrounds. Some low-resolution variants contain the background alone. |
| Last matching checkpoint, component `898c786c0d7087ab62eb1886b96240597caadc2d` | `188606728a57db94f94395ba03144fccbe05a91d` | All atlas pixels and selected attachment statistics match the main reference. |
| Shared uniform ranges, component `5e881fea2086ccd959c85b846ada42e8c59f3b6a` | `1b5a527f5e00abe64036db54c445d2a0e91325c1` | Green widget artifacts, cropped Settings icons and changed atlas RGB pixels. CB 4 pass 2 has RGB minima near -0.95. |
| Corrected uniform array capacity, component `79a2d579af8e1e559391ca03874ca7cbadc360bf` | `1b5a527f5e00abe64036db54c445d2a0e91325c1` | All 56-source atlas pixels and CB 4 passes 2, 4 and 5 statistics match the main reference, with RGB minimum zero. Retained widget and Settings photos show complete backgrounds. |
| Qualified development with the capacity correction, component `840e10fce0a40622b9c28b0f9da010a6402a84d5` | `be905fd8992417edf146792036d666c126baa093` | All atlas pixels and selected attachment statistics match the main reference. Photos through 175 seconds show complete widget and Settings backgrounds. |
| Scheduler and rebind checkpoint, component `a4590c23b319a1ae14332d71c0a4a2fabf729aaa` | `cda5fd8f027f29f1213961de4eb11a7726c61bf6` | All atlas pixels and selected attachment statistics match the main reference. Photos through 175 seconds show complete widget and Settings backgrounds. Full macOS CI passed. |
| Compute reuse checkpoint, component `8b1a75b3dc6999a47ea4275d04eb8e8c7b318c96` | `51280732aa8c2e86240fb5f73669f86798de9893` | Atlas pixels and selected attachment statistics match the reference. Complete backgrounds at 90 through 175 seconds. Full macOS CI and ordered desktop qualification through runtime 2485 passed. |
| Constant views and placement queries, component `3e556b62f9b9b05e250e41a79caf1f4e016b3ec8` | `85987cda9d68ef150340611d620dbbecaa1efa56` | Atlas pixels and selected attachment statistics match the reference. Complete backgrounds at 90 through 175 seconds. Full macOS CI passed. |
| Tiling measurement and region copies, component `2ab9328eb8a220c98c2aef997fa5f4633e7f672a` | `33186454573ae3c8c1af7ef2e0fcdcf217f5eb07` | Atlas pixels and selected attachment statistics match the reference. The explicit A740 tiling check settled as LinearCheap. Full macOS CI and the ordered desktop workload passed. |
| Staged row copies, component `d9535ac96256aacfab721d4d2eb16a6109c3181c` | `e3959de823c26623ba6a3f50bbc077d2508170f9` | Atlas pixels and selected attachment statistics match the reference. The ordered repeat completed with a stable WindowServer process. Full macOS CI passed. |
| Current main, component `f7b4ed1d3824c1033d9355e29889076f83163e3b` | `46e7cc96a284150bdf19b4a6a7890b0a8bc0ef71` | R2507 completed the ordered desktop workload with WindowServer PID 171 unchanged. Widget and Settings backgrounds are complete in retained photos; the user confirmed the displayed result. Local release build and signing passed. |

## Uniform array capacity

RUNTIME 2339 (`734e1cac02c26be4040dca3ea280a6e2f17908a2`) shares uniform descriptor ranges and places a binding's remaining offset in the shader. The shader's fixed array must span that descriptor range. Sizing it to the original window allows an offset inside the descriptor to exceed the declared array.

A740 reports a 64-byte uniform offset alignment and a 65536-byte maximum uniform range. A 16-byte window declared an 80-byte array, while a shared range can give it a 256-byte offset. The corrected compiler sizes eligible uniform arrays to the aligned maximum range and uses a separate cache semantics version. Shared ranges and the consumer's optimizations remain enabled.

The corrected runtime is `24128cb24c4a7146e58247a367ec0e06`. Capture `s393c4-48393a6e3`, boot `428DE739-2D3F-49F1-88C3-5FBF38A32A09`, retained photos at 60, 90, 120, 150 and 175 seconds. The image returned after 206.264 seconds; Linux log retrieval took 19.015 seconds and fastboot responded. The original reference atlas is `sf0ff4-22d059986`, and its CB 4 trace is `s57b45-14bbf54d9`. The failing comparison is `seb6b9-43f6ffa80`.

The qualified be905 capture is `s4fa13-4bd7e1abb`, boot `DBCB42BF-E951-4E1D-91D9-CAFE40F2B363`, runtime `2bb0552b6e78724f9ab119c3370d20ed`. Its image returned after 206.204 seconds; Linux retrieval took 19.213 seconds and fastboot responded. The Core Image half-blur probe completed 4096 pixels with no nonfinite values.

The local arm64e release build and signing passed. The [complete macOS CI build](https://github.com/UniXNU/UniXNU-sheng/actions/runs/43963130713) compiled and packaged core `6d059b16c2f09f2f6911d5a5c1ee0d8ee4c866f5`. The current source is available through [AdrenoMetal #51](https://github.com/UniXNU/AdrenoMetal/pull/51) and [UniXNU #286](https://github.com/UniXNU/UniXNU/pull/286).

## Capture

```sh
python3 scripts/prepare-sheng-gpu-lab.py INPUT.img OUTPUT.img \
  --core /path/to/UniXNU --plugin /path/to/plugin-build --tools /path/to/tools \
  --icon-path settings --icon-dump-cb 4:1 --preserve-data --autoreset 180

SERIAL=b714917c READER=/path/to/sheng-linux-log.img \
CORE=/path/to/UniXNU AUTORESET_SECONDS=180 \
ADDON=/path/to/automatic-login-addon.bin \
CAMERA_FRAME=/path/to/observations/latest.jpg CAMERA_KEEP_SECONDS="60 90 120 150 175" \
OUT=/path/to/capture scripts/run-sheng-unattended.sh OUTPUT.img 420
```

Compare the retained Settings photos, the decoded atlas pixels and the matching command-buffer attachments.

The scheduler and rebind capture is `s079b0-42da33372`, boot `BE80FAD6-5A74-44A0-A165-F01E26E6B089`, runtime `ab279cc610ed93f137b1668e3f751450`. The local release build and signing passed. Return took 205.927 seconds and Linux retrieval took 19.800 seconds, with fastboot responding. The 4096-pixel half-blur probe had no nonfinite values. The candidate is [AdrenoMetal #55](https://github.com/UniXNU/AdrenoMetal/pull/55) and [UniXNU #292](https://github.com/UniXNU/UniXNU/pull/292); its complete CI passed: [43987841609](https://github.com/UniXNU/UniXNU-sheng/actions/runs/43987841609).

The compute-reuse capture is `s23178-3b623c779`, boot `A5FC6DB7-5C84-4D1C-9178-15D5980C1B00`, runtime `8e5753f0087aa93fb01850fdecb7f45a`; return and Linux retrieval took 206.210 and 19.058 seconds. The constant-view capture is `sa5ae6-42021120d`, boot `50C2AB62-9A9F-40B9-9099-5B9C12872385`, runtime `6922265bbba960fbd06100a4f70b17a7`; return and retrieval took 206.224 and 18.856 seconds. Both returned to responsive fastboot. Their local builds and signing passed.

## A740 texture tiling measurement

Capture `s3e223-6182b8d69`, boot `034A90BD-C54F-41E2-9F83-CA1B3253C2FD`, runtime `a70fac9ecbef4a2f7c2d4125fda28c65`, measured eight rounds of two-pass 2048 by 1536 BGRA8Unorm frames.

| GPU operation | Median |
| --- | ---: |
| Linear frame over host pages | 1.339 ms |
| Optimal frame | 1.048 ms |
| Download to host pages | 0.889 ms |
| Upload from host pages | 0.342 ms |

The check reported `LinearCheap` and completed in 905.1 ms of host time. These frame and copy costs support the selected `InPlaceWrites` policy for this host-page workload. The icon atlas and selected attachment statistics matched the reference. Return took 206.099 seconds, Linux retrieval took 18.921 seconds and fastboot responded. Complete CI: [43995908832](https://github.com/UniXNU/UniXNU-sheng/actions/runs/43995908832).

The staged-row capture is `s9db86-480e4e4de`, boot `C5534944-2C10-4FFA-9B16-688790DC9CF6`, runtime `be44c1281af3bbf2e69e2bb44f2c211a`. Return took 206.273 seconds, Linux retrieval took 18.842 seconds and fastboot responded. The local release build and signing passed. Complete CI: [43998089607](https://github.com/UniXNU/UniXNU-sheng/actions/runs/43998089607).

## Desktop stability

The ordered workload uses source `619c1cecd1457ffbb91be8d6808ee0b192654213`, a background animation window and General, Network, Displays and General pane requests. The reference component `a4590c23b319a1ae14332d71c0a4a2fabf729aaa` completed capture `sc3cc5-70185657` and returned to fastboot.

The R2485 comparison uses development `33186454573ae3c8c1af7ef2e0fcdcf217f5eb07`, component `2ab9328eb8a220c98c2aef997fa5f4633e7f672a`. Its ordered capture is `s4e8fe-5ad112253`, boot `DF75D96F-FA04-4CEC-8F98-82DCAC6C8EE6`, runtime `a70fac9ecbef4a2f7c2d4125fda28c65`. All four pane requests and settling completed with a stable WindowServer process and complete widget/Settings backgrounds. Return took 195.981 seconds, Linux retrieval took 25.708 seconds and fastboot responded.

The comparison omits the first 30 seconds after the initial pane request and measures the following 48 seconds. It matches 84 GPU pass signatures by geometry, format, pixel count and fragment hash.

| Measurement | Scheduler reference | Qualified prefix |
| --- | ---: | ---: |
| Command callback latency median | 41.206 ms | 43.073 ms |
| Command callback latency p95 | 47.058 ms | 48.547 ms |
| Kernel submission call median | 6.376 ms | 6.802 ms |
| Encoding batch CPU median | 0.175 ms | 0.171 ms |
| Full-screen pass, three draws, fragment `3731781318`, median | 1.641 ms | 1.638 ms |
| Full-screen pass, one draw, fragment `2070874897`, median | 1.162 ms | 1.151 ms |

Component `d9535ac96256aacfab721d4d2eb16a6109c3181c` failed capture `sf2a4f-76733c08`, boot `E07FE387-AE42-477F-9892-3A94725CF7A2`. WindowServer PID 173 restarted as 1022 and the desktop returned to login before the sequence completed. The fresh report is `WindowServer-2026-11-14-151617.ips`. Module UUID `EA586705-94E8-3418-A84C-183386ADF0DD` matches the locally built module and its symbols.

The panic bounds-check location is `crates/metal2vulkan-runtime/src/replay.rs:123:19`, in `Handles<RenderPipelineState>::get`, reached by `render::replay::render` and `nmb_render_set_pipeline`. Rust panic unwinding then fails pointer authentication in libunwind. The cause is undetermined. Automatic return took 206.119 seconds, Linux retrieval took 19.569 seconds and fastboot responded. Repeat capture `s3de1f-63d0e8c2b`, boot `152C6F90-BAB5-407E-904B-D50FFB57C267`, completed the ordered workload with WindowServer PID 171 unchanged and returned to fastboot.

## Current desktop result

Main selects R2507 through [AdrenoMetal #62](https://github.com/UniXNU/AdrenoMetal/pull/62) and [UniXNU #298](https://github.com/UniXNU/UniXNU/pull/298). Capture `s3a6e1-222ad7510d`, boot `E89B4F09-A0DE-436C-9389-06EAFB2BCBB7`, completed all pane requests and settling with WindowServer PID 171 unchanged. Return took 195.873 seconds, Linux retrieval took 24.889 seconds and fastboot responded.

The 48-second comparison window matches 78 GPU pass signatures against the scheduler reference. Command callback latency was 41.462 ms median and 46.328 ms p95; kernel submission calls were 6.894 ms median and 9.243 ms p95. Encoding batch CPU time was 0.171 ms median. The full-screen three-draw pass with fragment `3731781318` took 1.656 ms median; the one-draw pass with fragment `2070874897` took 1.182 ms.

The runtime policy source audit resolves all 26 policy defaults and 19 Turnip options. `QueueTurnPolicy::Fair` is selected by default. The consumer retains the uniform array correction and manual Shader Cache application. The full image build is [44022249295](https://github.com/UniXNU/UniXNU-sheng/actions/runs/44022249295).
