#!/bin/sh
set -eu

binary=${1:-/tmp/v4l2_wayland_explicit_sync}
mode=${2:-baseline}
duration=${MONITOR_DEMO_TEST_SECONDS:-8}

case "$mode" in
    baseline|explicit) ;;
    *)
        printf 'Usage: %s [binary] [baseline|explicit]\n' "$0" >&2
        exit 64
        ;;
esac
case "$duration" in
    ''|*[!0-9]*)
        printf '%s\n' 'MONITOR_DEMO_TEST_SECONDS must be a positive integer' >&2
        exit 64
        ;;
esac
if [ "$duration" -lt 1 ]; then
    printf '%s\n' 'MONITOR_DEMO_TEST_SECONDS must be at least 1' >&2
    exit 64
fi

device=/dev/video80
parameter=/sys/module/rockchip_hdmirx/parameters/low_latency
drm_name=/sys/kernel/debug/dri/0/name
drm_state_source=/sys/kernel/debug/dri/0/state
original_ui=/etc/init.d/S50systemui
original_weston=/etc/init.d/S49weston
weston_bin=/usr/bin/weston
weston_config=/etc/monitor_demo/weston.ini
runtime_dir=${XDG_RUNTIME_DIR:-/run}
socket_name=wayland-monitor-low-latency
evidence_dir=/tmp/monitor-demo-low-latency-$mode
weston_log=$evidence_dir/weston-drm.log
client_log=$evidence_dir/client-sync.log
probe_log=$evidence_dir/probe.log
preflight_log=$evidence_dir/preflight.log
drm_state=$evidence_dir/drm-state.txt
drm_state_final=$evidence_dir/drm-state-final.txt
drm_state_final_tmp=$evidence_dir/drm-state-final.tmp
trace_summary=$evidence_dir/trace-summary.txt
drm_summary=$evidence_dir/drm-summary.txt
result_file=$evidence_dir/result.txt
baseline_marker=/tmp/monitor-demo-low-latency-baseline.ok
lock_dir=/tmp/monitor-demo-low-latency.lock
lock_token_file=$lock_dir/token
lock_token_tmp=$lock_dir/token.tmp
watchdog_fired=$evidence_dir/watchdog-fired
watchdog_armed=$evidence_dir/watchdog-armed
watchdog_armed_tmp=$evidence_dir/watchdog-armed.tmp
watchdog_result=$evidence_dir/watchdog-result.txt
watchdog_result_tmp=$evidence_dir/watchdog-result.tmp
client_gate=$evidence_dir/client-start.gate

weston_pid=
original_weston_pid=
client_pid=
client_start_time=
client_int_sent=0
state_pid=
watchdog_pid=
watchdog_start_time=
watchdog_owner=
watchdog_token=
pending_signal_status=
lock_owned=0
validation_passed=0
last_wait_status=0
boot_id=
binary_hash=
input_size=
input_format=
input_fps=
timing_hash=
driver_release_point=

driver_release_point_for_fps()
{
    if awk -v fps="$1" 'BEGIN { exit(fps + 0 >= 59 ? 0 : 1) }'; then
        printf '%s\n' line-10
    else
        printf '%s\n' two-thirds-frame
    fi
}

