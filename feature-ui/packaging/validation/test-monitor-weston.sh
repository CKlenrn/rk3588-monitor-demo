#!/bin/sh

set -eu

script_dir="$(CDPATH= cd "$(dirname "$0")" && pwd)"
source_script="$script_dir/../board/etc/monitor_demo/staged-init/S49monitor_weston"
test_root="$(mktemp -d "${TMPDIR:-/tmp}/monitor-weston-test.XXXXXX")"
trap 'rm -rf "$test_root"' EXIT INT TERM

fail()
{
    echo "FAIL: $*" >&2
    exit 1
}

mkdir -p "$test_root/bin" "$test_root/run"
cat >"$test_root/bin/pgrep" <<'EOF'
#!/bin/sh
[ "${TEST_PGREP_STATUS:-1}" -eq 0 ]
EOF
cat >"$test_root/fake-weston" <<'EOF'
#!/bin/sh
env >"$TEST_ENV_LOG"
exit 1
EOF
chmod 0755 "$test_root/bin/pgrep" "$test_root/fake-weston"
touch "$test_root/profile" "$test_root/weston.ini"

sed \
    -e "s|^PATH=.*|PATH=$test_root/bin:/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin|" \
    -e "s|^PID_FILE=.*|PID_FILE=$test_root/run/monitor-demo-weston.pid|" \
    -e "s|^LOG_FILE=.*|LOG_FILE=$test_root/weston.log|" \
    -e "s|^CONFIG_FILE=.*|CONFIG_FILE=$test_root/weston.ini|" \
    -e "s|^WESTON_BIN=.*|WESTON_BIN=$test_root/fake-weston|" \
    -e "s|^\. /etc/profile$|. $test_root/profile|" \
    "$source_script" >"$test_root/S49monitor_weston"
chmod 0755 "$test_root/S49monitor_weston"

export XDG_RUNTIME_DIR="$test_root/run"
export TEST_ENV_LOG="$test_root/weston.env"

printf '999999\n' >"$test_root/run/monitor-demo-weston.pid"
touch "$test_root/run/wayland-0" "$test_root/run/wayland-0.lock"
TEST_PGREP_STATUS=1
export TEST_PGREP_STATUS
"$test_root/S49monitor_weston" stop
[ ! -e "$test_root/run/monitor-demo-weston.pid" ] || fail "stale PID was not removed"
[ ! -e "$test_root/run/wayland-0" ] || fail "stale socket was not removed"
[ ! -e "$test_root/run/wayland-0.lock" ] || fail "stale lock was not removed"

touch "$test_root/run/wayland-0" "$test_root/run/wayland-0.lock"
WESTON_DISABLE_ATOMIC=1
WESTON_DRM_MIRROR=1
export WESTON_DISABLE_ATOMIC WESTON_DRM_MIRROR
if "$test_root/S49monitor_weston" start; then
    fail "failing fake Weston unexpectedly started"
fi
[ -s "$TEST_ENV_LOG" ] || fail "fake Weston did not record its environment"
grep -q '^WESTON_DISABLE_ATOMIC=' "$TEST_ENV_LOG" \
    && fail "WESTON_DISABLE_ATOMIC reached Weston"
grep -q '^WESTON_DRM_MIRROR=' "$TEST_ENV_LOG" \
    && fail "WESTON_DRM_MIRROR reached Weston"
grep -q '^WESTON_ALLOW_GBM_MODIFIERS=1$' "$TEST_ENV_LOG" \
    || fail "WESTON_ALLOW_GBM_MODIFIERS=1 did not reach Weston"
[ ! -e "$test_root/run/monitor-demo-weston.pid" ] || fail "failed start left a PID file"
[ ! -e "$test_root/run/wayland-0" ] || fail "failed start left a stale socket"
[ ! -e "$test_root/run/wayland-0.lock" ] || fail "failed start left a stale lock"

printf '999999\n' >"$test_root/run/monitor-demo-weston.pid"
touch "$test_root/run/wayland-0" "$test_root/run/wayland-0.lock"
TEST_PGREP_STATUS=0
export TEST_PGREP_STATUS
if "$test_root/S49monitor_weston" start; then
    fail "start succeeded while another Weston was reported"
fi
[ -e "$test_root/run/monitor-demo-weston.pid" ] || fail "live-Weston guard removed PID file"
[ -e "$test_root/run/wayland-0" ] || fail "live-Weston guard removed socket"
[ -e "$test_root/run/wayland-0.lock" ] || fail "live-Weston guard removed lock"

echo "PASS: monitor Weston environment and stale runtime cleanup"
