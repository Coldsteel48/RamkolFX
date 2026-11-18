#!/usr/bin/env bash
# End-to-end tests: runs tests/e2e/e2e_client.cpp's scenarios against a
# real ramkolfxd on the live PipeWire session. Not part of ctest, since it
# needs PipeWire, plays (silent) test streams, and starts the daemon.
#
# Each scenario gets a fresh daemon with its own temporary config
# directory, so your real settings and presets are never touched. The
# daemon makes its virtual sink the default output while running and puts
# the previous default back on exit; this script restores it too, just in
# case.
#
# Usage:
#   tests/e2e/run.sh                      # pick two real output devices automatically
#   tests/e2e/run.sh DEVICE_A [DEVICE_B]  # node.names, as in `pactl list short sinks`
#
# Scenarios that need two devices are skipped when only one exists.
# Needs: pw-play, pactl. Takes about a minute.
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")/../.."
BUILD_DIR=build

if pgrep -x ramkolfxd >/dev/null; then
    echo "ramkolfxd is already running - stop it first (only one instance can run)." >&2
    exit 1
fi

# Tests forced on: ramkolfx_e2e only exists when they are, and an existing
# build/ may have them cached off. Output kept quiet on success, shown on
# failure.
if ! BUILD_LOG=$( { cmake -S . -B "$BUILD_DIR" -G Ninja -DRAMKOLFX_BUILD_TESTS=ON &&
                    cmake --build "$BUILD_DIR" --target ramkolfxd ramkolfx_e2e; } 2>&1 ); then
    echo "$BUILD_LOG" >&2
    echo "build failed" >&2
    exit 1
fi
DAEMON="$BUILD_DIR/daemon/ramkolfxd"
CLIENT="$BUILD_DIR/tests/ramkolfx_e2e"

mapfile -t SINKS < <(pactl list short sinks | awk '{print $2}' | grep -v '^ramkolfx' || true)
DEVICE_A="${1:-${SINKS[0]:-}}"
DEVICE_B="${2:-${SINKS[1]:-}}"
if [[ -z "$DEVICE_A" ]]; then
    echo "no output device found" >&2
    exit 1
fi

WORK=$(mktemp -d)
ORIGINAL_DEFAULT=$(pactl get-default-sink)
DAEMON_PID=
cleanup() {
    [[ -n "$DAEMON_PID" ]] && kill -INT "$DAEMON_PID" 2>/dev/null && wait "$DAEMON_PID" 2>/dev/null || true
    pactl set-default-sink "$ORIGINAL_DEFAULT" 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

export RAMKOLFX_E2E_WAV="$WORK/silence.wav"
"$CLIENT" make-wav "$RAMKOLFX_E2E_WAV"

# start_daemon NAME [settings lines...]: fresh config dir unless KEEP_CONFIG=1.
start_daemon() {
    local Name=$1
    shift
    CONFIG="$WORK/config"
    if [[ "${KEEP_CONFIG:-0}" != 1 ]]; then
        rm -rf "$CONFIG"
        mkdir -p "$CONFIG/ramkolfx"
        printf '%s\n' "output_device=$DEVICE_A" "$@" > "$CONFIG/ramkolfx/settings.conf"
    fi
    XDG_CONFIG_HOME="$CONFIG" "$DAEMON" > "$WORK/$Name.log" 2>&1 &
    DAEMON_PID=$!
    # Up once the control socket answers; give PipeWire a moment to settle.
    for _ in $(seq 50); do
        "$CLIENT" ping 2>/dev/null && break
        sleep 0.1
    done
    sleep 1
}

stop_daemon() {
    kill -INT "$DAEMON_PID"
    wait "$DAEMON_PID" || true
    DAEMON_PID=
}

FAILED=0
run() {
    local Name=$1
    shift
    echo "=== $Name"
    if "$CLIENT" "$@"; then
        echo "--- $Name: OK"
    else
        echo "--- $Name: FAILED (daemon log: kept below)"
        tail -20 "$WORK/$Name.log" || true
        FAILED=1
    fi
}

gain_in_settings() {
    grep '^hweq.5.gain=' "$CONFIG/ramkolfx/settings.conf" | cut -d= -f2
}

start_daemon presets; run presets presets; stop_daemon
start_daemon bound-preset; run bound-preset bound-preset; stop_daemon

echo "=== save batching"
start_daemon batching
"$CLIENT" burst 3.5
sleep 0.5 # two timer ticks
if [[ "$(gain_in_settings)" == 3.5 ]]; then echo "  saved within a tick: OK"; else echo "  not saved after 500ms: FAILED"; FAILED=1; fi
"$CLIENT" burst -2.25
stop_daemon # immediately: the shutdown flush must catch it
if [[ "$(gain_in_settings)" == -2.25 ]]; then echo "  flushed on shutdown: OK"; else echo "  lost on shutdown: FAILED"; FAILED=1; fi

if [[ -n "$DEVICE_B" ]]; then
    start_daemon unbind; run unbind unbind "$DEVICE_A" "$DEVICE_B"; stop_daemon
    start_daemon spatial mode=0; run spatial spatial "$DEVICE_A" "$DEVICE_B"; stop_daemon
    KEEP_CONFIG=1 start_daemon spatial-restart
    run spatial-restart spatial-restart "$DEVICE_A" "$DEVICE_B"
    stop_daemon
else
    echo "=== unbind, spatial: skipped (needs two output devices)"
fi

if [[ $FAILED == 0 ]]; then echo "ALL E2E OK"; else echo "E2E FAILURES"; fi
exit $FAILED