plane_info()
{
    awk -v expected="$input_size" -v expected_format="$input_format" '
        function finish() {
            if (esmart && crtc && fb && format && linear && size) {
                found = 1
                found_plane = plane_id
                found_fb = fb_id
            }
        }
        /^plane\[/ {
            if (seen)
                finish()
            seen = 1
            plane_id = $0
            sub(/^plane\[/, "", plane_id)
            sub(/\].*/, "", plane_id)
            esmart = ($0 ~ /Esmart[0-3]-win0/)
            crtc = fb = format = linear = size = 0
            fb_id = 0
            next
        }
        seen && /^[^[:space:]]/ {
            finish()
            seen = 0
            next
        }
        seen && /^[[:space:]]*crtc=/ && $0 !~ /\(null\)/ { crtc = 1 }
        seen && /^[[:space:]]*fb=[1-9][0-9]*/ {
            fb = 1
            fb_id = $0
            sub(/^[[:space:]]*fb=/, "", fb_id)
        }
        seen && index($0, "format=" expected_format) { format = 1 }
        seen && /^[[:space:]]*modifier=0x0[[:space:]]*$/ { linear = 1 }
        seen && $0 ~ ("^[[:space:]]*size=" expected "[[:space:]]*$") { size = 1 }
        END {
            if (seen)
                finish()
            if (!found)
                exit 1
            print found_plane, found_fb
        }
    ' "$1"
}

validate_trace()
{
    awk -v mode="$mode" '
        function reject(message, key) {
            print "trace error: " message " [" key "]"
            bad = 1
        }
        $1 == "SYNC" {
            event = sequence = buffer_index = fence = ""
            for (i = 2; i <= NF; i++) {
                split($i, pair, "=")
                if (pair[1] == "event") event = pair[2]
                if (pair[1] == "sequence") sequence = pair[2]
                if (pair[1] == "index") buffer_index = pair[2]
                if (pair[1] == "fence_fd") fence = pair[2]
            }
            if (event == "" || sequence == "" || buffer_index == "" || fence == "") {
                reject("malformed SYNC line", NR)
                next
            }

            key = sequence "/" buffer_index
            if (mode == "baseline") {
                if (event == "dqbuf") {
                    if (state[key] != 0 || fence != -1)
                        reject("invalid implicit dqbuf", key)
                    else
                        state[key] = 1
                } else if (event == "commit") {
                    if (state[key] != 1)
                        reject("commit out of order", key)
                    else
                        state[key] = 2
                } else if (event == "implicit-release") {
                    if (state[key] != 2)
                        reject("implicit release out of order", key)
                    else
                        state[key] = 3
                } else if (event == "qbuf") {
                    if (state[key] != 3)
                        reject("qbuf before implicit release", key)
                    else {
                        state[key] = 4
                        complete++
                    }
                } else {
                    reject("explicit event in baseline: " event, key)
                }
            } else {
                if (event == "dqbuf") {
                    if (state[key] != 0 || fence + 0 <= 0)
                        reject("invalid explicit dqbuf", key)
                    else {
                        state[key] = 1
                        acquire[key] = fence
                    }
                } else if (event == "acquire-set") {
                    if (state[key] != 1 || fence != acquire[key])
                        reject("acquire fence mismatch", key)
                    else
                        state[key] = 2
                } else if (event == "commit") {
                    if (state[key] != 2)
                        reject("commit before acquire", key)
                    else
                        state[key] = 3
                } else if (event == "fenced-release") {
                    if (state[key] != 3 || fence + 0 < 0)
                        reject("fenced release out of order", key)
                    else {
                        state[key] = 4
                        release[key] = fence
                    }
                } else if (event == "release-signaled") {
                    if (state[key] != 4 || fence != release[key])
                        reject("release fence mismatch", key)
                    else
                        state[key] = 5
                } else if (event == "immediate-release") {
                    if (state[key] != 3)
                        reject("immediate release out of order", key)
                    else
                        state[key] = 6
                } else if (event == "qbuf") {
                    if (state[key] == 5) {
                        state[key] = 7
                        fenced++
                    } else if (state[key] == 6) {
                        state[key] = 7
                        immediate++
                    } else {
                        reject("qbuf before release", key)
                    }
                } else {
                    reject("implicit or unknown event: " event, key)
                }
            }
        }
        END {
            for (key in state) {
                if ((mode == "baseline" && state[key] != 4) ||
                    (mode == "explicit" && state[key] != 7))
                    incomplete++
            }
            if (incomplete > 4)
                reject("more than four incomplete shutdown frames", "all")
            if (mode == "baseline") {
                if (complete < 30)
                    reject("fewer than 30 complete implicit chains", "all")
                print "implicit_complete=" complete
            } else {
                if (fenced < 30)
                    reject("fewer than 30 complete fenced chains", "all")
                print "explicit_fenced_complete=" fenced
                print "explicit_immediate_complete=" immediate + 0
            }
            print "incomplete_at_shutdown=" incomplete + 0
            if (bad)
                exit 1
        }
    ' "$client_log"
}

validate_weston_drm()
{
    awk -v mode="$mode" -v plane="$plane_id" -v fb="$fb_id" \
        -v format="$input_format" '
        function reset_transaction() {
            applying = 0
            in_transaction = 0
            target_fb = 0
            target_crtc = 0
            target_format = 0
            target_fence = 0
        }
        function reject(message) {
            print "drm error: " message
            bad = 1
        }
        /atomic: could not|atomic: couldn.t (compile atomic|commit new) state/ {
            reject("atomic compile or commit failure")
        }
        /\[atomic\] applying output/ {
            if (!in_transaction) {
                reset_transaction()
                in_transaction = 1
                applying = 1
            } else if (!applying) {
                reset_transaction()
                in_transaction = 1
                applying = 1
            }
            next
        }
        /\[atomic\] testing output/ {
            if (!in_transaction || applying) {
                reset_transaction()
                in_transaction = 1
            }
            applying = 0
            next
        }
        in_transaction && applying && index($0, "[PLANE:" plane "]") {
            if ($0 ~ ("\\(FB_ID\\) -> " fb " \\("))
                target_fb = 1
            if ($0 ~ /\(CRTC_ID\) -> [1-9][0-9]* \(/) {
                value = $0
                sub(/.*\(CRTC_ID\) -> /, "", value)
                sub(/ .*/, "", value)
                target_crtc = value + 0
            }
            if (index($0, "FORMAT: " format))
                target_format = 1
            if ($0 ~ /\(IN_FENCE_FD\) -> [1-9][0-9]* \(/)
                target_fence = 1
        }
        /\[atomic\] drmModeAtomicCommit/ {
            if (in_transaction && applying && target_fb && target_crtc > 0 && target_format) {
                if (mode == "explicit" && !target_fence)
                    reject("matching APPLY transaction has no positive IN_FENCE_FD")
                else if (mode == "baseline" && target_fence)
                    reject("implicit APPLY transaction has a positive IN_FENCE_FD")
                else
                    pending[target_crtc]++
            }
            reset_transaction()
            next
        }
        /\[atomic\]\[CRTC:[0-9]+\] flip processing completed/ {
            for (crtc in pending) {
                if (pending[crtc] > 0 && index($0, "[CRTC:" crtc "]")) {
                    pending[crtc]--
                    complete++
                    break
                }
            }
        }
        END {
            if (complete < 30)
                reject("fewer than 30 matching APPLY -> commit -> flip transactions")
            print "drm_apply_flip_complete=" complete + 0
            if (bad)
                exit 1
        }
    ' "$weston_log"
}

process_running()
{
    [ -n "$1" ] && [ -n "$(readlink "/proc/$1/exe" 2>/dev/null)" ]
}

process_start_time()
{
    process_stat=$(cat "/proc/$1/stat" 2>/dev/null) || return 1
    process_stat=${process_stat##*) }
    set -- $process_stat
    [ "$#" -ge 20 ] || return 1
    printf '%s\n' "${20}"
}

same_process_running()
{
    [ -n "$1" ] && [ -n "$2" ] || return 1
    process_running "$1" || return 1
    current_start_time=$(process_start_time "$1" 2>/dev/null || true)
    [ "$current_start_time" = "$2" ]
}

install_signal_handlers()
{
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 131' QUIT
    trap 'exit 143' TERM
}

defer_spawn_signals()
{
    pending_signal_status=
    trap 'pending_signal_status=143' HUP INT QUIT TERM
}

finish_deferred_spawn()
{
    install_signal_handlers
    if [ -n "$pending_signal_status" ]; then
        deferred_status=$pending_signal_status
        pending_signal_status=
        exit "$deferred_status"
    fi
}

restore_low_latency_n()
{
    printf '%s\n' N > "$parameter" \
        && [ "$(cat "$parameter" 2>/dev/null)" = N ]
}

wait_for_client_gate()
{
    gate_path=$1
    gate_counter=0
    while [ ! -e "$gate_path" ] && [ "$gate_counter" -lt 50 ]; do
        sleep 0.1
        gate_counter=$((gate_counter + 1))
    done
    [ -e "$gate_path" ]
}

watchdog_guards_client()
{
    [ ! -e "$watchdog_fired" ] \
        && same_process_running "$watchdog_pid" "$watchdog_start_time" \
        && [ "$(cat "$lock_token_file" 2>/dev/null || true)" \
            = "$watchdog_token" ]
}

enable_low_latency_y_safely()
{
    watchdog_guards_client || return 1
    if ! printf '%s\n' Y > "$parameter" \
            || [ "$(cat "$parameter" 2>/dev/null)" != Y ]; then
        restore_low_latency_n || true
        return 1
    fi
    if ! watchdog_guards_client; then
        restore_low_latency_n || true
        return 1
    fi
}

run_explicit_capability_probe()
{
    if ! XDG_RUNTIME_DIR="$runtime_dir" WAYLAND_DISPLAY="$socket_name" \
            timeout 5 "$binary" -g -d "$input_format" --probe \
            > "$probe_log" 2>&1; then
        return 1
    fi
    grep -qx 'Wayland DMA-BUF and explicit sync are available' "$probe_log"
}

stop_child()
{
    pid=$1
    initial_signal=$2
    label=$3
    reap=${4:-yes}
    last_wait_status=0

    if process_running "$pid"; then
        kill -"$initial_signal" "$pid" 2>/dev/null || true
        counter=0
        while process_running "$pid" && [ "$counter" -lt 50 ]; do
            sleep 0.1
            counter=$((counter + 1))
        done
    fi
    if process_running "$pid"; then
        kill -KILL "$pid" 2>/dev/null || true
        counter=0
        while process_running "$pid" && [ "$counter" -lt 20 ]; do
            sleep 0.1
            counter=$((counter + 1))
        done
    fi
    if process_running "$pid"; then
        printf 'Failed to stop %s (pid %s)\n' "$label" "$pid" >&2
        return 1
    fi

    if [ "$reap" = yes ]; then
        wait "$pid" 2>/dev/null || last_wait_status=$?
    fi
    return 0
}

client_process_running()
{
    [ -n "$client_pid" ] || return 1
    if [ -n "$client_start_time" ]; then
        same_process_running "$client_pid" "$client_start_time"
    else
        process_running "$client_pid"
    fi
}

stop_client_safely()
{
    reap=${1:-yes}
    client_stop_ok=1
    last_wait_status=0

    if client_process_running; then
        if ! restore_low_latency_n; then
            client_stop_ok=0
        elif [ "$client_int_sent" -eq 0 ]; then
            if kill -INT "$client_pid" 2>/dev/null; then
                client_int_sent=1
            elif client_process_running; then
                client_stop_ok=0
            fi
        fi
        counter=0
        while client_process_running && [ "$counter" -lt 50 ]; do
            sleep 0.1
            counter=$((counter + 1))
        done
    fi
    if client_process_running; then
        kill -KILL "$client_pid" 2>/dev/null || true
        counter=0
        while client_process_running && [ "$counter" -lt 20 ]; do
            sleep 0.1
            counter=$((counter + 1))
        done
    fi
    if client_process_running; then
        printf 'Failed to stop client (pid %s)\n' "$client_pid" >&2
        return 1
    fi

    if [ "$reap" = yes ]; then
        wait "$client_pid" 2>/dev/null || last_wait_status=$?
    fi
    [ "$client_stop_ok" -eq 1 ]
}

write_watchdog_result()
{
    result_restore=$1
    result_client=$2
    {
        printf 'low_latency=%s\n' "$result_restore"
        printf 'client_stop=%s\n' "$result_client"
    } > "$watchdog_result_tmp" \
        && mv "$watchdog_result_tmp" "$watchdog_result"
}

watchdog_result_is_safe()
{
    [ -s "$watchdog_result" ] || return 1
    awk '
        $0 == "low_latency=pass" { low_latency++; next }
        $0 == "client_stop=INT" { client++; next }
        { bad = 1 }
        END {
            exit !(NR == 2 && low_latency == 1 && client == 1 && !bad)
        }
    ' "$watchdog_result"
}

watchdog_reported_client_signal()
{
    [ -s "$watchdog_result" ] \
        && grep -Eq '^client_stop=(INT|KILL|failed)$' "$watchdog_result"
}

invalidate_watchdog()
{
    [ "$lock_owned" -eq 1 ] || return 0
    write_watchdog_token "inactive:$$"
}

write_watchdog_token()
{
    token_value=$1
    printf '%s\n' "$token_value" > "$lock_token_tmp" \
        && mv "$lock_token_tmp" "$lock_token_file"
}

stop_watchdog()
{
    watchdog_stopped=1
    invalidate_watchdog || watchdog_stopped=0
    if [ -n "$watchdog_pid" ]; then
        if [ -n "$watchdog_start_time" ]; then
            counter=0
            while same_process_running "$watchdog_pid" "$watchdog_start_time" \
                    && [ "$counter" -lt 80 ]; do
                sleep 0.1
                counter=$((counter + 1))
            done
        fi
        if same_process_running "$watchdog_pid" "$watchdog_start_time" \
                || [ -z "$watchdog_start_time" ]; then
            if stop_child "$watchdog_pid" TERM 'safety watchdog'; then
                watchdog_pid=
            else
                watchdog_stopped=0
            fi
        else
            wait "$watchdog_pid" 2>/dev/null || true
            watchdog_pid=
        fi
    fi
    [ "$watchdog_stopped" -eq 1 ]
}

classify_watchdog_client()
{
    classified_token=$1
    classified_owner=$2
    watchdog_client=not-started
    watched_pid=
    watched_start_time=
    client_identity=${classified_token#"$classified_owner":}
    case "$client_identity" in
        *:*)
            watched_pid=${client_identity%%:*}
            watched_start_time=${client_identity#*:}
            watchdog_client=identity-changed
            ;;
    esac
    if same_process_running "$watched_pid" "$watched_start_time"; then
        watchdog_client=pending
    fi
}

stop_watchdog_client()
{
    [ "$watchdog_client" = pending ] || return 0
    watchdog_client=INT
    kill -INT "$watched_pid" 2>/dev/null || true
    counter=0
    while same_process_running "$watched_pid" "$watched_start_time" \
            && [ "$counter" -lt 50 ]; do
        sleep 0.1
        counter=$((counter + 1))
    done
    if same_process_running "$watched_pid" "$watched_start_time"; then
        watchdog_client=KILL
        kill -KILL "$watched_pid" 2>/dev/null || true
        counter=0
        while same_process_running "$watched_pid" "$watched_start_time" \
                && [ "$counter" -lt 20 ]; do
            sleep 0.1
            counter=$((counter + 1))
        done
        if same_process_running "$watched_pid" "$watched_start_time"; then
            watchdog_client=failed
        fi
    fi
}

safety_watchdog()
{
    expected_owner=$1

    printf '%s\n' "$expected_owner" > "$watchdog_armed_tmp" \
        && mv "$watchdog_armed_tmp" "$watchdog_armed" \
        || exit 1
    watchdog_counter=0
    watchdog_ticks=$((duration * 10))
    while [ "$watchdog_counter" -lt "$watchdog_ticks" ]; do
        sleep 0.1
        current_token=$(cat "$lock_token_file" 2>/dev/null || true)
        case "$current_token" in
            "$expected_owner":*) ;;
            *) exit 0 ;;
        esac
        watchdog_counter=$((watchdog_counter + 1))
    done

    watchdog_restore=fail
    classify_watchdog_client "$current_token" "$expected_owner"
    if restore_low_latency_n; then
        watchdog_restore=pass
    fi
    write_watchdog_result "$watchdog_restore" "$watchdog_client"
    : > "$watchdog_fired"

    stop_watchdog_client
    write_watchdog_result "$watchdog_restore" "$watchdog_client"

    while :; do
        current_token=$(cat "$lock_token_file" 2>/dev/null || true)
        case "$current_token" in
            "$expected_owner":*) ;;
            *) exit 0 ;;
        esac
        if ! restore_low_latency_n; then
            watchdog_restore=fail
            write_watchdog_result "$watchdog_restore" "$watchdog_client" \
                || true
        fi
        case "$watchdog_client" in
            not-started|identity-changed)
                classify_watchdog_client "$current_token" "$expected_owner"
                if [ "$watchdog_client" = pending ]; then
                    write_watchdog_result "$watchdog_restore" \
                        "$watchdog_client" || true
                    stop_watchdog_client
                    write_watchdog_result "$watchdog_restore" \
                        "$watchdog_client" || true
                fi
                ;;
        esac
        sleep 0.1
    done
}

