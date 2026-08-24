#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
: "${SDK_ROOT:?Set SDK_ROOT to the RK3588 Linux SDK root}"
sdk_root=$SDK_ROOT
output_root="$sdk_root/buildroot/output/alientek_rk3588"
host_root="$output_root/host"
sysroot="$host_root/aarch64-buildroot-linux-gnu/sysroot"
weston_source="$output_root/build/weston-14.0.2"
weston_build="$weston_source/build"
protocol="$weston_build/protocol"
build_dir=${BUILD_DIR:-"$script_dir/build"}
binary="$build_dir/v4l2_wayland_explicit_sync"
cc="$host_root/bin/aarch64-buildroot-linux-gnu-gcc"

mkdir -p "$build_dir"

"$cc" \
    -I"$weston_build/clients/weston-simple-dmabuf-v4l.p" \
    -I"$weston_build/clients" \
    -I"$weston_source/clients" \
    -I"$weston_build" \
    -I"$weston_source" \
    -I"$weston_build/include" \
    -I"$weston_source/include" \
    -I"$protocol" \
    -I"$sysroot/usr/include" \
    -I"$sysroot/usr/include/libdrm" \
    -std=gnu99 -O2 -Wall -Wextra -Wpedantic -Wno-pedantic \
    -Wno-unused-parameter -Wno-shift-negative-value \
    -Wno-missing-field-initializers -D_LARGEFILE_SOURCE \
    -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64 -D_FORTIFY_SOURCE=1 \
    "$script_dir/v4l2_wayland_explicit_sync.c" \
    "$protocol/linux-dmabuf-unstable-v1-protocol.c" \
    "$protocol/xdg-shell-protocol.c" \
    "$protocol/weston-direct-display-protocol.c" \
    "$protocol/viewporter-protocol.c" \
    "$protocol/linux-explicit-synchronization-unstable-v1-protocol.c" \
    -Wl,--as-needed -Wl,--no-undefined -Wl,-O1 \
    "$sysroot/usr/lib/libwayland-client.so" \
    "$sysroot/usr/lib/libwayland-cursor.so" \
    -o "$binary"

file "$binary"
printf '%s\n' "$binary"
