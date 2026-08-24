#!/bin/sh

set -eu

PATH=/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin
original_ui=/etc/init.d/S50systemui
original_weston=/etc/init.d/S49weston
monitor_weston=/etc/monitor_demo/staged-init/S49monitor_weston
supervisor=/usr/libexec/monitor-demo-supervisor
app=/opt/monitor_demo/current/monitor_demo
helper=/opt/monitor_demo/current/v4l2_wayland_explicit_sync
usb_prepare=/usr/libexec/monitor-demo-prepare-usb
supervisor_pid_file=/run/monitor-demo-supervisor.pid
app_pid_file=/run/monitor-demo.pid
stop_file=/run/monitor-demo.stop
low_latency_path=/sys/module/rockchip_hdmirx/parameters/low_latency
video_device=/dev/video80
wait_attempts=50
wait_interval=0.1
supervisor_stop_timeout=7
restoring=0
supervisor_pid=
mode="${1:-explicit}"

wait_for_weston_stopped()
{
    counter=0
    while { pgrep -x weston >/dev/null 2>&1 \
            || [ -S "${XDG_RUNTIME_DIR:-/run}/wayland-0" ]; } \
            && [ "$counter" -lt "$wait_attempts" ]; do
        sleep "$wait_interval"
        counter=$((counter + 1))
    done
    ! pgrep -x weston >/dev/null 2>&1 \
        && [ ! -S "${XDG_RUNTIME_DIR:-/run}/wayland-0" ]
}

wait_for_wayland_ready()
{
    counter=0
    while { ! pgrep -x weston >/dev/null 2>&1 \
            || [ ! -S "${XDG_RUNTIME_DIR:-/run}/wayland-0" ]; } \
            && [ "$counter" -lt "$wait_attempts" ]; do
        sleep "$wait_interval"
        counter=$((counter + 1))
    done
    pgrep -x weston >/dev/null 2>&1 \
        && [ -S "${XDG_RUNTIME_DIR:-/run}/wayland-0" ]
}

wait_for_original_ui()
{
    counter=0
    while { ! pgrep -x systemui >/dev/null 2>&1 \
            || ! pgrep -x controlcenter >/dev/null 2>&1; } \
            && [ "$counter" -lt "$wait_attempts" ]; do
        sleep "$wait_interval"
        counter=$((counter + 1))
    done
    pgrep -x systemui >/dev/null 2>&1 \
        && pgrep -x controlcenter >/dev/null 2>&1
}

product_processes_running()
{
    pgrep -f "$app" >/dev/null 2>&1 \
        || pgrep -f '[v]4l2_wayland_explicit_sync' >/dev/null 2>&1
}

wait_for_product_processes_stopped()
{
    counter=0
    while product_processes_running \
            && [ "$counter" -lt "$wait_attempts" ]; do
        sleep "$wait_interval"
        counter=$((counter + 1))
    done
    ! product_processes_running
}

signal_product_processes()
{
    signal="$1"
    for pid in $(pgrep -f "$app" 2>/dev/null || true) \
            $(pgrep -f '[v]4l2_wayland_explicit_sync' 2>/dev/null || true); do
        [ "$pid" = "$$" ] || kill "-$signal" "$pid" 2>/dev/null || true
    done
}

stop_product_processes()
{
    if ! product_processes_running; then
        rm -f "$app_pid_file"
        return 0
    fi

    signal_product_processes TERM
    if ! wait_for_product_processes_stopped; then
        signal_product_processes KILL
        wait_for_product_processes_stopped || return 1
    fi
    rm -f "$app_pid_file"
}

