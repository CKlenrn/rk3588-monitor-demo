#!/bin/sh

set -eu

original_ui=/etc/init.d/S50systemui
app=/opt/monitor_demo/current/monitor_demo
helper=/opt/monitor_demo/current/v4l2_wayland_explicit_sync
usb_prepare=/usr/libexec/monitor-demo-prepare-usb
mode="${1:-explicit}"

if [ "$#" -gt 1 ]; then
    echo "usage: $0 [baseline|explicit]" >&2
    exit 64
fi
case "$mode" in
    baseline) export MONITOR_DEMO_LOW_LATENCY=0 ;;
    explicit) unset MONITOR_DEMO_LOW_LATENCY ;;
    *)
        echo "usage: $0 [baseline|explicit]" >&2
        exit 64
        ;;
esac

if [ ! -x "$original_ui" ] || [ ! -x "$app" ] || [ ! -x "$helper" ] \
        || [ ! -x "$usb_prepare" ]; then
    echo "original UI script, monitor_demo release, low-latency helper, or USB preparation helper is missing" >&2
    exit 66
fi
"$usb_prepare"

restore_ui()
{
    trap - EXIT INT TERM
    "$original_ui" start || true
}
trap restore_ui EXIT INT TERM

"$original_ui" stop || true
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run}"
export QT_QPA_PLATFORM=wayland
release_dir="$(dirname "$(readlink -f "$app")")"
export LD_LIBRARY_PATH="$release_dir:$release_dir/CrAdapter${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$release_dir"
"$app"
