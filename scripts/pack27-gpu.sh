#!/bin/bash
# Pack the iPadOS 27 addon (Metal plugin + Turnip, wallpaper, audio plug-in) and its trust bundle from the current builds.
# usage: pack27-gpu.sh [make-gpu-addon args...]   -> out/.../addon27g.bin, trust27g.bundle
set -euo pipefail
cd "$(dirname "$0")/../.."
X=UniXNU/out/sheng/ipados-27.0.1-24A446; T=$X/sheng_ipados27/tree; P=$T/build/adreno-metal-plugin27
rm -f $X/gpu27.bin $X/addon27g.bin $X/trust27g.bundle
ADRENO_PLUGIN_DIR=$P python3 $T/scripts/make-gpu-addon.py --publish --no-daemon "$@" --wallpaper $X/wallpaper27 --wallpaper-floating $X/wallpaper27-floating $X/gpu27.bin | tail -1
python3 UniXNU/scripts/addon-add-dir.py $X/gpu27.bin $X/addon27g.bin audio $X/audio-hal/stage | tail -1
read -r plugin turnip < <(python3 -c "import json;t=json.load(open('$P/trust.json'));print(t['cdhash'],t['turnip_cdhash'])")
audio=$(python3 -c "import json;print(json.load(open('$X/audio-hal/trust.json'))['cdhash'])")
python3 UniXNU/scripts/trust-append-module.py $X/sheng_ipados27/kernel/ipsw-trust.bundle $X/trust27g.bundle $audio $plugin $turnip