stop_supervisor()
{
    [ -n "$supervisor_pid" ] || return 0
    target_pid="$supervisor_pid"
    touch "$stop_file"

    if kill -0 "$target_pid" 2>/dev/null; then
        kill "$target_pid" 2>/dev/null || true
        (
            sleep "$supervisor_stop_timeout"
            if kill -0 "$target_pid" 2>/dev/null; then
                kill -KILL "$target_pid" 2>/dev/null || true
            fi
        ) &
        killer=$!
        wait "$target_pid" 2>/dev/null || true
        kill "$killer" 2>/dev/null || true
        wait "$killer" 2>/dev/null || true
    else
        wait "$target_pid" 2>/dev/null || true
    fi

    supervisor_pid=
    rm -f "$supervisor_pid_file"
}

force_low_latency_off()
{
    [ -w "$low_latency_path" ] && [ -r "$low_latency_path" ] || return 1
    printf '0\n' >"$low_latency_path" || return 1
    [ "$(cat "$low_latency_path")" = N ]
}

wait_for_video_device_free()
{
    counter=0
    while fuser "$video_device" >/dev/null 2>&1 \
            && [ "$counter" -lt "$wait_attempts" ]; do
        sleep "$wait_interval"
        counter=$((counter + 1))
    done
    ! fuser "$video_device" >/dev/null 2>&1
}

restore_original()
{
    [ "$restoring" -eq 0 ] || return 0
    restoring=1
    restore_failed=0

    stop_supervisor || restore_failed=1
    if ! stop_product_processes; then
        echo "monitor application or helper did not stop" >&2
        restore_failed=1
    fi
    if ! force_low_latency_off; then
        echo "failed to restore low_latency=N" >&2
        restore_failed=1
    fi
    if ! wait_for_video_device_free; then
        echo "$video_device is still busy" >&2
        restore_failed=1
    fi
    if ! "$monitor_weston" stop || ! wait_for_weston_stopped; then
        echo "monitor Weston did not stop" >&2
        restore_failed=1
    fi

    if "$original_weston" start >/dev/null 2>&1 \
            && wait_for_wayland_ready; then
        if ! "$original_ui" start || ! wait_for_original_ui; then
            echo "original SystemUI or ControlCenter did not start" >&2
            restore_failed=1
        fi
    else
        echo "original Weston did not start" >&2
        restore_failed=1
    fi

    rm -f "$stop_file"
    [ "$restore_failed" -eq 0 ]
}

finish()
{
    status=$?
    trap - EXIT HUP INT TERM
    if ! restore_original; then
        status=75
    fi
    exit "$status"
}

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

if [ ! -x "$original_ui" ] || [ ! -x "$original_weston" ] \
        || [ ! -x "$monitor_weston" ] || [ ! -x "$supervisor" ] \
        || [ ! -x "$app" ] || [ ! -x "$helper" ] \
        || [ ! -x "$usb_prepare" ]; then
    echo "required validation file is missing" >&2
    exit 66
fi
"$usb_prepare"

if pgrep -f "$supervisor" >/dev/null 2>&1 || product_processes_running; then
    echo "monitor supervisor, application, or helper is already running" >&2
    exit 75
fi
if fuser "$video_device" >/dev/null 2>&1; then
    echo "$video_device is already busy" >&2
    exit 75
fi
if ! force_low_latency_off; then
    echo "cannot establish low_latency=N before validation" >&2
    exit 74
fi
rm -f "$supervisor_pid_file" "$app_pid_file" "$stop_file"

trap finish EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

"$original_ui" stop || true
"$original_weston" stop || true
if ! wait_for_weston_stopped; then
    echo "original Weston did not stop" >&2
    exit 74
fi
"$monitor_weston" start

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run}"
export QT_QPA_PLATFORM=wayland
release_dir="$(dirname "$(readlink -f "$app")")"
export LD_LIBRARY_PATH="$release_dir:$release_dir/CrAdapter${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cd "$release_dir"

"$supervisor" "$app" &
supervisor_pid=$!
printf '%s\n' "$supervisor_pid" >"$supervisor_pid_file"
supervisor_status=0
wait "$supervisor_pid" || supervisor_status=$?
supervisor_pid=
exit "$supervisor_status"