capture_drm_snapshot()
{
    snapshot_target=$1
    snapshot_tmp=$snapshot_target.tmp
    rm -f "$snapshot_tmp"

    defer_spawn_signals
    cat "$drm_state_source" > "$snapshot_tmp" &
    state_pid=$!
    finish_deferred_spawn

    counter=0
    while process_running "$state_pid" && [ "$counter" -lt 10 ]; do
        sleep 0.1
        counter=$((counter + 1))
    done
    if process_running "$state_pid"; then
        restore_low_latency_n || true
        printf '%s\n' 'DRM state reader exceeded its one-second bound' >&2
        return 1
    fi

    snapshot_status=0
    defer_spawn_signals
    wait "$state_pid" 2>/dev/null || snapshot_status=$?
    state_pid=
    finish_deferred_spawn
    if [ "$snapshot_status" -ne 0 ]; then
        rm -f "$snapshot_tmp"
        return 1
    fi
    mv "$snapshot_tmp" "$snapshot_target"
}

restore_original()
{
    status=$?
    restore_failed=0
    display_failed=0
    unknown_weston=0
    weston_ready=0
    watchdog_safe=1
    client_stopped=1
    state_stopped=1
    low_latency_restored=1
    trap - EXIT
    trap ':' HUP INT QUIT TERM
    set +e

    if ! restore_low_latency_n; then
        printf '%s\n' 'Failed to restore low_latency=N' >&2
        low_latency_restored=0
        restore_failed=1
    fi

    if ! stop_watchdog; then
        printf '%s\n' 'Failed to stop the safety watchdog' >&2
        watchdog_safe=0
        restore_failed=1
    fi

    if watchdog_reported_client_signal; then
        client_int_sent=1
    fi

    if [ -n "$client_pid" ]; then
        if [ "$watchdog_safe" -eq 1 ]; then
            stop_client_safely || client_stopped=0
        else
            stop_client_safely no-reap || client_stopped=0
        fi
        if [ "$client_stopped" -eq 0 ]; then
            restore_failed=1
        else
            client_pid=
        fi
    fi

    if [ -n "$state_pid" ]; then
        if ! stop_child "$state_pid" TERM 'DRM state reader'; then
            state_stopped=0
            restore_failed=1
        else
            state_pid=
        fi
    fi

    if fuser "$device" >/dev/null 2>&1; then
        printf '%s\n' 'Video device remains busy after client exit' >&2
        client_stopped=0
        restore_failed=1
    fi

    if [ -n "$weston_pid" ]; then
        stop_child "$weston_pid" TERM 'isolated Weston' || display_failed=1
    fi

    rm -f "$runtime_dir/$socket_name" "$runtime_dir/$socket_name.lock"
    current_weston_pid=$(pgrep -x weston 2>/dev/null || true)
    if [ -z "$current_weston_pid" ]; then
        rm -f "$runtime_dir/wayland-0" "$runtime_dir/wayland-0.lock"
        timeout 5 "$original_weston" start || display_failed=1
    elif [ "$current_weston_pid" != "$original_weston_pid" ]; then
        printf '%s\n' 'Refusing to start SystemUI: an unknown Weston remains' >&2
        display_failed=1
        unknown_weston=1
    fi

    if [ "$unknown_weston" -eq 0 ]; then
        counter=0
        while { ! pgrep -x weston >/dev/null 2>&1 \
                || [ ! -S "$runtime_dir/wayland-0" ]; } \
                && [ "$counter" -lt 50 ]; do
            sleep 0.1
            counter=$((counter + 1))
        done
        if ! pgrep -x weston >/dev/null 2>&1 \
                || [ ! -S "$runtime_dir/wayland-0" ]; then
            printf '%s\n' 'Failed to restore original Weston' >&2
            display_failed=1
        else
            weston_ready=1
        fi
    fi

    if [ "$weston_ready" -eq 1 ]; then
        if ! pgrep -x systemui >/dev/null 2>&1 \
                || ! pgrep -x controlcenter >/dev/null 2>&1; then
            timeout 5 "$original_ui" stop >/dev/null 2>&1 || true
            timeout 5 "$original_ui" start || display_failed=1
        fi
        counter=0
        while { ! pgrep -x systemui >/dev/null 2>&1 \
                || ! pgrep -x controlcenter >/dev/null 2>&1; } \
                && [ "$counter" -lt 50 ]; do
            sleep 0.1
            counter=$((counter + 1))
        done
        if ! pgrep -x systemui >/dev/null 2>&1 \
                || ! pgrep -x controlcenter >/dev/null 2>&1; then
            printf '%s\n' 'Failed to restore SystemUI' >&2
            display_failed=1
        fi
    fi

    if [ "$display_failed" -ne 0 ]; then
        restore_failed=1
    fi

    if [ "$lock_owned" -eq 1 ]; then
        if [ "$watchdog_safe" -eq 1 ] && [ "$client_stopped" -eq 1 ] \
                && [ "$state_stopped" -eq 1 ] \
                && [ "$low_latency_restored" -eq 1 ] \
                && [ "$display_failed" -eq 0 ]; then
            rm -f "$lock_token_file" "$lock_token_tmp"
            if rmdir "$lock_dir" 2>/dev/null; then
                lock_owned=0
            else
                printf '%s\n' 'Failed to release the low-latency test lock' >&2
                restore_failed=1
            fi
        else
            printf 'Safety lock retained at %s; reboot before another test\n' \
                "$lock_dir" >&2
        fi
    fi

    if [ "$mode" = baseline ]; then
        if [ "$status" -eq 0 ] && [ "$restore_failed" -eq 0 ] \
                && [ "$validation_passed" -eq 1 ]; then
            if ! printf 'boot_id=%s\nbinary_sha256=%s\ninput_format=%s\ninput_size=%s\ntiming_sha256=%s\n' \
                    "$boot_id" "$binary_hash" "$input_format" \
                    "$input_size" "$timing_hash" \
                    > "$baseline_marker"; then
                rm -f "$baseline_marker"
                restore_failed=1
            fi
        else
            rm -f "$baseline_marker"
        fi
    fi

    if [ "$restore_failed" -ne 0 ]; then
        status=70
    fi
    {
        printf 'mode=%s\n' "$mode"
        printf 'validation=%s\n' "$validation_passed"
        printf 'restore=%s\n' "$([ "$restore_failed" -eq 0 ] && printf pass || printf fail)"
        printf 'exit_status=%s\n' "$status"
    } >> "$result_file" 2>/dev/null || true
    exit "$status"
}

