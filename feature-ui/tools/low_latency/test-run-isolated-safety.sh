#!/bin/sh
set -eu

script=${1:-./run-isolated-baseline.sh}
test_dir=$(mktemp -d /tmp/monitor-demo-safety-test.XXXXXX)

cleanup()
{
    case "$test_dir" in
        /tmp/monitor-demo-safety-test.*) rm -rf -- "$test_dir" ;;
    esac
}
trap cleanup EXIT HUP INT TERM

sed '/^restore_original()$/,$d' "$script" > "$test_dir/helpers.sh"
set -- /bin/true baseline
MONITOR_DEMO_TEST_SECONDS=1
export MONITOR_DEMO_TEST_SECONDS
. "$test_dir/helpers.sh"
input_format=NV16

rga_plane_fixture=$test_dir/drm-state-rga
client_log=$test_dir/client-sync.log
printf '%s\n' 'RGA landscape path enabled: ROT_270' > "$client_log"
cat > "$rga_plane_fixture" <<'EOF'
plane[131]: Esmart3-win0
    crtc=crtc-124
    fb=43
    format=NV16
    modifier=0x0
    size=1080x1920
EOF
set -- $(plane_info "$rga_plane_fixture")
[ "$1" = 131 ]
[ "$2" = 43 ]

: > "$client_log"
printf '%s\n' 'RGA landscape path enabled: ROT_270' >> "$client_log"
trace_sequence=1
while [ "$trace_sequence" -le 30 ]; do
    trace_index=$((trace_sequence % 4))
    printf 'SYNC event=dqbuf sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=rga-submit sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=rga-release sequence=%s index=%s fence_fd=10\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=rga-wait sequence=%s index=%s fence_fd=11\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=rga-done sequence=%s index=%s fence_fd=11\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=commit sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=implicit-release sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=qbuf sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    trace_sequence=$((trace_sequence + 1))
done >> "$client_log"
printf '%s\n' 'SYNC event=shutdown sequence=0 index=-1 fence_fd=-1' \
    >> "$client_log"
validate_trace > "$test_dir/implicit-rga-trace-summary"
grep -qx 'rga_complete=30' "$test_dir/implicit-rga-trace-summary"

mode=explicit
: > "$client_log"
printf '%s\n' 'RGA landscape path enabled: ROT_270' >> "$client_log"
trace_sequence=1
while [ "$trace_sequence" -le 30 ]; do
    trace_index=$((trace_sequence % 4))
    printf 'SYNC event=dqbuf sequence=%s index=%s fence_fd=20\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=rga-submit sequence=%s index=%s fence_fd=20\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=rga-release sequence=%s index=%s fence_fd=21\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=acquire-set sequence=%s index=%s fence_fd=21\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=commit sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=fenced-release sequence=%s index=%s fence_fd=22\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=release-signaled sequence=%s index=%s fence_fd=22\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=rga-wait sequence=%s index=%s fence_fd=23\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=rga-done sequence=%s index=%s fence_fd=23\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=qbuf sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    trace_sequence=$((trace_sequence + 1))
done >> "$client_log"
cat >> "$client_log" <<'EOF'
SYNC event=dqbuf sequence=31 index=3 fence_fd=20
SYNC event=rga-submit sequence=31 index=3 fence_fd=20
SYNC event=rga-release sequence=31 index=3 fence_fd=21
SYNC event=acquire-set sequence=31 index=3 fence_fd=21
SYNC event=commit sequence=31 index=3 fence_fd=-1
SYNC event=dqbuf sequence=32 index=0 fence_fd=20
SYNC event=rga-submit sequence=32 index=0 fence_fd=20
SYNC event=rga-release sequence=32 index=0 fence_fd=21
SYNC event=acquire-set sequence=32 index=0 fence_fd=21
SYNC event=commit sequence=32 index=0 fence_fd=-1
SYNC event=fenced-release sequence=32 index=0 fence_fd=22
SYNC event=shutdown sequence=0 index=-1 fence_fd=-1
SYNC event=rga-wait sequence=31 index=3 fence_fd=23
SYNC event=rga-done sequence=31 index=3 fence_fd=23
SYNC event=rga-wait sequence=32 index=0 fence_fd=23
SYNC event=rga-done sequence=32 index=0 fence_fd=23
EOF
validate_trace > "$test_dir/explicit-rga-trace-summary"
grep -qx 'rga_complete=32' "$test_dir/explicit-rga-trace-summary"
grep -qx 'incomplete_at_shutdown=2' "$test_dir/explicit-rga-trace-summary"

