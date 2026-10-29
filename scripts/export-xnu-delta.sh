#!/usr/bin/env bash
# Write the difference between "pinned XNU + the selected series (without the delta patch itself)" and the xnu work tree
# as UniXNU-sheng/patches/xnu-12377/zzzz-sheng-ipados27-bringup.patch, and list it last in that series. Re-runnable.
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
T=$R/UniXNU/out/sheng/ipados-27.0.1-24A446/sheng_ipados27
W=$T/tree/.cache/xnu-work
NAME=zzzz-sheng-ipados27-bringup.patch
P=/tmp/xnu-pristine
rm -rf $P
git clone -q --shared --no-checkout "$(python3 -c "import json;print(json.load(open('$T/xnu-inputs.json'))['source'])")" $P
cd $P
git checkout -q --detach "$(python3 -c "import json;print(json.load(open('$T/xnu-inputs.json'))['commit'])")"
while read -r p; do [ "$(basename "$p")" = $NAME ] && continue; git apply "$p"; done < $T/xnu-patches.list
git add -A; git -c user.email=a@b -c user.name=n commit -q -m series
files=$({ diff -rq --exclude=.git --exclude='*.orig' --exclude=TrustCache --exclude=DriverKit "$W" $P 2>/dev/null || true; } | sed -n 's#^Files .*/xnu-work/\(.*\) and /tmp/xnu-pristine/.* differ$#\1#p')
for f in $files; do cp "$W/$f" "$P/$f"; done
only=$(diff -rq --exclude=.git --exclude='*.orig' --exclude=TrustCache --exclude=DriverKit "$W" $P 2>/dev/null | grep '^Only in' || true)
[ -z "$only" ] || { echo "files only on one side:"; echo "$only"; }
git diff --binary > $R/UniXNU-sheng/patches/xnu-12377/$NAME
grep -qx $NAME $R/UniXNU-sheng/patches/xnu-12377/series || echo $NAME >> $R/UniXNU-sheng/patches/xnu-12377/series
echo "$(echo "$files" | wc -w) files, $(wc -l < $R/UniXNU-sheng/patches/xnu-12377/$NAME) lines"