if [ "$(id -u)" -ne 0 ]; then
    printf '%s\n' 'Run this test as root' >&2
    exit 77
fi
for required in "$binary" "$original_ui" "$original_weston" "$weston_bin"; do
    if [ ! -x "$required" ]; then
        printf 'Not executable: %s\n' "$required" >&2
        exit 66
    fi
done
for command in awk date fuser grep pgrep readlink sha256sum timeout v4l2-ctl; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf 'Required command is unavailable: %s\n' "$command" >&2
        exit 66
    fi
done
if [ ! -d "$runtime_dir" ] || [ ! -r "$weston_config" ] \
        || [ ! -w "$parameter" ] || [ ! -r "$drm_name" ] \
        || [ ! -r "$drm_state_source" ]; then
    printf '%s\n' 'Weston, low-latency, or DRM state input is unavailable' >&2
    exit 66
fi
if ! grep -qi rockchip "$drm_name"; then
    printf '%s\n' 'DRM card 0 is not the Rockchip display device' >&2
    exit 1
fi
if [ "$(cat "$parameter")" != N ]; then
    printf '%s\n' 'Refusing to start: low_latency is not N' >&2
    exit 1
fi
if fuser "$device" >/dev/null 2>&1; then
    printf 'Refusing to start: %s is busy\n' "$device" >&2
    exit 1