rga_trace_log=$client_log
client_log=$test_dir/rga-trace-without-banner.log
sed '1d' "$rga_trace_log" > "$client_log"
if validate_trace > "$test_dir/rga-trace-without-banner-summary"; then
    printf '%s\n' 'RGA trace without enable marker was accepted' >&2
    exit 1
fi
grep -q 'invalid explicit RGA submit' \
    "$test_dir/rga-trace-without-banner-summary"

client_log=$test_dir/rga-trace-duplicate-done.log
awk '
    { print }
    !added && /event=rga-done/ { print; added = 1 }
' "$rga_trace_log" > "$client_log"
if validate_trace > "$test_dir/rga-trace-duplicate-done-summary"; then
    printf '%s\n' 'Duplicate RGA completion was accepted' >&2
    exit 1
fi
grep -q 'invalid explicit RGA completion' \
    "$test_dir/rga-trace-duplicate-done-summary"

mode=baseline
client_log=$test_dir/legacy-implicit-trace.log
: > "$client_log"
trace_sequence=1
while [ "$trace_sequence" -le 30 ]; do
    trace_index=$((trace_sequence % 4))
    printf 'SYNC event=dqbuf sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=commit sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=implicit-release sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=qbuf sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    trace_sequence=$((trace_sequence + 1))
done >> "$client_log"
validate_trace > "$test_dir/legacy-implicit-trace-summary"
grep -qx 'implicit_complete=30' "$test_dir/legacy-implicit-trace-summary"
grep -qx 'rga_complete=0' "$test_dir/legacy-implicit-trace-summary"

mode=explicit
client_log=$test_dir/legacy-explicit-trace.log
: > "$client_log"
trace_sequence=1
while [ "$trace_sequence" -le 30 ]; do
    trace_index=$((trace_sequence % 4))
    printf 'SYNC event=dqbuf sequence=%s index=%s fence_fd=20\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=acquire-set sequence=%s index=%s fence_fd=20\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=commit sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=fenced-release sequence=%s index=%s fence_fd=22\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=release-signaled sequence=%s index=%s fence_fd=22\n' \
        "$trace_sequence" "$trace_index"
    printf 'SYNC event=qbuf sequence=%s index=%s fence_fd=-1\n' \
        "$trace_sequence" "$trace_index"
    trace_sequence=$((trace_sequence + 1))
done >> "$client_log"
validate_trace > "$test_dir/legacy-explicit-trace-summary"
grep -qx 'explicit_fenced_complete=30' "$test_dir/legacy-explicit-trace-summary"
grep -qx 'rga_complete=0' "$test_dir/legacy-explicit-trace-summary"

mode=baseline
client_log=$test_dir/client-sync.log
: > "$client_log"
input_size=3840x2160

prepare_paths()
{
    scenario=$1
    mkdir -p "$scenario"
    parameter=$scenario/low_latency
    lock_token_file=$scenario/token
    lock_token_tmp=$scenario/token.tmp
    watchdog_fired=$scenario/watchdog-fired
    watchdog_armed=$scenario/watchdog-armed
    watchdog_armed_tmp=$scenario/watchdog-armed.tmp
    watchdog_result=$scenario/watchdog-result
    watchdog_result_tmp=$scenario/watchdog-result.tmp
    duration=1
}

