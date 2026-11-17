# macOS unattended desktop inspection

Xiaomi Pad 6S Pro (sheng, 8 GiB), macOS 27.3 build 26C5035r, Android boot header v0. The following preparation uses source `e19928db3e6f49247f21b4593906edfc70750d65` ([PR #122](https://github.com/UniXNU/UniXNU-sheng/pull/122)).

## Prepare the image

Put the selected EDK II BaseTools directory on `PATH`. Supply the existing local account password on standard input.

```sh
export PATH=/path/to/BaseTools/Source/C/bin:$PATH
python3 scripts/prepare-sheng-gpu-lab.py INPUT.img OUTPUT.img \
  --core /path/to/UniXNU --plugin /path/to/plugin-build --tools /path/to/tools \
  --manual-session --preserve-data --autoreset 180 \
  --auto-login-user unixnu --password-stdin --capture-graphics \
  --ssh-public-key /path/to/authorized-key.pub < /path/to/private/account-password
```

The image includes the selected GPU addon, automatic login, graphics logs and SSH key. Its file mode is `0600`. The login job selects the account, keeps the screen and system awake and sets both sleep timers to zero. Shader Cache provides manual Start and Stop controls.

For a persistent desktop, use `autoreset=0` and `uefi-watchdog=1`. Firmware hands over an armed 30-second Gunyah watchdog; the kernel takes ownership and feeds it indefinitely when the return deadline is zero. Diagnostic recording uses the separate `log` and `diag` settings.

## Run and collect

```sh
SERIAL=b714917c READER=/path/to/sheng-linux-log.img \
CORE=/path/to/UniXNU AUTORESET_SECONDS=180 OUT=/path/to/capture \
CAMERA_FRAME=/path/to/observations/latest.jpg CAMERA_KEEP_SECONDS="60 90 120 150 175" \
scripts/run-sheng-unattended.sh OUTPUT.img 420
```

The runner selects a fresh boot tag, resets the active A/B slot's retry budget, RAM-boots the image, retrieves its tagged logs through Linux and checks that fastboot responds. The 180-second return setting selects diagnostic export at 150 seconds. `EXTRA_KERNEL_ARGS` supplies targeted diagnostics.

## Camera

```sh
scripts/capture-sheng-camera.sh /path/to/observations/latest.jpg 5 600
```

The host needs camera permission and FFmpeg with AVFoundation support. `CAMERA_DEVICE` selects the camera index, default `0`. The command atomically replaces one 1280 by 720 JPEG every five seconds for ten minutes. The runner retains fresh frames at `CAMERA_KEEP_SECONDS`, named by boot tag and elapsed seconds.

Keep the current boot image, Linux reader, installation seed and raw captures. Generated reports and superseded build outputs can be regenerated from their retained sources.

## Selected Settings icons

```sh
python3 scripts/prepare-sheng-gpu-lab.py INPUT.img OUTPUT.img \
  --core /path/to/UniXNU --plugin /path/to/plugin-build --tools /path/to/tools \
  --icon-path settings --icon-dump-cb 4:1 --preserve-data --autoreset 180 \
  --auto-login-user unixnu --password-stdin --capture-graphics \
  --ssh-public-key /path/to/authorized-key.pub < /path/to/private/account-password
```

This capture requests the Settings sidebar icons through IconServices with cache bypass, exports `icons.png` and records the selected command buffer. Compare backgrounds, glyphs and edges against the [rendering reference](metal-upstream-rendering.md). `graphics-state.log` includes process and SSH state; `graphics-events-previous.log` contains the isolated command-buffer payloads.

Render-server stderr is appended to `/private/var/tmp/NativeAdrenoMetal.cache/runtime-stderr.log`. Each process records its boot UUID and PID. Graphics diagnostics include the current boot's last 64 KiB in `graphics-state.log`. Capture `s3a6e1-222ad7510d` exported the boot marker for WindowServer PID 171, completed the ordered desktop workload and returned to fastboot.

Capture `sa5ae6-42021120d`, boot `50C2AB62-9A9F-40B9-9099-5B9C12872385`, used the one-pass account preparation and reached General Settings. All 56-source atlas pixels and selected attachment statistics matched the main reference. Console user `unixnu` (UID 501), caffeinate, enabled SSH and Shader Cache `running=false` were recorded. The image returned after 206.224 seconds; Linux retrieval took 18.856 seconds and fastboot responded. Complete macOS CI: [43992174081](https://github.com/UniXNU/UniXNU-sheng/actions/runs/43992174081).

## Texture tiling cost capture

Device source `64e00e4d90e6455257ed477de703dfd95d25c68b` ([PR #123](https://github.com/UniXNU/UniXNU-sheng/pull/123)) adds `--tiling-cost-check` to the selected-icon preparation. The icon probe requests the runtime check with a three-second wait and records its verdict and medians in `icon-probe.log`. The runtime policy log includes the tiling state.

Capture `s3e223-6182b8d69` returned `LinearCheap` on A740: linear 1.339 ms, optimal 1.048 ms, download 0.889 ms and upload 0.342 ms, medians of eight rounds. Its atlas pixels matched the reference and the device returned to fastboot.

## Fixed desktop comparison

Use `--graphics-comparison` in the preparation command. Source `fd964ee46eb855543482e2f7261bf49df3d22178` ([PR #124](https://github.com/UniXNU/UniXNU-sheng/pull/124)) selects one round for return timers below 300 seconds: General, Network, Displays and General, 15 seconds per pane, followed by 15 seconds of settling. The initial desktop reset waits 15 seconds. The animation uses an accessory application with a background window; Settings stays in front. Longer captures use two rounds, 20 seconds per pane and 30 seconds of settling. `agent.log` records the selected timing, pane requests and completion.

Reference capture `sc3cc5-70185657`, boot `1A0BEED0-706D-4613-92AD-D57F340E0F70`, completed the four pane requests and settling period with the 20-layer animation. It returned after 197.638 seconds; Linux retrieval took 25.249 seconds and fastboot responded. Both ordered image packages passed local arm64e compilation, signing and packaging.

```sh
python3 scripts/audit-gpu-desktop-comparison.py \
  /path/to/reference-capture /path/to/candidate-capture --output comparison.json
```

Each capture directory contains `diag/files`. The report uses current-boot pane requests and the raw frame clock, omits the first 30 seconds and matches GPU passes by size, format, draws, pixel count and fragment hash. It reports command callback latency, kernel submission calls, encoding CPU time and GPU pass medians and p95 values. A WindowServer restart or an incomplete sequence rejects the comparison.

Candidate `sf2a4f-76733c08` restarted WindowServer from PID 173 to 1022; the command returned status 2. Its [runtime failure](metal-upstream-rendering.md#desktop-stability) remains under qualification.
