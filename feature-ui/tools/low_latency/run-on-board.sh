#!/bin/sh
set -eu

binary=${1:-/tmp/v4l2_wayland_explicit_sync}
device=/dev/video80
parameter=/sys/module/rockchip_hdmirx/parameters/low_latency
runtime_dir=${XDG_RUNTIME_DIR:-/run}
enabled=0

cleanup()
{
    if [ "$enabled" -eq 1 ]; then
        printf '%s\n' N > "$parameter"
    fi
}

trap cleanup EXIT
trap 'exit 130' HUP INT TERM

if [ ! -x "$binary" ]; then
    printf 'Not executable: %s\n' "$binary" >&2
    exit 1
fi
if [ "$(cat "$parameter")" != N ]; then
    printf 'Refusing to start: low_latency is not N\n' >&2
    exit 1
fi
if fuser "$device" >/dev/null 2>&1; then
    printf 'Refusing to start: %s is busy\n' "$device" >&2
    exit 1
fi

XDG_RUNTIME_DIR="$runtime_dir" "$binary" -d NV16 --probe
enabled=1
printf '%s\n' Y > "$parameter"
if [ "$(cat "$parameter")" != Y ]; then
    printf 'Failed to enable HDMI RX low latency\n' >&2
    exit 1
fi

XDG_RUNTIME_DIR="$runtime_dir" "$binary" \
    -v "$device" -f NV16 -d NV16 --fullscreen
