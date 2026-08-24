#!/bin/sh

set -eu

script_dir="$(CDPATH= cd "$(dirname "$0")" && pwd)"
source_script="$script_dir/test-isolated-weston.sh"
test_root="$(mktemp -d "${TMPDIR:-/tmp}/isolated-weston-test.XXXXXX")"
test_shell="${TEST_SCRIPT_SHELL:-sh}"
validation_pid=

cleanup()
{
    if [ -n "$validation_pid" ]; then
        kill -KILL "$validation_pid" 2>/dev/null || true
        wait "$validation_pid" 2>/dev/null || true
    fi
    for pid_file in "$test_root"/*/state/socket-server.pid \
            "$test_root"/*/state/supervisor.pid; do
        [ -r "$pid_file" ] || continue
        kill -KILL "$(cat "$pid_file")" 2>/dev/null || true
    done
    rm -rf "$test_root"
}
trap cleanup EXIT HUP INT TERM

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

make_fixture()
{
    case_root="$1"
    mkdir -p "$case_root/bin" "$case_root/run" "$case_root/release" \
             "$case_root/state"

    cat >"$case_root/bin/socket-server" <<'EOF'
#!/usr/bin/perl
use strict;
use warnings;
use IO::Socket::UNIX;
use Socket qw(SOCK_STREAM);
my $socket = IO::Socket::UNIX->new(
    Type => SOCK_STREAM,
    Local => $ARGV[0],
    Listen => 1,
) or die "$!";
$SIG{TERM} = sub { exit 0 };
sleep 60;
EOF

    cat >"$case_root/bin/weston-control" <<'EOF'
#!/bin/sh
name="$(basename "$0")"
socket="$XDG_RUNTIME_DIR/wayland-0"
case "$1" in
    start)
        rm -f "$socket"
        "$TEST_SOCKET_SERVER" "$socket" &
        echo "$!" >"$TEST_STATE/socket-server.pid"
        counter=0
        while [ ! -S "$socket" ] && [ "$counter" -lt 50 ]; do
            sleep 0.01
            counter=$((counter + 1))
        done
        [ -S "$socket" ] || exit 1
        touch "$TEST_STATE/weston" "$TEST_STATE/$name-start"
        ;;
    stop)
        if [ -r "$TEST_STATE/socket-server.pid" ]; then
            kill "$(cat "$TEST_STATE/socket-server.pid")" 2>/dev/null || true
            wait "$(cat "$TEST_STATE/socket-server.pid")" 2>/dev/null || true
        fi
        rm -f "$TEST_STATE/socket-server.pid" "$TEST_STATE/weston" "$socket"
        touch "$TEST_STATE/$name-stop"
        ;;
    *)
        exit 1
        ;;
esac
EOF
    cp "$case_root/bin/weston-control" "$case_root/bin/original-weston"
    cp "$case_root/bin/weston-control" "$case_root/bin/monitor-weston"

    cat >"$case_root/bin/original-ui" <<'EOF'
#!/bin/sh
case "$1" in
    start)
        touch "$TEST_STATE/original-ui-start"
        if [ "${TEST_UI_READY:-0}" -eq 1 ]; then
            touch "$TEST_STATE/systemui" "$TEST_STATE/controlcenter"
        fi
        ;;
    stop)
        rm -f "$TEST_STATE/systemui" "$TEST_STATE/controlcenter"
        touch "$TEST_STATE/original-ui-stop"
        ;;
    *)
        exit 1
        ;;
esac
EOF

    cat >"$case_root/bin/supervisor" <<'EOF'
#!/bin/sh
shutdown()
{
    touch "$TEST_STATE/supervisor-term"
    rm -f "$TEST_STATE/supervisor" "$TEST_STATE/application" \
          "$TEST_STATE/helper" "$TEST_STATE/video-busy"
    exit 0
}
trap shutdown HUP INT TERM
echo "$$" >"$TEST_STATE/supervisor.pid"
touch "$TEST_STATE/supervisor" "$TEST_STATE/application" \
      "$TEST_STATE/helper" "$TEST_STATE/video-busy"
while :; do
    sleep 0.05
done
EOF

    cat >"$case_root/bin/pgrep" <<'EOF'
#!/bin/sh
case "$*" in
    "-x weston")
        marker=weston
        ;;
    "-x systemui")
        marker=systemui
        ;;
    "-x controlcenter")
        marker=controlcenter
        ;;
    *supervisor*)
        marker=supervisor
        ;;
    *v4l2_wayland_explicit_sync*)
        marker=helper
        ;;
    */monitor_demo*)
        marker=application
        ;;
    *)
        exit 1
        ;;
esac
[ -e "$TEST_STATE/$marker" ] || exit 1
if [ -r "$TEST_STATE/$marker.pid" ]; then
    cat "$TEST_STATE/$marker.pid"
else
    echo 4242
fi
EOF

    cat >"$case_root/bin/fuser" <<'EOF'
#!/bin/sh
[ -e "$TEST_STATE/video-busy" ]
EOF

    cat >"$case_root/bin/cat" <<'EOF'
#!/bin/sh
if [ "$#" -eq 1 ] && [ "$1" = "$TEST_LOW_LATENCY_PATH" ]; then
    value="$("$TEST_REAL_CAT" "$1")"
    if [ "$value" = 0 ]; then
        echo N
    else
        echo "$value"
    fi
else
    exec "$TEST_REAL_CAT" "$@"
fi
EOF

    cat >"$case_root/bin/usb-prepare" <<'EOF'
