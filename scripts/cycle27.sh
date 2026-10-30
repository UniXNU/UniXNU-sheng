#!/usr/bin/env bash
# One iPadOS 27 sheng cycle: build kernel, payload, boot image, then the unattended run (boot, autoreset, log read).
# usage: cycle27.sh [--no-kernel] [extra --kernel-arg values ...]
set -euo pipefail
SERIES=$(cd "$(dirname "$0")/../.." && pwd)
U=$SERIES/UniXNU
T=$U/out/sheng/ipados-27.0.1-24A446/sheng_ipados27
K=$T/kernel
LOG=${CYCLE_LOG:-/tmp/cycle27.log}
cd "$U"
export ATH12K_WIFI_DIR=${ATH12K_WIFI_DIR:-$SERIES/wt/NativeWifi}
build_kernel=1
if [ "${1:-}" = --no-kernel ]; then build_kernel=0; shift; fi
args=(--kernel-arg sheng-log=1 --kernel-arg sheng-autoreset=${AUTORESET:-120})
# UFS_DIAG_OFF=1 drops native-ufs-diagnostics=1 (23 bytes) when the command line is tight.
[ -n "${UFS_DIAG_OFF:-}" ] || args+=(--kernel-arg native-ufs-diagnostics=1)
for a in "$@"; do args+=(--kernel-arg "$a"); done
DATA=${DATA:-$U/out/sheng/ipados-27.0.1-24A446/data-seed/data.dbr}
[ "$DATA" = none ] || args+=(--data-ramdisk "$DATA")
if [ $build_kernel = 1 ]; then
  ./unixnu defconfig sheng_ipados27_defconfig -D ../UniXNU-sheng > "$LOG" 2>&1
  for pass in 1 2; do
    ./unixnu build kernel -D ../UniXNU-sheng --kernelcache out/ipsw/24A446/kernelcache.decompressed >> "$LOG" 2>&1 || { echo "kernel build failed: $LOG"; grep -n " error\|referenced from" "$LOG" | head; exit 1; }
    # The emitter moves the native kernel; the display bindings are linked against the move of the previous emission.
    N=$T/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped
    S=$T/tree/build/native-legacy-display/native-slide
    linked=$(nm "$N" | awk '$3=="_native_legacy_display_allocate"{print $1}')
    [ -n "$linked" ] || break
    final=$(nm "$K/kernel.macho" 2>/dev/null | awk '$3=="_native_legacy_display_allocate"{print $1}')
    slide=$(python3 -c "print(hex((0x$final-0x$linked)%(1<<64)))")
    [ "$(cat "$S" 2>/dev/null)" = "$slide" ] && break
    echo "native slide $slide (was $(cat "$S" 2>/dev/null || echo none)), relinking" | tee -a "$LOG"
    echo "$slide" > "$S"
  done
  if [ -f "$T/tree/build/native-legacy-display/native-slide" ]; then
    "${DIS_PYTHON:-python3}" "$SERIES/UniXNU-sheng/scripts/check-legacy27-binds.py" $K/kernel.macho | tee -a "$LOG" || { echo "display bindings do not reach the original code"; exit 1; }
  fi
fi
# The kernel command line is cut near 1000 bytes (an image at exactly 1000 lost its autoreset): refuse anything past 900 before the payload step.
cmdline=$(python3 - "${args[@]}" <<'PY'
import sys
a=sys.argv[1:]
print(" ".join(a[i+1] for i in range(0,len(a)-1) if a[i]=="--kernel-arg"))
PY
)
if [ ${#cmdline} -gt "${CMDLINE_MAX:-700}" ]; then echo "kernel arguments from this script are ${#cmdline} bytes (limit ${CMDLINE_MAX:-700}): shorten them"; exit 1; fi
# ADDON: an NGPUADD2 addon (e.g. the audio plug-in); TRUST: a trust bundle that also carries its CDHashes.
[ -n "${ADDON:-}" ] && args+=(--gpu-addon "$ADDON" --kernel-arg sheng-gpu-addon=1)
./unixnu payload -D ../UniXNU-sheng --uefi ../uefi-sheng --kernel $K/kernel.macho --trust-cache ${TRUST:-$K/ipsw-trust.bundle} \
  "${args[@]}" --unique-chip-id b714917c >> "$LOG" 2>&1 || { echo "payload failed: $LOG"; tail -5 "$LOG"; exit 1; }
want=$(shasum -a 256 $K/kernel.macho | cut -c1-64)
have=$(python3 -c "import json;print(json.load(open('$T/payload/payload.json'))['kernel_sha256'])")
[ "$want" = "$have" ] || { echo "payload kernel mismatch"; exit 1; }
./unixnu bootimg -D ../UniXNU-sheng --uefi ../uefi-sheng --dtb out/sheng/inputs/sheng.dtb >> "$LOG" 2>&1 || { echo "bootimg failed"; exit 1; }
echo "image ready: kernel ${want:0:16}"
# NOBOOT=1: build the image and stop before the hardware run.
[ -n "${NOBOOT:-}" ] && exit 0
cd ../UniXNU-sheng
SERIAL=b714917c READER=$U/out/sheng/inputs/sheng-linux-log.img bash scripts/run-sheng-unattended.sh $T/boot/boot.img ${LIMIT:-420}
