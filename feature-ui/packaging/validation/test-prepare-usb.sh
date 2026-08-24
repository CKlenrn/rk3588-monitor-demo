#!/bin/sh

set -eu

script_dir="$(CDPATH= cd "$(dirname "$0")" && pwd)"
helper="$script_dir/../board/usr/libexec/monitor-demo-prepare-usb"
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM

mkdir -p "$test_dir/devices/usb6/power" \
         "$test_dir/devices/6-1/power" \
         "$test_dir/devices/6-1:1.0/power"
printf '16\n' >"$test_dir/usbfs_memory_mb"
printf '2\n' >"$test_dir/autosuspend"
printf '1d6b\n' >"$test_dir/devices/usb6/idVendor"
printf '1a86\n' >"$test_dir/devices/6-1/idVendor"
printf 'auto\n' >"$test_dir/devices/usb6/power/control"
printf 'auto\n' >"$test_dir/devices/6-1/power/control"
printf 'auto\n' >"$test_dir/devices/6-1:1.0/power/control"

MONITOR_DEMO_USBFS_MEMORY_FILE="$test_dir/usbfs_memory_mb" \
MONITOR_DEMO_USB_AUTOSUSPEND_FILE="$test_dir/autosuspend" \
MONITOR_DEMO_USB_DEVICES_DIR="$test_dir/devices" \
    "$helper" >/dev/null

[ "$(cat "$test_dir/usbfs_memory_mb")" = 150 ]
[ "$(cat "$test_dir/autosuspend")" = -1 ]
[ "$(cat "$test_dir/devices/usb6/power/control")" = on ]
[ "$(cat "$test_dir/devices/6-1/power/control")" = on ]
[ "$(cat "$test_dir/devices/6-1:1.0/power/control")" = auto ]

printf '256\n' >"$test_dir/usbfs_memory_mb"
MONITOR_DEMO_USBFS_MEMORY_FILE="$test_dir/usbfs_memory_mb" \
MONITOR_DEMO_USB_AUTOSUSPEND_FILE="$test_dir/autosuspend" \
MONITOR_DEMO_USB_DEVICES_DIR="$test_dir/devices" \
    "$helper" >/dev/null
[ "$(cat "$test_dir/usbfs_memory_mb")" = 256 ]

echo "prepare USB offline test passed"
