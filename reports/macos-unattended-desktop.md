# macOS unattended desktop inspection

Xiaomi Pad 6S Pro (sheng, 8 GiB), macOS 27.3 build 26C5035r.

Test and Linux reader images use Android boot header v0.

The tools in [PR #114](https://github.com/UniXNU/UniXNU-sheng/pull/114), commit `1e8a33f`, select a short return deadline and capture the tablet through the host camera.

```sh
SERIAL=b714917c READER=/path/to/sheng-linux-log.img \
CORE=/path/to/UniXNU AUTORESET_SECONDS=180 \
ADDON=/path/to/runtime-addon.bin OUT=/path/to/capture \
scripts/run-sheng-unattended.sh /path/to/desktop.img 420
```

`EXTRA_KERNEL_ARGS='icon-cb=1:2'` selects targeted diagnostics. The runner owns the return deadline, diagnostic export deadline and boot tag.

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

Cold-start capture `sbd840-26c4b6aa6` used runner revision `ac5daa1710800d80adc8a385b65e55eb85ad5658`. Both images passed its v0 preflight. The camera again showed the desktop without input. The test returned after 204.555 seconds, retrieved its fresh Linux log and confirmed fastboot.


Capture `s1b88c-bcd3d5c5` used runner `4f1dda3` with `EXTRA_KERNEL_ARGS='icon-cb=1:2'`. Both v0 images passed preflight. The exported graphics marker and icon command-buffer log identify boot `97012673-B5F9-45BF-A180-0223EF7CB77C`. The test returned after 206.982 seconds; Linux retrieval completed in 18.331 seconds and confirmed responsive fastboot. The camera showed incomplete Settings icons, so the integrated GPU remains under rendering investigation.


## Selected Settings icons

```sh
python3 scripts/prepare-sheng-gpu-lab.py INPUT.img OUTPUT.img \
  --core /path/to/UniXNU --plugin /path/to/plugin-build --tools /path/to/tools \
  --icon-path settings --icon-dump-cb 3:8 --preserve-data --autoreset 180
```

The selected flow requests the Settings sidebar icons through IconServices with cache bypass, exports `icons.png` and records the requested command-buffer range. `graphics-state.log` includes process state and available runtime stderr. Compare icon backgrounds, glyphs and edges with the reference image.

Capture `sf0ff4-22d059986` collected 56 sources and the icon atlas under boot `8D8D6012-53DA-4C8B-A8FD-FCEF1D52624E`, using main GPU component `26d4902c588d0d0c23993cc4f32ebe9062d5c377`. The image returned after 206.938 seconds; Linux retrieval completed in 19.012 seconds and confirmed fastboot.
