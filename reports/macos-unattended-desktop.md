# macOS unattended desktop inspection

Xiaomi Pad 6S Pro (sheng, 8 GiB), macOS 27.3 build 26C5035r.

Test and Linux reader images use Android boot header v0.

The tools in [PR #114](https://github.com/UniXNU/UniXNU-sheng/pull/114), commit `b795973`, select a short return deadline and capture the tablet through the host camera.

```sh
SERIAL=b714917c READER=/path/to/sheng-linux-log.img \
CORE=/path/to/UniXNU AUTORESET_SECONDS=180 \
ADDON=/path/to/runtime-addon.bin OUT=/path/to/capture \
scripts/run-sheng-unattended.sh /path/to/desktop.img 420
```

`AUTORESET_SECONDS=180` selects a 180-second kernel return timer and a 150-second diagnostic export. `CORE` selects the kernel checkout used for repacking. `ADDON` selects the runtime addon. The runner resets the selected A/B slot's retry budget, RAM-boots the test image, retrieves its fresh tagged logs through Linux and checks that fastboot responds.

Captures `s5ef93-11379a842` and `s89668-cbd6f44e` returned to fastboot after 204.477 and 204.491 seconds. Their Linux readers completed in 20.183 and 20.186 seconds, exited successfully and confirmed responsive fastboot. Both runs preserved the installed Data volume.

```sh
scripts/capture-sheng-camera.sh /path/to/observations/latest.jpg 15 1800
```

The host requires camera permission and FFmpeg with AVFoundation support. `CAMERA_DEVICE` selects the camera index, default `0`. The command captures one 1280×720 JPEG every 15 seconds for 1,800 seconds and atomically replaces the same file. The observed session retained one image of approximately 70 KiB. Copy individual frames to separate files when a comparison needs them.

Keep the current boot image, Linux reader, installation seed and raw captures. Generated trace reports and obsolete build outputs can be regenerated from their retained sources.

## Automatic login and screen power

Prepare an addon for the existing local account with the core repository's helper:

```sh
python3 scripts/make-macos-autologin-addon.py INPUT.bin OUTPUT.bin \
  --user unixnu --password-stdin < /path/to/private/account-password
```

The addon installs the selected automatic login credentials, sets `autoLoginUserScreenLocked=false`, keeps the screen and system awake and sets both sleep timers to zero. Capture `s0cbc8-674b3970`, boot session `7FE90579-835F-4ABB-A438-1D0F3AB3AAC1`, records console user `unixnu` (UID 501), Finder, Dock and caffeinate. The camera showed the desktop without input. The timed boot returned through Linux to responsive fastboot after 204.392 seconds; the reader completed in 18.479 seconds.