fi
if ! pgrep -x weston >/dev/null 2>&1 \
        || ! pgrep -x systemui >/dev/null 2>&1 \
        || ! pgrep -x controlcenter >/dev/null 2>&1; then
    printf '%s\n' 'Refusing to start: original Weston/SystemUI is incomplete' >&2
    exit 1
fi
set -- $(pgrep -x weston)
if [ "$#" -ne 1 ]; then
    printf '%s\n' 'Refusing to start: multiple Weston processes found' >&2
    exit 1
fi
original_weston_pid=$1

boot_id=$(cat /proc/sys/kernel/random/boot_id)
binary_hash=$(sha256sum "$binary" | awk '{ print $1 }')
if ! mkdir "$lock_dir" 2>/dev/null; then
    printf 'Refusing to start: safety lock exists at %s\n' "$lock_dir" >&2
    exit 1
fi
lock_owned=1
watchdog_owner="$boot_id:$$:$binary_hash"
watchdog_token="$watchdog_owner:setup"
trap restore_original EXIT
install_signal_handlers
if ! write_watchdog_token "$watchdog_token"; then
    printf '%s\n' 'Failed to initialize the low-latency safety lock' >&2
    exit 1
fi

mkdir -p "$evidence_dir"
: > "$weston_log"
: > "$client_log"
: > "$probe_log"
: > "$preflight_log"
: > "$result_file"
rm -f "$drm_state" "$drm_state_final" "$drm_state_final_tmp" \
    "$trace_summary" "$drm_summary" "$watchdog_fired" \
    "$watchdog_armed" "$watchdog_armed_tmp" "$watchdog_result" \
    "$watchdog_result_tmp" "$client_gate"

