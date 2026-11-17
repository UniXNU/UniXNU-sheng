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

Capture `sa5ae6-42021120d`, boot `50C2AB62-9A9F-40B9-9099-5B9C12872385`, used the one-pass account preparation and reached General Settings. All 56-source atlas pixels and selected attachment statistics matched the main reference. Console user `unixnu` (UID 501), caffeinate, enabled SSH and Shader Cache `running=false` were recorded. The image returned after 206.224 seconds; Linux retrieval took 18.856 seconds and fastboot responded. Complete macOS CI: [43992174081](https://github.com/UniXNU/UniXNU-sheng/actions/runs/43992174081).
