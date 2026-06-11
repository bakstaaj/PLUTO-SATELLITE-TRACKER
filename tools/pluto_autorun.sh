#!/bin/sh

DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}"
RUNNER="${DEPLOY_DIR}/run_tracker.sh"
PID_FILE="/var/run/pluto_sat_tracker.pid"
LOG_DIR="${SD_ROOT}/logs"
LOG_FILE="${LOG_DIR}/pluto_sat_tracker.log"

find_pids() {
    PIDS="$(pidof pluto_sat_tracker 2>/dev/null || true)"
    if [ -z "$PIDS" ]; then
        PIDS="$(ps | awk '/pluto_sat_tracker/ && !/awk/ { print $1 }')"
    fi
    echo "$PIDS"
}

wait_for_sd() {
    COUNT=0
    while [ "$COUNT" -lt 30 ]; do
        if [ -d "$(dirname "$SD_ROOT")" ]; then
            mkdir -p "$SD_ROOT/data" "$SD_ROOT/cache" "$LOG_DIR"
            return 0
        fi
        COUNT=$((COUNT + 1))
        sleep 1
    done
    return 1
}

if [ -n "$(find_pids)" ]; then
    exit 0
fi

if [ ! -x "$RUNNER" ]; then
    exit 0
fi

if ! wait_for_sd; then
    exit 0
fi

nohup "$RUNNER" -- --net >"$LOG_FILE" 2>&1 < /dev/null &
echo $! > "$PID_FILE"