format_text=$(v4l2-ctl -d "$device" --get-fmt-video 2>&1)
timing_text=$(v4l2-ctl -d "$device" --query-dv-timings 2>&1)
input_size=$(printf '%s\n' "$format_text" | awk -F: '
    /Width\/Height/ { gsub(/[[:space:]]/, "", $2); gsub("/", "x", $2); print $2; exit }
')
input_format=$(printf '%s\n' "$format_text" | awk -F"'" '/Pixel Format/ { print $2; exit }')
timing_width=$(printf '%s\n' "$timing_text" | awk -F: '/Active width/ { gsub(/[[:space:]]/, "", $2); print $2; exit }')
timing_height=$(printf '%s\n' "$timing_text" | awk -F: '/Active height/ { gsub(/[[:space:]]/, "", $2); print $2; exit }')
input_fps=$(printf '%s\n' "$timing_text" | awk -F'[()]' '
    /frames per second/ { value = $2; sub(/[[:space:]]*frames per second.*/, "", value); print value; exit }
')
case "$input_format" in
    NV16|NV24) ;;
    *)
        printf 'Expected a locked NV16 or NV24 input, got format=%s\n' \
            "$input_format" >&2
        exit 1
        ;;
esac
if [ -z "$input_size" ] \
        || [ "${timing_width}x${timing_height}" != "$input_size" ]; then
    printf 'Input size does not match timing: format=%s size=%s timing=%sx%s\n' \
        "$input_format" "$input_size" "$timing_width" "$timing_height" >&2
    exit 1
fi
if [ -z "$input_fps" ] \
        || ! awk -v fps="$input_fps" 'BEGIN { exit(fps + 0 > 0 ? 0 : 1) }'; then
    printf 'Expected a positive input frame rate, got %s fps\n' "$input_fps" >&2
    exit 1
fi
driver_release_point=$(driver_release_point_for_fps "$input_fps")
timing_hash=$(printf '%s\n' "$timing_text" | sha256sum | awk '{ print $1 }')
{
    printf 'boot_id=%s\n' "$boot_id"
    printf 'binary_sha256=%s\n' "$binary_hash"
    printf 'input_format=%s\ninput_size=%s\n' "$input_format" "$input_size"
    printf 'input_fps=%s\ndriver_release_point=%s\ntiming_sha256=%s\n' \
        "$input_fps" "$driver_release_point" "$timing_hash"
    printf 'low_latency=%s\n' "$(cat "$parameter")"
    printf '%s\n' "$timing_text"
    printf '%s\n' "$format_text"
} > "$preflight_log"

if [ "$mode" = baseline ]; then
    rm -f "$baseline_marker"
else
    if [ ! -r "$baseline_marker" ] \
            || ! grep -qx "boot_id=$boot_id" "$baseline_marker" \
            || ! grep -qx "binary_sha256=$binary_hash" "$baseline_marker" \
            || ! grep -qx "input_format=$input_format" "$baseline_marker" \
            || ! grep -qx "input_size=$input_size" "$baseline_marker" \
            || ! grep -qx "timing_sha256=$timing_hash" "$baseline_marker"; then
        printf '%s\n' 'Run and pass baseline with this binary and input first' >&2
        exit 1
    fi
fi

timeout 5 "$original_ui" stop || true
counter=0
while { pgrep -x systemui >/dev/null 2>&1 \
        || pgrep -x controlcenter >/dev/null 2>&1; } \
        && [ "$counter" -lt 50 ]; do
    sleep 0.1
    counter=$((counter + 1))
done
if pgrep -x systemui >/dev/null 2>&1 \
        || pgrep -x controlcenter >/dev/null 2>&1; then
    printf '%s\n' 'Original SystemUI did not stop' >&2
    exit 1
fi

timeout 5 "$original_weston" stop || true
counter=0
while pgrep -x weston >/dev/null 2>&1 && [ "$counter" -lt 50 ]; do
    sleep 0.1
    counter=$((counter + 1))
done
if pgrep -x weston >/dev/null 2>&1; then
    printf '%s\n' 'Original Weston did not stop' >&2
    exit 1
