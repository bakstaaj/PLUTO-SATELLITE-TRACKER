#!/bin/sh
#
# Pluto Satellite Tracker autorun — installed to /mnt/jffs2/autorun.sh
#
# Storage layout:
#   /mnt/jffs2/pluto_sat_tracker/  — binaries, web, config, catalog (persistent)
#   /tmp/pluto_sat_tracker/         — passes, track state, logs (ephemeral, resets on boot)
#   /media/mmcblk0p1/...            — tools, python runtime (read-only SD card OK)

DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}"
TMP_ROOT="/tmp/pluto_sat_tracker"
RUNNER="${DEPLOY_DIR}/run_tracker.sh"
PID_FILE="/var/run/pluto_sat_tracker.pid"
LOG_FILE="${TMP_ROOT}/pluto_sat_tracker.log"

# Create transient dirs — always writable since /tmp is a tmpfs
mkdir -p "${TMP_ROOT}/logs"

find_pids() {
    PIDS="$(pidof pluto_sat_tracker 2>/dev/null || true)"
    if [ -z "$PIDS" ]; then
        PIDS="$(ps | awk '/pluto_sat_tracker/ && !/awk/ { print $1 }')"
    fi
    echo "$PIDS"
}

maybe_wait_for_sd() {
    # SD is optional (read-only OK). Wait up to 10s for mount parent to appear
    # so tools/python are available for pass refresh, but do not block startup.
    COUNT=0
    while [ "$COUNT" -lt 10 ]; do
        if [ -d "$(dirname "$SD_ROOT")" ]; then
            return 0
        fi
        COUNT=$((COUNT + 1))
        sleep 1
    done
    echo "autorun: SD card mount parent not found after 10s — continuing without SD" >> "$LOG_FILE"
    return 0
}

if [ -n "$(find_pids)" ]; then
    exit 0
fi

if [ ! -x "$RUNNER" ]; then
    echo "autorun: runner not found or not executable: $RUNNER" >> "$LOG_FILE"
    exit 0
fi

maybe_wait_for_sd

nohup "$RUNNER" -- --net >"$LOG_FILE" 2>&1 < /dev/null &
echo $! > "$PID_FILE"
