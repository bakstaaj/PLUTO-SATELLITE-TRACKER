#!/bin/sh

DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}"
REFRESH_RUNNER="${SD_ROOT}/tools/pluto_refresh_data.sh"
OBSERVER_FILE="${DEPLOY_DIR}/config/observer.json"
CATALOG_FILE="${SD_ROOT}/data/satellites.json"
PASSES_FILE="${SD_ROOT}/data/passes.json"
LOG_DIR="${SD_ROOT}/logs"
PID_FILE="${LOG_DIR}/pass_refresh_loop.pid"
LOOP_LOG="${LOG_DIR}/pass_refresh_loop.log"
REFRESH_INTERVAL_SECONDS="${PLUTO_PASS_REFRESH_INTERVAL_SECONDS:-300}"
STALE_MINUTES="${PLUTO_PASS_REFRESH_STALE_MINUTES:-15}"

mkdir -p "$LOG_DIR"

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

passes_stale() {
  if [ ! -f "$PASSES_FILE" ]; then
    return 0
  fi
  find "$PASSES_FILE" -mmin +"$STALE_MINUTES" | grep -q .
}

can_refresh() {
  [ -x "$REFRESH_RUNNER" ] || [ -r "$REFRESH_RUNNER" ] || return 1
  [ -f "$OBSERVER_FILE" ] || return 1
  [ -f "$CATALOG_FILE" ] || return 1
  return 0
}

maybe_refresh() {
  if ! can_refresh; then
    log "Skipping pass refresh: observer, catalog, or runner is unavailable"
    return 0
  fi
  if ! passes_stale; then
    return 0
  fi

  log "Refreshing passes in background"
  if /bin/sh "$REFRESH_RUNNER" passes >> "$LOOP_LOG" 2>&1; then
    log "Pass refresh complete"
  else
    log "Pass refresh failed"
  fi
}

log "Pass refresh loop started (interval=${REFRESH_INTERVAL_SECONDS}s stale=${STALE_MINUTES}m)"

while true; do
  maybe_refresh
  sleep "$REFRESH_INTERVAL_SECONDS"
done