wait_for_file()
{
    waited_path=$1
    waited_limit=${2:-40}
    waited_counter=0
    while [ ! -e "$waited_path" ] && [ "$waited_counter" -lt "$waited_limit" ]; do
        sleep 0.1
        waited_counter=$((waited_counter + 1))
    done
    [ -e "$waited_path" ]
}

wait_for_stopped()
{
    waited_pid=$1
    waited_limit=${2:-40}
    waited_counter=0
    while process_running "$waited_pid" \
            && [ "$waited_counter" -lt "$waited_limit" ]; do
        sleep 0.1
        waited_counter=$((waited_counter + 1))
    done
    ! process_running "$waited_pid"
}

wait_for_line()
{
    waited_pattern=$1
    waited_path=$2
    waited_limit=${3:-40}
    waited_counter=0
    while ! grep -qx "$waited_pattern" "$waited_path" 2>/dev/null \
            && [ "$waited_counter" -lt "$waited_limit" ]; do
        sleep 0.1
        waited_counter=$((waited_counter + 1))
    done
    grep -qx "$waited_pattern" "$waited_path" 2>/dev/null
}

[ "$(driver_release_point_for_fps 50.00)" = two-thirds-frame ]
[ "$(driver_release_point_for_fps 59.94)" = line-10 ]

plane_fixture=$test_dir/drm-state
cat > "$plane_fixture" <<'EOF'
plane[107]: Esmart2-win0
    crtc=crtc-123
    fb=42
    format=NV16
    modifier=0x0
    size=3840x2160
EOF
set -- $(plane_info "$plane_fixture")
[ "$1" = 107 ]
[ "$2" = 42 ]
input_format=NV24
if plane_info "$plane_fixture" >/dev/null 2>&1; then
    printf '%s\n' 'plane format validation accepted the wrong input format' >&2
    exit 1
fi
input_format=NV16

start_client()
{
    event_file=$1
    python3 - "$parameter" "$event_file" <<'PY' &
import signal
import sys
import time

parameter, event_file = sys.argv[1:]
ready_file = event_file + ".ready"

def stop(_signum, _frame):
    signal.signal(signal.SIGINT, signal.SIG_DFL)
    signal.signal(signal.SIGTERM, signal.SIG_DFL)
    with open(parameter, encoding="ascii") as stream:
        value = stream.read().strip()
    with open(event_file, "w", encoding="ascii") as stream:
        stream.write("pass\n" if value == "N" else "fail\n")
    time.sleep(0.5)
    raise SystemExit(0)

signal.signal(signal.SIGINT, stop)
signal.signal(signal.SIGTERM, stop)
with open(ready_file, "w", encoding="ascii"):
    pass
while True:
    time.sleep(0.1)
PY
    client_pid=$!
    client_start_time=$(process_start_time "$client_pid")
    client_int_sent=0
    wait_for_file "$event_file.ready" 20
}

prepare_paths "$test_dir/order"
printf '%s\n' Y > "$parameter"
start_client "$scenario/client-event"
watchdog_owner="test:$$:order"
printf '%s:%s:%s\n' "$watchdog_owner" "$client_pid" \
    "$client_start_time" > "$lock_token_file"
(safety_watchdog "$watchdog_owner") &
test_watchdog_pid=$!
wait_for_line 'client_stop=INT' "$watchdog_result" 30
printf '%s\n' inactive > "$lock_token_file"
wait "$test_watchdog_pid"
wait "$client_pid" || true
[ "$(cat "$parameter")" = N ]
[ "$(cat "$scenario/client-event")" = pass ]
grep -qx 'low_latency=pass' "$watchdog_result"
grep -qx 'client_stop=INT' "$watchdog_result"

