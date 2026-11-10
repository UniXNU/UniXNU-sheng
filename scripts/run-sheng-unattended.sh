#!/usr/bin/env bash
# Boots a test image, waits for the tablet to restart into fastboot by itself (the image's autoreset=<seconds> path), then boots the
# Linux log reader and fetches the log. If the tablet does not come back within the limit it says so and waits for a manual fastboot entry.
# usage: [SERIAL=<serial>] [READER=<log-reader.img>] run-sheng-unattended.sh IMAGE [wait_seconds=420]
set -uo pipefail
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$ROOT"
IMG=${1:?image}
LIMIT=${2:-420}
SERIAL=${SERIAL:-$(fastboot devices 2>/dev/null | awk 'NR==1{print $1}')}
[ -n "$SERIAL" ] || { echo "no fastboot device; set SERIAL"; exit 1; }
READER=${READER:-out/sheng/boot/sheng-linux-log.img}
[ -f "$READER" ] || { echo "missing log reader: $READER (set READER)"; exit 1; }
present() { fastboot devices 2>/dev/null | grep -q "^$SERIAL"; }
until present; do sleep 2; done
fastboot -s "$SERIAL" boot "$IMG" || exit 1
sleep 8
while present; do sleep 1; done
echo "test image running; waiting up to ${LIMIT}s for the tablet to return to fastboot"
end=$((SECONDS + LIMIT)); manual=0
until present; do
  if (( SECONDS > end )); then manual=1; echo "AUTORESET DID NOT HAPPEN within ${LIMIT}s: enter fastboot by hand, the reader starts then"; end=$((SECONDS + 100000)); fi
  sleep 2
done
if (( manual )); then echo "tablet is in fastboot (entered by hand)"; else echo "tablet came back to fastboot by itself"; fi
fastboot -s "$SERIAL" boot "$READER" && python3 "$ROOT/scripts/read-sheng-log.py" --wait 240
