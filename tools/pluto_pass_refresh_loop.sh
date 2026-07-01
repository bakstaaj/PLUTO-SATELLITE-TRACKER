#!/bin/sh
# PASS_REFRESH_LOOP_V2
# Simple 15-minute pass refresh cycle.
#
# On boot, the runtime calls passes_boot which does a quick 2-pass preview
# followed by a full 10-pass generation. This loop waits a short settle period
# then runs passes_worker every 15 minutes thereafter.

DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}"
TMP_ROOT="${PLUTO_SAT_TRANSIENT_DIR:-/tmp/pluto_sat_tracker}"
REFRESH_RUNNER="${SD_ROOT}/tools/pluto_refresh_data.sh"
OBSERVER_FILE="${DEPLOY_DIR}/config/observer.json"
CATALOG_FILE="${DEPLOY_DIR}/data/satellites.json"
LOG_DIR="${TMP_ROOT}/logs"
PID_FILE="${LOG_DIR}/pass_refresh_loop.pid"
LOOP_LOG="${LOG_DIR}/pass_refresh_loop.log"

# Time to wait at boot before the first cycle run, giving the boot worker
# (passes_boot) time to complete its initial generation.
BOOT_SETTLE_SECONDS="${PLUTO_PASS_REFRESH_BOOT_SETTLE_SECONDS:-120}"
# Time between refresh cycles.
CYCLE_INTERVAL_SECONDS="${PLUTO_PASS_REFRESH_INTERVAL_SECONDS:-900}"

mkdir -p "$LOG_DIR"

# Prevent duplicate loop instances
if [ -f "$PID_FILE" ]; then
  OLD_PID="$(cat "$PID_FILE" 2>/dev/null || true)"
  if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
    exit 0
  fi
fi

echo $$ > "$PID_FILE"
trap 'rm -f "$PID_FILE"' EXIT INT TERM

log() {
  echo "[$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo unknown)] $*" >> "$LOOP_LOG"
}

can_refresh() {
  [ -x "$REFRESH_RUNNER" ] || [ -r "$REFRESH_RUNNER" ] || return 1
  [ -f "$OBSERVER_FILE" ] || return 1
  [ -f "$CATALOG_FILE" ] || return 1
  return 0
}

log "Pass refresh loop started (settle=${BOOT_SETTLE_SECONDS}s cycle=${CYCLE_INTERVAL_SECONDS}s)"

# Wait for the boot worker to get a head start
sleep "$BOOT_SETTLE_SECONDS"

while true; do
  if ! can_refresh; then
    log "Skipping pass refresh: observer, catalog, or runner is unavailable"
    sleep 60
    continue
  fi

  log "Running 15-min pass refresh"
  if /bin/sh "$REFRESH_RUNNER" passes_worker >> "$LOOP_LOG" 2>&1; then
    log "Pass refresh complete"
  else
    log "Pass refresh failed"
  fi

  sleep "$CYCLE_INTERVAL_SECONDS"
done