#!/bin/sh
touch "$TEST_STATE/usb-prepared"
EOF
    cat >"$case_root/release/monitor_demo" <<'EOF'
#!/bin/sh
exit 0
EOF
    cat >"$case_root/release/v4l2_wayland_explicit_sync" <<'EOF'
#!/bin/sh
exit 0
EOF

    chmod 0755 "$case_root/bin/"* "$case_root/release/"*
    printf 'N\n' >"$case_root/low_latency"
    : >"$case_root/video80"

    # BusyBox ash bypasses PATH for applet built-ins, so override cat in-shell.
    sed \
        -e '/^set -eu$/a\
cat() { "$TEST_FAKE_CAT" "$@"; }' \
        -e "s|^PATH=.*|PATH=$case_root/bin:/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin|" \
        -e "s|^original_ui=.*|original_ui=$case_root/bin/original-ui|" \
        -e "s|^original_weston=.*|original_weston=$case_root/bin/original-weston|" \
        -e "s|^monitor_weston=.*|monitor_weston=$case_root/bin/monitor-weston|" \
        -e "s|^supervisor=.*|supervisor=$case_root/bin/supervisor|" \
        -e "s|^app=.*|app=$case_root/release/monitor_demo|" \
        -e "s|^helper=.*|helper=$case_root/release/v4l2_wayland_explicit_sync|" \
        -e "s|^usb_prepare=.*|usb_prepare=$case_root/bin/usb-prepare|" \
        -e "s|^supervisor_pid_file=.*|supervisor_pid_file=$case_root/run/supervisor.pid|" \
        -e "s|^app_pid_file=.*|app_pid_file=$case_root/run/app.pid|" \
        -e "s|^stop_file=.*|stop_file=$case_root/run/stop|" \
        -e "s|^low_latency_path=.*|low_latency_path=$case_root/low_latency|" \
        -e "s|^video_device=.*|video_device=$case_root/video80|" \
        -e 's|^wait_attempts=.*|wait_attempts=5|' \
        -e 's|^wait_interval=.*|wait_interval=0.01|' \
        -e 's|^supervisor_stop_timeout=.*|supervisor_stop_timeout=1|' \
        "$source_script" >"$case_root/test-isolated-weston.sh"
    chmod 0755 "$case_root/test-isolated-weston.sh"
}

wait_for_marker()
{
    marker="$1"
    counter=0
    while [ ! -e "$marker" ] && [ "$counter" -lt 100 ]; do
        sleep 0.01
        counter=$((counter + 1))
    done
    [ -e "$marker" ]
}

run_term_case()
{
    name="$1"
    ui_ready="$2"
    expected_status="$3"
    case_root="$test_root/$name"
    make_fixture "$case_root"

    export TEST_STATE="$case_root/state"
    export TEST_FAKE_CAT="$case_root/bin/cat"
    export TEST_SOCKET_SERVER="$case_root/bin/socket-server"
    export TEST_LOW_LATENCY_PATH="$case_root/low_latency"
    export TEST_REAL_CAT
    export TEST_UI_READY="$ui_ready"
    export XDG_RUNTIME_DIR="$case_root/run"

    "$case_root/bin/original-weston" start
    "$case_root/bin/original-ui" start
    rm -f "$TEST_STATE/original-weston-start" \
          "$TEST_STATE/original-ui-start"

    "$test_shell" "$case_root/test-isolated-weston.sh" baseline \
        >"$case_root/output.log" 2>&1 &
    validation_pid=$!
    if ! wait_for_marker "$TEST_STATE/supervisor.pid"; then
        cat "$case_root/output.log" >&2
        fail "$name did not start the supervisor"
    fi

    kill -TERM "$validation_pid"
    status=0
    wait "$validation_pid" || status=$?
    validation_pid=

    [ "$status" -eq "$expected_status" ] \
        || fail "$name returned $status, expected $expected_status"
    [ -e "$TEST_STATE/supervisor-term" ] \
        || fail "$name did not forward TERM to the supervisor"
    [ ! -e "$TEST_STATE/application" ] \
        || fail "$name left the application running"
    [ ! -e "$TEST_STATE/helper" ] \
        || fail "$name left the helper running"
    [ ! -e "$TEST_STATE/video-busy" ] \
        || fail "$name left the video device busy"
    [ "$(cat "$case_root/low_latency")" = 0 ] \
        || fail "$name did not write the low-latency safe value"
    [ -e "$TEST_STATE/monitor-weston-stop" ] \
        || fail "$name did not stop monitor Weston"
    [ -e "$TEST_STATE/original-weston-start" ] \
        || fail "$name did not restore original Weston"
    [ -e "$TEST_STATE/original-ui-start" ] \
        || fail "$name did not invoke original UI startup"

    if [ "$ui_ready" -eq 1 ]; then
        [ -e "$TEST_STATE/systemui" ] \
            || fail "$name did not restore SystemUI"
        [ -e "$TEST_STATE/controlcenter" ] \
            || fail "$name did not restore ControlCenter"
    else
        grep -q 'original SystemUI or ControlCenter did not start' \
            "$case_root/output.log" \
            || fail "$name did not report missing UI processes"
    fi

    "$case_root/bin/original-weston" stop
}

TEST_REAL_CAT="$(which cat)"
export TEST_REAL_CAT
run_term_case term-restores-ui 1 143
run_term_case ui-script-false-success 0 75

echo "PASS: isolated Weston signal cleanup and UI recovery"
