# Metal upstream rendering comparison

Xiaomi Pad 6S Pro, sheng, 8 GiB, macOS 27.3 build 26C5035r, Android boot header v0. The installed Data volume is preserved.

The comparison uses the General pane in System Settings and a 1024 by 896 atlas of 56 icon sources. Each pair contains the high-resolution workspace icon and the low-resolution IconServices rendering. The main reference renders the high-resolution glyphs; some low-resolution variants contain the colored background.

| Source | Upstream revision | Result |
| --- | --- | --- |
| Main reference, component `26d4902c588d0d0c23993cc4f32ebe9062d5c377` | `555b13d57e09bff70996a88cff64f1275256d5cf` | Complete icon backgrounds. Some low-resolution variants contain the background alone. |
| Last matching checkpoint, component `898c786c0d7087ab62eb1886b96240597caadc2d` | `188606728a57db94f94395ba03144fccbe05a91d` | All atlas pixels and selected attachment statistics match the main reference. |
| Shared uniform ranges, component `5e881fea2086ccd959c85b846ada42e8c59f3b6a` | `1b5a527f5e00abe64036db54c445d2a0e91325c1` | Green widget artifacts, cropped Settings icons and changed atlas RGB pixels. CB 4 pass 2 has RGB minima near -0.95. |
| Corrected uniform array capacity, component `79a2d579af8e1e559391ca03874ca7cbadc360bf` | `1b5a527f5e00abe64036db54c445d2a0e91325c1` | All 56-source atlas pixels and CB 4 passes 2, 4 and 5 statistics match the main reference, with RGB minimum zero. Retained widget and Settings photos show complete backgrounds. |

## Uniform array capacity

RUNTIME 2339 (`734e1cac02c26be4040dca3ea280a6e2f17908a2`) shares uniform descriptor ranges and places a binding's remaining offset in the shader. The shader's fixed array must span that descriptor range. Sizing it to the original window allows an offset inside the descriptor to exceed the declared array.

A740 reports a 64-byte uniform offset alignment and a 65536-byte maximum uniform range. A 16-byte window declared an 80-byte array, while a shared range can give it a 256-byte offset. The corrected compiler sizes eligible uniform arrays to the aligned maximum range and uses a separate cache semantics version. Shared ranges and the consumer's optimizations remain enabled.

The corrected runtime is `24128cb24c4a7146e58247a367ec0e06`. Capture `s393c4-48393a6e3`, boot `428DE739-2D3F-49F1-88C3-5FBF38A32A09`, retained photos at 60, 90, 120, 150 and 175 seconds. The image returned after 206.264 seconds; Linux log retrieval took 19.015 seconds and fastboot responded. The original reference atlas is `sf0ff4-22d059986`, and its CB 4 trace is `s57b45-14bbf54d9`. The failing comparison is `seb6b9-43f6ffa80`.

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