fi

rm -f "$runtime_dir/wayland-0" "$runtime_dir/wayland-0.lock" \
    "$runtime_dir/$socket_name" "$runtime_dir/$socket_name.lock"
defer_spawn_signals
env -u WESTON_DISABLE_ATOMIC -u WESTON_DRM_MIRROR \
    XDG_RUNTIME_DIR="$runtime_dir" \
    "$weston_bin" --socket="$socket_name" --config="$weston_config" \
    --logger-scopes=log,drm-backend --flight-rec-scopes= \
    >>"$weston_log" 2>&1 &
weston_pid=$!
finish_deferred_spawn

counter=0
while [ "$counter" -lt 50 ]; do
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        printf 'Isolated Weston exited; see %s\n' "$weston_log" >&2
        exit 1
    fi
    if [ -S "$runtime_dir/$socket_name" ] \
            && grep -q 'DRM: supports atomic modesetting' "$weston_log"; then
        break
    fi
    sleep 0.1
    counter=$((counter + 1))
done
if [ ! -S "$runtime_dir/$socket_name" ] \
        || ! grep -q 'DRM: supports atomic modesetting' "$weston_log" \
        || grep -q 'Entering mirror mode' "$weston_log"; then
    printf 'Isolated atomic Weston validation failed; see %s\n' "$weston_log" >&2
    exit 1
fi
if tr '\000' '\n' < "/proc/$weston_pid/environ" \
        | grep -Eq '^WESTON_(DISABLE_ATOMIC|DRM_MIRROR)='; then
    printf '%s\n' 'Isolated Weston inherited a forbidden environment variable' >&2
    exit 1
fi
if [ "$(cat "$parameter")" != N ]; then
    printf '%s\n' 'low_latency changed before capability probe' >&2
    exit 1
fi

if [ "$mode" = explicit ]; then
    if ! run_explicit_capability_probe; then
        printf 'Explicit-sync probe failed; see %s\n' "$probe_log" >&2
        exit 1
    fi
fi

test_deadline=$(($(date +%s) + duration))
watchdog_token="$watchdog_owner:pending"
if ! write_watchdog_token "$watchdog_token"; then
    printf '%s\n' 'Failed to prepare the low-latency safety watchdog' >&2
    exit 1
fi
defer_spawn_signals
(
    trap - HUP INT QUIT TERM
    safety_watchdog "$watchdog_owner"
) &
watchdog_pid=$!
watchdog_spawn_failed=0
if ! watchdog_start_time=$(process_start_time "$watchdog_pid" 2>/dev/null); then
    printf '%s\n' 'Failed to record the watchdog process identity' >&2
    watchdog_spawn_failed=1
fi
finish_deferred_spawn
if [ "$watchdog_spawn_failed" -ne 0 ]; then
    exit 1
fi
counter=0
while [ "$counter" -lt 20 ]; do
    if [ -r "$watchdog_armed" ] \
            && [ "$(cat "$watchdog_armed" 2>/dev/null)" = "$watchdog_owner" ]; then
        break
    fi
    if ! same_process_running "$watchdog_pid" "$watchdog_start_time"; then
        break
    fi
    sleep 0.1
    counter=$((counter + 1))
done
if [ ! -r "$watchdog_armed" ] \
        || [ "$(cat "$watchdog_armed" 2>/dev/null)" != "$watchdog_owner" ] \
        || ! same_process_running "$watchdog_pid" "$watchdog_start_time"; then
    printf '%s\n' 'Safety watchdog failed to arm' >&2
    exit 1
fi

defer_spawn_signals
(
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 131' QUIT
    trap 'exit 143' TERM
    if ! wait_for_client_gate "$client_gate"; then
        printf '%s\n' 'Client start gate timed out before token publication' >&2
        exit 75
    fi
    if [ "$mode" = explicit ]; then
        exec env XDG_RUNTIME_DIR="$runtime_dir" WAYLAND_DISPLAY="$socket_name" \
            "$binary" -g -v "$device" -f "$input_format" \
            -d "$input_format" -s -t
    fi
    exec env XDG_RUNTIME_DIR="$runtime_dir" WAYLAND_DISPLAY="$socket_name" \
        "$binary" -g -v "$device" -f "$input_format" \
        -d "$input_format" -s -q -t
) > "$client_log" 2>&1 &
client_pid=$!
spawn_failed=0
if ! client_start_time=$(process_start_time "$client_pid" 2>/dev/null); then
    printf '%s\n' 'Failed to record the client process identity' >&2
    spawn_failed=1
else
    watchdog_token="$watchdog_owner:$client_pid:$client_start_time"
    if ! write_watchdog_token "$watchdog_token"; then
        printf '%s\n' 'Failed to arm the low-latency safety lock' >&2
        spawn_failed=1
    fi
fi
finish_deferred_spawn
if [ "$spawn_failed" -ne 0 ]; then
    exit 1
fi

if [ "$mode" = explicit ]; then
    if ! enable_low_latency_y_safely; then
        printf '%s\n' 'Safety watchdog cannot guard low_latency=Y' >&2
        exit 1
    fi
else
    if ! watchdog_guards_client; then
        printf '%s\n' 'Safety watchdog expired before the baseline client start' >&2
        exit 1
    fi
fi
if ! watchdog_guards_client; then
    restore_low_latency_n || true
    printf '%s\n' 'Safety watchdog expired before opening the client start gate' >&2
    exit 1
fi
if ! : > "$client_gate"; then
    restore_low_latency_n || true
    printf '%s\n' 'Failed to open the client start gate' >&2
    exit 1
fi
if [ "$mode" = explicit ]; then
    printf 'Running %s explicit-sync direct-display test (%s)\n' \
        "$input_format" "$driver_release_point"
