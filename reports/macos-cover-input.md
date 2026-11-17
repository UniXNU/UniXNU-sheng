# macOS cover input verification

Xiaomi Pad 6S Pro, sheng, 8 GiB, macOS 27.3 build 26C5035r, Android boot header v0, preserved Data.

| Area | Result |
| --- | --- |
| Protocol checks | WN8030 and macOS keyboard HID checks passed, including tracking reset after a 60 ms report gap. Keyboard trace, clamshell state, touch gating and launch policy checks passed. |
| Kernel build | The configured exported toolchain built the DEVELOPMENT kernel and prelinked it with the selected macOS components. Immutable cover log formats and the HID descriptor use the macOS read-only data segment. |
| Desktop | Automatic login reached the desktop and General Settings. Photos at 120, 150 and 175 seconds show complete widget and icon backgrounds. |
| Cover service | The service query returned ProductID 163 and an initial 29-word timing snapshot. Its decoder reported controller stage 7, three frames and zero I2C failures. SSH was enabled. |
| Return | The test image returned after 206.233 seconds. Linux retrieval took 19.468 seconds and fastboot responded. |
| Pending validation | Physical lid transitions and trackpad motion require an on-device capture. |

Capture `s2fc70-50e43ee32`, boot `CCD0C1C2-CCBF-4B52-A8B4-19C7B8DCEAC0`, used kernel source `ac40d6811c15f775371db7d0aa1fccb3f099b8e6` and device source `4a3945586f915c4891c7465dc727bf9538737da3`. The GPU runtime was `b7cbb00af341d0784552f6f949815433`, component `03f9086f42d39fd1d3d02c11267305e1ebb4d9ba`.

The cover publishes `ShengTouchpadTiming` when its HID device starts and at most once per second of touchpad report time. The layout contains touchpad and pointer counts, gap buckets and controller counters. The macOS HID checker passed 5,119 checks for source `ac40d6811c15f775371db7d0aa1fccb3f099b8e6`; its exported-toolchain kernel prelinked successfully.

## Kernel preparation

```sh
python3 scripts/prepare-sheng-gpu-lab.py INPUT.img OUTPUT.img \
  --core /path/to/UniXNU --plugin /path/to/plugin-build --tools /path/to/tools \
  --kernel /path/to/kernel.macho --root-policies /path/to/root-policies.bin \
  --kernel-tree /path/to/resolved-tree --uefi /path/to/uefi-sheng \
  --graphics-view --kernel-arg enable-jobs=com.openssh.sshd \
  --preserve-data --autoreset 180
```

Kernel replacement carries the matching root policies, entry probes and POST seal in the same payload build. The lab captures cover identity and timing properties beside remote-login state. See [the unattended runner](metal-upstream-rendering.md#capture) for automatic login, camera retention and Linux retrieval.

## Lab controls

`enable-jobs=LABEL` enables a selected launch job under the boot policy. `prepare-sheng-gpu-lab.py --kernel` replaces the kernel, root policy bundle and entry probes in one payload preparation. The daemon records bounded cover identity and timing properties with remote-login state. The four control files at `a7489b1662e6ba1753415448c928cb7d7f74ca73` match the corresponding source used by the guarded capture above.

The shared lab controls passed the [complete macOS CI build](https://github.com/UniXNU/UniXNU-sheng/actions/runs/43978179270) with device `4a3945586f915c4891c7465dc727bf9538737da3` and core `e23fdd384f34859653c970d3e9f4e79331f387bd`. The four control files are selected in [PR #120](https://github.com/UniXNU/UniXNU-sheng/pull/120).
