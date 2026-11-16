# Metal upstream rendering comparison

Xiaomi Pad 6S Pro, sheng, 8 GiB, macOS 27.3 build 26C5035r, Android boot header v0. The installed Data volume is preserved.

The comparison uses the General pane in System Settings and a 1024 by 896 atlas of 56 icon sources. Each pair contains the high-resolution workspace icon and the low-resolution IconServices rendering. The main reference renders the high-resolution glyphs; some low-resolution variants contain the colored background.

| Source | Upstream revision | Result |
| --- | --- | --- |
| Main reference, component `26d4902c588d0d0c23993cc4f32ebe9062d5c377` | `555b13d57e09bff70996a88cff64f1275256d5cf` | Complete icon backgrounds. |
| Main consumer optimizations, component `89d7942e06dad93b5a5a52f817aa8c19fd899b37` | `842fd73b71f4c595810651bb61d8a0c48f486845` | All atlas pixels match the reference. CB 4 pass 2 and its downstream attachments match the reference statistics, with RGB minimum zero. |
| Native half contraction, component `e717879a905089bc65d34851fb795bb9fb505fdb` | `f3c9f748ce94ede0bddb3626654e1d8b0bb31797` | All atlas pixels and selected attachment statistics match the reference, with RGB minimum zero. |
| Render-target checkpoint, component `79915e358c0e2c4e750ed851eb9c8f4359dfb339` | `a5a0ecbd30aeeba46f4f5ad80c7112fdaccf112b` | All atlas pixels and selected attachment statistics match the reference, with RGB minimum zero. |
| Half arithmetic checkpoint, component `59b23b38b722be17ef167a53cea8412830489760` | `50cdea6eb876c6319b7dba8bccafb28bb9e97d88` | All atlas pixels and selected attachment statistics match the reference, with RGB minimum zero. |
| Snapshot reuse checkpoint, component `54c18dcffc359d21f4e620c3b38b3908cecc46a5` | `8b9b2875ab2ca32de30db6d5c396e1469e164bee` | All atlas pixels and selected attachment statistics match the reference, with RGB minimum zero. |
| Texture row checkpoint, component `252601c418be284246c648eb9a014ad0268f3a13` | `38cc6ae244af2bb2e8bb4ceede5bf9cc7dbede3b` | All atlas pixels and selected attachment statistics match the reference, with RGB minimum zero. Settings backgrounds remain complete through 175 seconds. |
| Snapshot and binding checkpoint, component `5e881fea2086ccd959c85b846ada42e8c59f3b6a` | `1b5a527f5e00abe64036db54c445d2a0e91325c1` | Green checkerboard artifacts in a widget, cropped Settings icons and changed atlas RGB pixels. CB 4 pass 2 has RGB minima near -0.95; atlas alpha pixels match the reference. |
| Integrated diagnostic runtime, component `f3103ad1e5f4b6b53ba5669db9d63dac0cb9d271` | `39dc65850326a5c69ad8bdf87a6fd58c8d188108` | Cropped backgrounds and colored edges. CB 4 pass 2 has RGB minima near -0.95. |

The verified checkpoint uses runtime `50e85da40a23cd75369942090039129b`. Capture `se24f6-3383f343b`, boot `67CB716E-F9E4-49A3-9021-44FDA26D2C55`, retained the 120, 150 and 175 second Settings photos. The image returned after 206.111 seconds; Linux log retrieval took 18.942 seconds and fastboot responded. The reference atlas is from `sf0ff4-22d059986`; its CB 4 trace is from `s57b45-14bbf54d9`. The integrated diagnostic capture is `se8d01-1cc2adcce`.

The regression is bounded by `38cc6ae2` (RUNTIME 2313) and `1b5a527f` (RUNTIME 2340), a range of 101 commits. Capture `seb6b9-43f6ffa80`, boot `E917244A-979D-4CAC-B828-E9A293DAC83D`, reproduced the selected attachment statistics of `39dc6585`. Its v0 image returned after 207.460 seconds; Linux retrieval took 19.332 seconds and fastboot responded. The cause within this range is unresolved.

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
