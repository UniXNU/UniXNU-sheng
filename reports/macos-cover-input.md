# macOS cover input verification

Xiaomi Pad 6S Pro, sheng, 8 GiB, macOS 27.3 build 26C5035r, Android boot header v0, preserved Data.

| Area | Result |
| --- | --- |
| Protocol checks | WN8030 and macOS keyboard HID checks passed, including tracking reset after a 60 ms report gap. Keyboard trace, clamshell state, touch gating and launch policy checks passed. |
| Kernel build | The configured exported toolchain built the DEVELOPMENT kernel and prelinked it with the selected macOS components. Immutable cover log formats and the HID descriptor use the macOS read-only data segment. |
| Desktop | Automatic login reached the desktop and General Settings. Photos at 120, 150 and 175 seconds show complete widget and icon backgrounds. |
| Cover service | The service query returned ProductID 163. SSH was enabled. |
| Return | The test image returned after 205.935 seconds. Linux retrieval took 19.143 seconds and fastboot responded. |
| Pending validation | Physical lid transitions and trackpad motion require an on-device capture. |

Capture `sd8c9a-87004aede`, boot `701D1EDE-9D10-4439-87D8-2ED1E984364F`, used the kernel source at `6b293e4` and lab preparation at `906cd78`. The GPU runtime was `2bb0552b6e78724f9ab119c3370d20ed`, component `840e10fc`.

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
