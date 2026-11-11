#!/bin/sh
# Host checks of the QcomIris parts that run without hardware: the pinned buffer calculator through the shim, and every other
# host test under tests/. Usage: tools/check.sh
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
python3 "$root/tools/fetch-sources.py"
out="$root/out/host"
mkdir -p "$out"
iris="$root/deps/linux/drivers/media/platform/qcom/iris"
# The SMMU table kit of the UniXNU repository checked out next to this board repository (kit: in a test's sources).
kit=${ARM_SMMU_4K_DIR:-$root/../../../UniXNU/kits/ArmSmmu4K}
[ -f "$kit/ArmSmmu4KLive.h" ] || { echo "ArmSmmu4K kit missing: $kit (set ARM_SMMU_4K_DIR)" >&2; exit 1; }
flags="-std=gnu11 -O1 -g -Wall -Werror -fsanitize=address,undefined -I$root/shim -I$root/include -I$iris -I$kit"
clang $flags "$iris/iris_vpu_buffer.c" "$root/src/IrisBuffers.c" "$root/tests/buffers_host.c" -o "$out/buffers"
"$out/buffers"
for test in "$root"/tests/*_host.c; do
    name=$(basename "$test" .c)
    [ "$name" = buffers_host ] && continue
    extra=$(sed -n 's#^/\* sources: \(.*\) \*/$#\1#p' "$test")
    sources=""
    for s in $extra; do
        case $s in kit:*) sources="$sources $kit/${s#kit:}" ;; *) sources="$sources $root/$s" ;; esac
    done
    clang $flags -I"$root/src" $sources "$test" -o "$out/$name"
    "$out/$name"
done
# The decoder device (kernel/IrisMedia.cpp) against stand-ins for the kernel, the board and the session.
clang++ -std=gnu++20 -O1 -g -Wall -Werror -Wno-unused-function -fsanitize=address,undefined -I"$root/tests/kernel-stub" -I"$root/include" \
    -I"$root/kernel" -I"$root/shim" -I"$iris" "$root/kernel/IrisMedia.cpp" "$root/tests/media_host.cpp" -o "$out/media_host"
"$out/media_host"
