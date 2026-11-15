# macOS 27.3 ABL memory handoff

Device: Xiaomi Pad 6S Pro, sheng, 8 GiB. OS: macOS 27.3, build 26C5035r.

## Firmware

The complete DEBUG CLANGPDB firmware builds with LLVM/LLD 22.1.8 and `SHENG_BOOT_GUARD=1`:

```sh
./unixnu build firmware -D ../UniXNU-sheng --uefi ../uefi-sheng
```

The tested firmware source is `4408c45b43b1d303f99d172cd15717ca8f3210f6`. It has the same complete source tree as [uefi-sheng PR #6](https://github.com/UniXNU/uefi-sheng/pull/6), `760412a31aed607e3016aa63ce58737c8eb0eda4`. The firmware arms recovery before preparing the ABL memory map.

## Device observations

Matching-firmware captures report eight CPUs and `hw.memsize` from 7,388,971,008 to 7,389,036,544 bytes. Capture `s2d373-68f68e191`, boot UUID `B7CEEB79-EB24-4DAF-AC5F-15DED7926D4E`, records 243,120 outside-memory pages adopted and zero refused. Its uptime reaches 302 seconds, with 151 persistent-log rounds and zero write errors.

The same firmware supports persistent UFS access and 16 GPU IOSurface CPU/GPU round trips with zero mismatched bytes. Capture `s9ed3b-71c50dadf` also completes 120 of 120 HEVC 8-bit frames and 12 of 12 4K HEVC 10-bit frames. Sampled decoded NV12 and P010 values match the software reference.

Each timed boot returns through the Linux log reader to responsive fastboot. The runs preserve the installed Data volume.