prepare_paths "$test_dir/normal-stop"
printf '%s\n' Y > "$parameter"
start_client "$scenario/client-event"
client_int_sent=0
stop_client_safely
[ "$last_wait_status" -eq 0 ]
[ "$(cat "$parameter")" = N ]
[ "$(cat "$scenario/client-event")" = pass ]
[ "$(wc -l < "$scenario/client-event")" -eq 1 ]
client_pid=

prepare_paths "$test_dir/pre-enable-expired"
printf '%s\n' N > "$parameter"
sleep 30 &
watchdog_pid=$!
watchdog_start_time=$(process_start_time "$watchdog_pid")
watchdog_owner="test:$$:pre-enable-expired"
watchdog_token="$watchdog_owner:pending"
printf '%s\n' "$watchdog_token" > "$lock_token_file"
: > "$watchdog_fired"
if enable_low_latency_y_safely; then
    printf '%s\n' 'low_latency was enabled after watchdog expiry' >&2
    exit 1
fi
[ "$(cat "$parameter")" = N ]
kill -TERM "$watchdog_pid" 2>/dev/null || true
wait "$watchdog_pid" 2>/dev/null || true
watchdog_pid=

prepare_paths "$test_dir/enable-race"
printf '%s\n' N > "$parameter"
if (
    guard_calls=0
    watchdog_guards_client()
    {
        guard_calls=$((guard_calls + 1))
        [ "$guard_calls" -eq 1 ]
    }
    enable_low_latency_y_safely
); then
    printf '%s\n' 'low_latency remained enabled after watchdog guard loss' >&2
    exit 1
fi
[ "$(cat "$parameter")" = N ]

prepare_paths "$test_dir/fired-publication"
printf '%s\n' Y > "$parameter"
start_client "$scenario/client-event"
watchdog_owner="test:$$:fired-publication"
printf '%s:%s:%s\n' "$watchdog_owner" "$client_pid" \
    "$client_start_time" > "$lock_token_file"
lock_owned=1
(safety_watchdog "$watchdog_owner") &
watchdog_pid=$!
watchdog_start_time=$(process_start_time "$watchdog_pid")
wait_for_file "$watchdog_fired"
grep -qx 'client_stop=pending' "$watchdog_result"
stop_watchdog
[ -s "$watchdog_result" ]
watchdog_result_is_safe
wait "$client_pid" 2>/dev/null || true
client_pid=
lock_owned=0

probe_fixture=$test_dir/probe-fixture.sh
{
    printf '%s\n' '#!/bin/sh'
    printf '%s\n' 'if [ -n "${PROBE_ARGS_FILE:-}" ]; then'
    printf '%s\n' '    printf "%s\n" "$*" > "$PROBE_ARGS_FILE"'
    printf '%s\n' 'fi'
    printf '%s\n' 'if [ "${PROBE_FIXTURE_SLEEP:-0}" -ne 0 ]; then'
    printf '%s\n' '    exec sleep "$PROBE_FIXTURE_SLEEP"'
    printf '%s\n' 'fi'
    printf '%s\n' \
        'printf "%s\n" "Wayland DMA-BUF and explicit sync are available" >&2'
} > "$probe_fixture"
chmod +x "$probe_fixture"
prepare_paths "$test_dir/probe-bounded"
binary=$probe_fixture
runtime_dir=$scenario
socket_name=test-socket
probe_log=$scenario/probe.log
PROBE_ARGS_FILE=$scenario/probe-args
export PROBE_ARGS_FILE
export PROBE_FIXTURE_SLEEP=30
probe_started=$(date +%s)
if run_explicit_capability_probe; then
    printf '%s\n' 'blocked capability probe unexpectedly succeeded' >&2
    exit 1
fi
probe_elapsed=$(($(date +%s) - probe_started))
[ "$probe_elapsed" -ge 4 ]
[ "$probe_elapsed" -le 7 ]
PROBE_FIXTURE_SLEEP=0
export PROBE_FIXTURE_SLEEP
run_explicit_capability_probe
grep -qx 'Wayland DMA-BUF and explicit sync are available' "$probe_log"
grep -qx -- '-g -d NV16 --probe' "$PROBE_ARGS_FILE"
binary=/bin/true

