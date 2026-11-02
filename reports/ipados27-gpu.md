# iPadOS 27 GPU integration

Target: sheng, SM8550/A740, iPadOS 27.0.1 (24A446), arm64e.

## Implemented behavior

- Poster uses single-sample textures, pipelines and render passes, with resolve output preserved.
- Same-user shader caches cover the frontend, runtime and Turnip IR3. Sandbox restrictions select private caches.
- Metal resource interfaces include heap aliasing, texture/view swizzles, mip/slice copies, function constants, completion callbacks, allocation statistics, queue capacity and complete viewport/scissor arrays. Zero-length compressed no-copy textures infer their layout size.
- Linked functions retain separate source and exported names. Dynamically indexed tables above 4 KiB use shared constant storage.
- Priority follows current live queue requests and GPU task role. Normal/low work uses ring 1; requested high priorities use ring 0. Queue FIFO survives priority changes.
- Preemption restores each ring's executed page table, saves GMEM, runs the SRAM postamble and keeps the GPU awake through WPTR restoration. Board MMIO retries dropped context-switch writes.

## Qualification

| Check | Result |
| --- | --- |
| Scheduling host test with address/undefined-behavior sanitizers | Pass: packet encoding, GMEM, keepalive, pending/timeout states, per-ring context, FIFO, fence-history rollover and CPU-table fallback |
| Core memory, VM, user-client, timer and lifecycle host checks | Pass with `check-native-adreno-memory.py --core-only`; task GPU roles select foreground/background requests |
| Metal interface pixels, normal and single-sample modes | Pass: heap reclaim, swizzles, resolves, copies, callbacks and queue capacity and shader-selected second viewport/scissor |
| Linked-function compiler tests | Pass: 13 cases, including source/exported-name identity and CPU/L3 execution |
| Large constant tables | Pass: boundary and pointer-expression cases; 16 KiB release case uses about 23 MB RSS and 0.09 s |
| Turnip, Metal bridge, kernel GPU and board archives | Pass: arm64e iPadOS 27 |
| Device frame pacing and repeated preemption | Pending |

The runtime priority is aggregated per process because its logical Metal queues share one Vulkan queue and one kernel FIFO. Queue-local getters retain each request. The ring switch watchdog is 1 s; failed switches enter recovery and retain diagnostics.

The broader legacy GMU test fixture uses an obsolete `NativeAdrenoGmuDefaultSizes` signature. Its API migration remains outside core qualification. Vendor formatting changes are confined to already-formatted files; Clippy passes with the existing `creation.rs` sort-style lint excluded.