else
    printf 'Running %s implicit-sync direct-display baseline (%s)\n' \
        "$input_format" "$driver_release_point"
fi

plane_seen=0
plane_lost=0
plane_matches=0
client_early_exit=0
while [ "$(date +%s)" -lt "$test_deadline" ]; do
    if ! client_process_running; then
        if [ ! -e "$watchdog_fired" ]; then
            client_early_exit=1
        fi
        break
    fi
    if [ ! -e "$watchdog_fired" ] \
            && ! same_process_running "$watchdog_pid" \
                "$watchdog_start_time"; then
        restore_low_latency_n || true
        printf '%s\n' 'Safety watchdog exited before the test deadline' >&2
        exit 1
    fi
    if ! capture_drm_snapshot "$drm_state_final"; then
        exit 1
    fi
    if plane_info "$drm_state_final" >/dev/null 2>&1; then
        plane_matches=$((plane_matches + 1))
        if [ "$plane_seen" -eq 0 ]; then
            cp "$drm_state_final" "$drm_state"
            plane_seen=1
        fi
    elif [ "$plane_seen" -eq 1 ]; then
        plane_lost=1
    fi
    sleep 0.1
done

if client_process_running; then
    if ! capture_drm_snapshot "$drm_state_final"; then
        exit 1
    fi
fi

main_restore_failed=0
if ! restore_low_latency_n; then
    main_restore_failed=1
fi

watchdog_stop_failed=0
defer_spawn_signals
if ! stop_watchdog; then
    watchdog_stop_failed=1
fi
finish_deferred_spawn
if [ "$watchdog_stop_failed" -ne 0 ]; then
    printf '%s\n' 'Safety watchdog did not stop within its bound' >&2
    exit 1
fi

watchdog_was_fired=0
watchdog_report_failed=0
if [ -e "$watchdog_fired" ]; then
    watchdog_was_fired=1
    if ! watchdog_result_is_safe; then
        watchdog_report_failed=1
    fi
fi
if watchdog_reported_client_signal; then
    client_int_sent=1
fi

client_stop_failed=0
defer_spawn_signals
if stop_client_safely; then
    client_status=$last_wait_status
    client_pid=
else
    client_stop_failed=1
fi
finish_deferred_spawn
if [ "$client_stop_failed" -ne 0 ]; then
    printf '%s\n' 'Client did not stop within the bounded grace period' >&2
    exit 1
fi

if [ "$main_restore_failed" -ne 0 ]; then
    printf '%s\n' 'Failed to disable low_latency at the test deadline' >&2
    exit 1
fi
if [ "$client_early_exit" -ne 0 ]; then
    printf '%s\n' 'Client exited before the test duration elapsed' >&2
    exit 1
fi
if [ "$watchdog_was_fired" -eq 1 ]; then
    if [ "$watchdog_report_failed" -ne 0 ]; then
        printf 'Safety watchdog reported a failure; see %s\n' \
            "$watchdog_result" >&2
        exit 1
    fi
    case "$client_status" in
        0) ;;
        *)
            printf 'Client exited with status %s after watchdog action; see %s\n' \
                "$client_status" "$client_log" >&2
            exit 1
            ;;
    esac
elif [ "$client_status" -ne 0 ]; then
    printf 'Client exited with status %s; see %s\n' "$client_status" "$client_log" >&2
    exit 1
fi
if [ "$plane_seen" -ne 1 ] || [ "$plane_matches" -lt 30 ] \
        || [ "$plane_lost" -ne 0 ] || [ ! -s "$drm_state" ]; then
    printf '%s never reached a linear Esmart plane; see %s\n' \
        "$input_format" "$drm_state" >&2
    exit 1
fi
if [ ! -s "$drm_state_final" ] \
        || ! plane_info "$drm_state_final" >/dev/null 2>&1; then
    printf '%s was not on a linear Esmart plane at test end; see %s\n' \
        "$input_format" "$drm_state_final" >&2
    exit 1
fi
if grep -Eq 'HDMI RX did not return a valid sync_file|Invalid Wayland release fence|VIDIOC_(DQBUF|QBUF|S_FMT)|Error: zwp_linux_buffer_params.create failed|No zwp_linux_explicit_synchronization_v1 global' "$client_log"; then
    printf 'Client reported a capture or synchronization error; see %s\n' "$client_log" >&2
    exit 1
fi
if ! validate_trace > "$trace_summary"; then
    printf 'Synchronization trace validation failed; see %s\n' "$trace_summary" >&2
    exit 1
fi

set -- $(plane_info "$drm_state")
plane_id=$1
fb_id=$2
set -- $(plane_info "$drm_state_final")
final_plane_id=$1
if [ "$final_plane_id" != "$plane_id" ]; then
    printf '%s moved from plane %s to %s during the test\n' \
        "$input_format" "$plane_id" "$final_plane_id" >&2
    exit 1
fi
if ! validate_weston_drm > "$drm_summary"; then
    printf 'Weston DRM evidence validation failed; see %s\n' "$weston_log" >&2
    exit 1
fi

{
    printf 'mode=%s\n' "$mode"
    printf 'input=%s_%s_%sfps\n' "$input_size" "$input_format" "$input_fps"
    printf 'driver_release_point=%s\n' "$driver_release_point"
    printf 'plane_id=%s\nfb_id=%s\n' "$plane_id" "$fb_id"
    cat "$trace_summary"
    cat "$drm_summary"
    printf '%s\n' 'evidence=pass'
} >> "$result_file"
validation_passed=1
printf 'PASS mode=%s input=%s/%s@%sfps path=%s plane=%s evidence=%s\n' \
    "$mode" "$input_size" "$input_format" "$input_fps" \
    "$driver_release_point" "$plane_id" "$evidence_dir"
exit 0