prepare_paths "$test_dir/reused-pid"
printf '%s\n' Y > "$parameter"
start_client "$scenario/client-event"
watchdog_owner="test:$$:reused"
wrong_start_time=$((client_start_time + 1))
printf '%s:%s:%s\n' "$watchdog_owner" "$client_pid" \
    "$wrong_start_time" > "$lock_token_file"
(safety_watchdog "$watchdog_owner") &
test_watchdog_pid=$!
wait_for_line 'client_stop=identity-changed' "$watchdog_result" 30
printf '%s\n' inactive > "$lock_token_file"
wait "$test_watchdog_pid"
[ "$(cat "$parameter")" = N ]
kill -0 "$client_pid"
[ ! -e "$scenario/client-event" ]
grep -qx 'client_stop=identity-changed' "$watchdog_result"
kill -KILL "$client_pid" 2>/dev/null || true
wait "$client_pid" 2>/dev/null || true

prepare_paths "$test_dir/stale-token"
printf '%s\n' Y > "$parameter"
watchdog_owner="test:$$:stale"
printf '%s\n' 'inactive:test' > "$lock_token_file"
(safety_watchdog "$watchdog_owner")
[ "$(cat "$parameter")" = Y ]
[ ! -e "$watchdog_fired" ]

prepare_paths "$test_dir/arm-failure"
printf '%s\n' Y > "$parameter"
watchdog_owner="test:$$:arm-failure"
printf '%s\n' "$watchdog_owner:pending" > "$lock_token_file"
watchdog_armed_tmp=$scenario/missing/watchdog-armed.tmp
arm_status=0
(safety_watchdog "$watchdog_owner") 2>/dev/null || arm_status=$?
[ "$arm_status" -ne 0 ]
[ "$(cat "$parameter")" = Y ]
[ ! -e "$watchdog_fired" ]

prepare_paths "$test_dir/killed-parent"
printf '%s\n' Y > "$parameter"
(
    start_client "$scenario/client-event"
    watchdog_owner="test:killed-parent"
    printf '%s:%s:%s\n' "$watchdog_owner" "$client_pid" \
        "$client_start_time" > "$lock_token_file"
    (safety_watchdog "$watchdog_owner") &
    printf '%s\n' "$!" > "$scenario/watchdog-pid"
    printf '%s\n' "$client_pid" > "$scenario/client-pid"
    : > "$scenario/ready"
    while :; do sleep 1; done
) &
parent_pid=$!
counter=0
while [ ! -e "$scenario/ready" ] && [ "$counter" -lt 20 ]; do
    sleep 0.1
    counter=$((counter + 1))
done
[ -e "$scenario/ready" ]
killed_client_pid=$(cat "$scenario/client-pid")
killed_watchdog_pid=$(cat "$scenario/watchdog-pid")
kill -KILL "$parent_pid"
wait "$parent_pid" 2>/dev/null || true
counter=0
while [ "$(cat "$parameter")" != N ] && [ "$counter" -lt 40 ]; do
    sleep 0.1
    counter=$((counter + 1))
done
[ "$(cat "$parameter")" = N ]
if ! wait_for_stopped "$killed_client_pid"; then
    printf '%s\n' 'watchdog did not stop the killed parent client' >&2
    exit 1
fi
wait_for_line 'client_stop=INT' "$watchdog_result" 30
printf '%s\n' inactive > "$lock_token_file"
if ! wait_for_stopped "$killed_watchdog_pid"; then
    printf '%s\n' 'orphaned watchdog did not stop after token invalidation' >&2
    exit 1
fi

