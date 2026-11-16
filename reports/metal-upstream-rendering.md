# Metal upstream rendering comparison

Xiaomi Pad 6S Pro, sheng, 8 GiB, macOS 27.3 build 26C5035r, Android boot header v0. The installed Data volume is preserved.

The comparison uses the General pane in System Settings and a 1024 by 896 atlas of 56 icon sources. Each pair contains the high-resolution workspace icon and the low-resolution IconServices rendering. The main reference renders the high-resolution glyphs; some low-resolution variants contain the colored background.

| Source | Upstream revision | Result |
| --- | --- | --- |
| Main reference, component `26d4902c588d0d0c23993cc4f32ebe9062d5c377` | `555b13d57e09bff70996a88cff64f1275256d5cf` | Complete icon backgrounds. |
| Main consumer optimizations, component `89d7942e06dad93b5a5a52f817aa8c19fd899b37` | `842fd73b71f4c595810651bb61d8a0c48f486845` | All atlas pixels match the reference. CB 4 pass 2 and its downstream attachments match the reference statistics, with RGB minimum zero. |
| Native half contraction, component `e717879a905089bc65d34851fb795bb9fb505fdb` | `f3c9f748ce94ede0bddb3626654e1d8b0bb31797` | All atlas pixels and selected attachment statistics match the reference, with RGB minimum zero. |
| Render-target checkpoint, component `79915e358c0e2c4e750ed851eb9c8f4359dfb339` | `a5a0ecbd30aeeba46f4f5ad80c7112fdaccf112b` | All atlas pixels and selected attachment statistics match the reference, with RGB minimum zero. |
| Integrated diagnostic runtime, component `f3103ad1e5f4b6b53ba5669db9d63dac0cb9d271` | `39dc65850326a5c69ad8bdf87a6fd58c8d188108` | Cropped backgrounds and colored edges. CB 4 pass 2 has RGB minima near -0.95. |

The verified checkpoint uses runtime `53a28ca1e4990dd03563a0a8758c2916`. Capture `s5e0b2-52770d967`, boot `53D5AC3D-E293-4637-8175-9CF9C4912D54`, retained the 120, 150 and 175 second Settings photos. The image returned after 205.343 seconds; Linux log retrieval took 18.998 seconds and fastboot responded. The reference atlas is from `sf0ff4-22d059986`; its CB 4 trace is from `s57b45-14bbf54d9`. The integrated diagnostic capture is `se8d01-1cc2adcce`.

## Capture

```sh
python3 scripts/prepare-sheng-gpu-lab.py INPUT.img OUTPUT.img \
  --core /path/to/UniXNU --plugin /path/to/plugin-build --tools /path/to/tools \
  --icon-path settings --icon-dump-cb 4:1 --preserve-data --autoreset 180

SERIAL=b714917c READER=/path/to/sheng-linux-log.img \
CORE=/path/to/UniXNU AUTORESET_SECONDS=180 \
ADDON=/path/to/automatic-login-addon.bin \
CAMERA_FRAME=/path/to/observations/latest.jpg CAMERA_KEEP_SECONDS="120 150 175" \
OUT=/path/to/capture scripts/run-sheng-unattended.sh OUTPUT.img 420
```

Compare the retained Settings photos, the decoded atlas pixels and the matching command-buffer attachments.
