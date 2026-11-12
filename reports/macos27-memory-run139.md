# macOS 27 memory and encoding check

Device: sheng, SM8550. OS: macOS 27.3, build 26C5035r. Capture: run139,
boot session E61E0991, 2026-11-12. Source bases: UniXNU
`2bbb39dcee0cada3b04e41f0256d678e80667c5b`, UniXNU-sheng
`b9223ad4020a669b1e62b11fb46c8fbcbafd9b1d`. The capture also used the
uncommitted encoding CPU-time and BO cache diagnostics published with this
report. It is not a clean-revision qualification of the new hang diagnostics.

## Encoding experiment

The payload selected `metal-flow=6`, disabling the runtime lowering worker
while retaining commit submission and pipeline priming. Baseline run136 used
the default flow. Both used HEVC wallpaper decoding and the UI stress harness.
The run139 trace was copied before starting memory pressure at uptime 186 s.

| Measurement | run136 | run139 |
| --- | ---: | ---: |
| Retained render buffers | 1128 | 638 |
| Trace span (s) | 228 | 170 |
| Buffers with at least 30 passes | 220 | 93 |
| Heavy-buffer commit to schedule, median (ms) | 17.0 | 17.4 |
| Heavy-buffer commit to schedule, p90 (ms) | 31.5 | 29.1 |
| Continuous frame rate, mean | 59 | 61 |

The different trace windows and pass counts limit the comparison. No clear
reduction in the target median was demonstrated. The lowering worker remains
at its existing default; this experiment is closed without a policy change.

## Compressor and swap

The harness allocated 5120 MiB from `/dev/urandom` in 64 MiB strings, held
them for 20 s, read one byte per 4 KiB and released them. The child ran through
`taskpolicy -a -d default -g default -t 0 -l 0` and was removed from background
scheduling with `taskpolicy -B`. Earlier run138 samples had shown low-priority
I/O and fault throttling; those timings do not measure ordinary application
swap throughput.

Selected rows from the last boot's daemon log:

```text
before, T+187s:
vm.swapusage: total = 0.00M  used = 0.00M  free = 0.00M
Pages stored in compressor: 287653
Pages occupied by compressor: 101994
Swapins: 0
Swapouts: 0
kern.memorystatus_vm_pressure_level: 1

during, T+303s:
vm.swapusage: total = 4096.00M  used = 2979.00M  free = 1117.00M
Swapins: 87345
Swapouts: 851406
kern.memorystatus_vm_pressure_level: 2

hog: holds 5120 MiB, step 883 ms, at 96.3 s
hog: holding 20 s
hog: read back 5120 MiB, step 1001 ms
hog: done, sum 167156010, 197.8 s
hog exit status 0

15 s after, T+412s:
vm.swapusage: total = 2048.00M  used = 316.00M  free = 1732.00M
Swapins: 1320353
Swapouts: 1538041
kern.memorystatus_vm_pressure_level: 1
```

The ordinary workload's compressor ratio before pressure was 2.82:1. Swap
grew to four 1 GiB files and shrank again. Read-back of the full 5 GiB took
about 81 s (197.8 total minus 96.3 allocation, the final 0.1 s allocation
sleep and 20 s hold), about 63 MiB/s. This includes page faults, decompression,
memory copying and concurrent reclaim, not just UFS reads. Pressure level 1
is normal and 2 is warning. The hog exited successfully and pressure returned
to normal. These results establish working compression, swap-out and swap-in;
they do not establish a device throughput ceiling or a byte-for-byte integrity
comparison. No compressor scheduling or swap policy change is justified by
this capture. The run139 spindump contained no samples and supplies no new
stack evidence.

## GPU limit

The final kernel log reports one GPU hang and one successful restart, with
zero failed restarts. It does not identify the earlier zero-high-word fault.
That fault's root cause remains unresolved. The new hang snapshot and address
search are diagnostics, not a fix for it.

## Capture identity

Integration checks passed: the ASan/UBSan ring, preemption and submission
host suite, including fault-search, snapshot retention and replacement;
the Adreno740 arm64e iOS 27 kernel-object build with `-Werror`;
BootLog syntax checking with the recorded kernel compile command; the
macOS 27 Metal plugin build and signing (`--no-replay`); the structured
porting-record check; and the English/Chinese VitePress site build.
BootLog still emits pre-existing warnings outside these changed lines.
The new diagnostic integration was not booted again.

The local capture is under `UniXNU/out/macos-work/run139/`. The shared daemon
log includes older boots; select the last `== daemon start` block before
computing memory statistics. Hashes of the retained files:

| File | SHA-256 |
| --- | --- |
| `test/boot-E61E0991/frames.stress.trace` | `144aa8af8a4eba7305dd1af2e77afaa767ff8aba509f4c54fdb82600004de876` |
| `test/daemon.log` | `59c7e88e223fede2e4746fff889694d0da9874c17f3c9dee9757a79377402a77` |
| `log/untagged-1f474beef/latest.txt` | `d9fd95cbdec309f08efab9b272b566763342d2cbc05fd5ae15972549a10e37e5` |
