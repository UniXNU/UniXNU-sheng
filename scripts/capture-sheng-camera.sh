#!/usr/bin/env bash
# Keep the latest camera frame for an unattended device test.
set -euo pipefail
if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo 'usage: capture-sheng-camera.sh OUTPUT.jpg [interval_seconds=15] [duration_seconds=1800]' >&2
    exit 2
fi
output=$1
interval=${2:-15}
duration=${3:-1800}
if [[ ! $interval =~ ^[1-9][0-9]*$ || ! $duration =~ ^[1-9][0-9]*$ ]]; then
    echo 'interval and duration must be positive integer seconds' >&2
    exit 2
fi
mkdir -p "$(dirname "$output")"
exec ffmpeg -nostdin -hide_banner -loglevel error \
    -f avfoundation -pixel_format uyvy422 -framerate 30 -video_size 1280x720 \
    -i "${CAMERA_DEVICE:-0}:none" -vf "fps=1/$interval" -q:v 2 \
    -update 1 -atomic_writing 1 -t "$duration" -y "$output"