prepare_paths "$test_dir/killed-before-token"
printf '%s\n' N > "$parameter"
client_gate=$scenario/client-start.gate
(
    watchdog_owner="test:killed-before-token"
    printf '%s\n' "$watchdog_owner:pending" > "$lock_token_file"
    (safety_watchdog "$watchdog_owner") &
    printf '%s\n' "$!" > "$scenario/watchdog-pid"
    (
        if wait_for_client_gate "$client_gate"; then
            : > "$scenario/client-opened"
            while :; do sleep 1; done
        fi
    ) &
    printf '%s\n' "$!" > "$scenario/client-pid"
    : > "$scenario/ready"
    while :; do sleep 1; done
) &
parent_pid=$!
wait_for_file "$scenario/ready" 20
gated_client_pid=$(cat "$scenario/client-pid")
gated_watchdog_pid=$(cat "$scenario/watchdog-pid")
kill -KILL "$parent_pid"
wait "$parent_pid" 2>/dev/null || true
[ "$(cat "$parameter")" = N ]
if ! wait_for_stopped "$gated_client_pid" 70; then
    printf '%s\n' 'gated client survived parent death before token publication' >&2
    exit 1
fi
[ ! -e "$scenario/client-opened" ]
wait_for_line 'client_stop=not-started' "$watchdog_result" 30
printf '%s\n' inactive > "$lock_token_file"
if ! wait_for_stopped "$gated_watchdog_pid"; then
    printf '%s\n' 'pre-token watchdog did not stop after invalidation' >&2
    exit 1
fi

signal_dir=$test_dir/deferred-signal
mkdir -p "$signal_dir"
signal_status=0
sh -c '
    set -eu
    helper_file=$1
    signal_dir=$2
    set -- /bin/true baseline
    MONITOR_DEMO_TEST_SECONDS=1
    export MONITOR_DEMO_TEST_SECONDS
    . "$helper_file"
    spawned_pid=
    cleanup_spawn()
    {
        saved_status=$?
        trap - EXIT
        printf "%s\n" cleanup >> "$signal_dir/cleanup-count"
        if [ -n "$spawned_pid" ]; then
            kill -TERM "$spawned_pid" 2>/dev/null || true
            wait "$spawned_pid" 2>/dev/null || true
        fi
        exit "$saved_status"
    }
    trap cleanup_spawn EXIT
    defer_spawn_signals
    sleep 30 &
    kill -TERM "$$"
    spawned_pid=$!
    printf "%s\n" "$spawned_pid" > "$signal_dir/spawned-pid"
    finish_deferred_spawn
' sh "$test_dir/helpers.sh" "$signal_dir" || signal_status=$?
[ "$signal_status" -eq 143 ]
[ "$(wc -l < "$signal_dir/cleanup-count")" -eq 1 ]
deferred_pid=$(cat "$signal_dir/spawned-pid")
if process_running "$deferred_pid"; then
    printf '%s\n' 'deferred signal left a spawned child running' >&2
    exit 1
fi

prepare_paths "$test_dir/blocked-reader"
printf '%s\n' Y > "$parameter"
mkfifo "$scenario/drm-state"
drm_state_source=$scenario/drm-state
start_client "$scenario/client-event"
started=$(date +%s)
if capture_drm_snapshot "$scenario/snapshot"; then
    printf '%s\n' 'blocked DRM reader unexpectedly succeeded' >&2
    exit 1
fi
elapsed=$(($(date +%s) - started))
[ "$elapsed" -le 3 ]
[ "$(cat "$parameter")" = N ]
stop_client_safely
client_pid=
counter=0
while [ ! -e "$scenario/client-event" ] && [ "$counter" -lt 20 ]; do
    sleep 0.1
    counter=$((counter + 1))
done
[ "$(cat "$scenario/client-event")" = pass ]
stop_child "$state_pid" TERM 'test DRM reader'
state_pid=

printf '%s\n' \
    'PASS release-point dynamic-format rga-trace watchdog-order normal-stop pre-enable-expiry enable-race fired-publication bounded-probe pid-reuse stale-token arm-failure killed-parent killed-before-token deferred-signal bounded-reader'
