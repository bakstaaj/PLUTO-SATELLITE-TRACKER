#!/bin/sh
# ASYNC_PASS_REFRESH_RUNNER_V4
# PASS_REFRESH_1HR_NO_FULL_V1
# Refresh Pluto satellite catalog/pass data.
#
# MODE=passes is intentionally non-blocking for browser/UI use:
#   1. Return quickly after queueing a background quick-preview worker.
#   2. The worker generates a small current preview and publishes it atomically.
#   3. The worker stops after a 1-hour pass scan; full rebuild is disabled for audio stability.
#
# The browser UI is the normal time source on an untethered Pluto. It should call
# /api/time/sync before /api/refresh/passes. This script derives start UTC from
# the Pluto system clock after that browser sync and uses PID+age locks so stale
# boot-time jobs cannot preserve old pass windows.

sanitize_path() {
  P="$(printf "%s" "$1" | sed 's#\\#/#g')"
  case "$P" in
    C:/msys64/*|c:/msys64/*)
      P="/${P#?:/msys64/}"
      ;;
    /c/msys64/*|/C/msys64/*)
      P="/${P#/c/msys64/}"
      P="/${P#/C/msys64/}"
      ;;
  esac
  printf "%s" "$P"
}

DEPLOY_DIR="$(sanitize_path "${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}")"
SD_ROOT="$(sanitize_path "${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}")"
DATA_DIR="${SD_ROOT}/data"
TOOLS_DIR="${SD_ROOT}/tools"
MODE="${1:-passes}"
STATUS_FILE="${DATA_DIR}/refresh_status.json"
STATUS_UPDATER="${TOOLS_DIR}/write_refresh_status.py"
LOG_DIR="${SD_ROOT}/logs"

QUICK_HOURS="${PLUTO_PASS_QUICK_HOURS:-1}"
QUICK_LIMIT="${PLUTO_PASS_QUICK_LIMIT:-20}"
QUICK_STEP_SECONDS="${PLUTO_PASS_QUICK_STEP_SECONDS:-120}"
FULL_HOURS="${PLUTO_PASS_FULL_HOURS:-1}"
FULL_LIMIT="${PLUTO_PASS_FULL_LIMIT:-20}"
FULL_STEP_SECONDS="${PLUTO_PASS_STEP_SECONDS:-120}"
QUICK_LOCK_MAX_AGE="${PLUTO_PASS_QUICK_LOCK_MAX_AGE_SECONDS:-180}"
FULL_LOCK_MAX_AGE="${PLUTO_PASS_FULL_LOCK_MAX_AGE_SECONDS:-240}"
CATALOG_LOCK_MAX_AGE="${PLUTO_CATALOG_LOCK_MAX_AGE_SECONDS:-900}"
REFRESH_START_UTC="${PLUTO_REFRESH_START_UTC:-$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo '')}"

now_epoch() {
  date -u '+%s' 2>/dev/null || echo 0
}

json_clean() {
  printf "%s" "$1" | tr '\r\n"' '   ' | cut -c 1-240
}

write_status() {
  STATE="$1"
  TARGET="$2"
  MESSAGE="$(json_clean "$3")"
  NOW="$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo '')"

  mkdir -p "$DATA_DIR"
  cat >"${STATUS_FILE}.tmp" <<EOF
{
  "ok": true,
  "state": "${STATE}",
  "target": "${TARGET}",
  "updated_utc": "${NOW}",
  "message": "${MESSAGE}"
}
EOF
  mv "${STATUS_FILE}.tmp" "$STATUS_FILE"
}

fail() {
  write_status "error" "$MODE" "$1"
  echo "$1" >&2
  exit 1
}

lock_age_seconds() {
  LOCK_DIR="$1"
  NOW="$(now_epoch)"
  STARTED=""
  if [ -f "${LOCK_DIR}/started_epoch" ]; then
    STARTED="$(cat "${LOCK_DIR}/started_epoch" 2>/dev/null || true)"
  fi
  case "$STARTED" in
    ''|*[!0-9]*)
      if command -v stat >/dev/null 2>&1; then
        STARTED="$(stat -c %Y "$LOCK_DIR" 2>/dev/null || echo "$NOW")"
      else
        STARTED="$NOW"
      fi
      ;;
  esac
  if [ "$NOW" -ge "$STARTED" ] 2>/dev/null; then
    echo $((NOW - STARTED))
  else
    echo 0
  fi
}

acquire_lock() {
  LOCK_DIR="$1"
  TARGET="$2"
  BUSY_MESSAGE="$3"
  MAX_AGE="$4"

  if mkdir "$LOCK_DIR" 2>/dev/null; then
    echo "$$" >"${LOCK_DIR}/pid" 2>/dev/null || true
    now_epoch >"${LOCK_DIR}/started_epoch" 2>/dev/null || true
    trap 'rmdir "$LOCK_DIR" >/dev/null 2>&1 || rm -rf "$LOCK_DIR" >/dev/null 2>&1 || true' EXIT INT TERM
    return 0
  fi

  OLD_PID=""
  if [ -f "${LOCK_DIR}/pid" ]; then
    OLD_PID="$(cat "${LOCK_DIR}/pid" 2>/dev/null || true)"
  fi
  AGE="$(lock_age_seconds "$LOCK_DIR")"

  if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
    if [ "$AGE" -le "$MAX_AGE" ] 2>/dev/null; then
      write_status "running" "$TARGET" "$BUSY_MESSAGE"
      exit 0
    fi
    kill "$OLD_PID" >/dev/null 2>&1 || true
    sleep 1
    kill -9 "$OLD_PID" >/dev/null 2>&1 || true
  fi

  rm -rf "$LOCK_DIR" >/dev/null 2>&1 || true
  if mkdir "$LOCK_DIR" 2>/dev/null; then
    echo "$$" >"${LOCK_DIR}/pid" 2>/dev/null || true
    now_epoch >"${LOCK_DIR}/started_epoch" 2>/dev/null || true
    trap 'rmdir "$LOCK_DIR" >/dev/null 2>&1 || rm -rf "$LOCK_DIR" >/dev/null 2>&1 || true' EXIT INT TERM
    return 0
  fi

  write_status "running" "$TARGET" "$BUSY_MESSAGE"
  exit 0
}

clear_pass_locks() {
  # Browser-time pass refresh is authoritative. Clear old pass refresh locks/jobs
  # before queueing a fresh worker so stale boot-time windows cannot survive.
  for LOCK_DIR in /tmp/pluto_refresh_passes_quick.lock /tmp/pluto_refresh_passes_full.lock /tmp/pluto_refresh_passes_worker.lock /tmp/pluto_refresh_passes.lock; do
    OLD_PID=""
    if [ -f "${LOCK_DIR}/pid" ]; then
      OLD_PID="$(cat "${LOCK_DIR}/pid" 2>/dev/null || true)"
    fi
    if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
      kill "$OLD_PID" >/dev/null 2>&1 || true
      sleep 1
      kill -9 "$OLD_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$LOCK_DIR" >/dev/null 2>&1 || true
  done
}

pass_cache_age_seconds() {
  PASS_FILE="${DATA_DIR}/passes.json"
  NOW="$(now_epoch)"
  if [ ! -s "$PASS_FILE" ]; then
    echo 999999
    return 0
  fi
  if command -v stat >/dev/null 2>&1; then
    MTIME="$(stat -c %Y "$PASS_FILE" 2>/dev/null || echo 0)"
  else
    MTIME=0
  fi
  case "$MTIME" in
    ''|*[!0-9]*) MTIME=0 ;;
  esac
  if [ "$NOW" -ge "$MTIME" ] 2>/dev/null; then
    echo $((NOW - MTIME))
  else
    echo 999999
  fi
}

skip_recent_pass_refresh_if_fresh() {
  REFRESH_INTERVAL="${PLUTO_PASS_REFRESH_INTERVAL_SECONDS:-1800}"
  AGE="$(pass_cache_age_seconds)"
  if [ "$AGE" -lt "$REFRESH_INTERVAL" ] 2>/dev/null; then
    write_status "idle" "passes" "Using recent 1-hour pass cache; next short scan allowed in $((REFRESH_INTERVAL - AGE)) seconds"
    exit 0
  fi
}


run_python() {
  # FIRMWARE_PYTHON_ONLY_V3
  script="$1"
  shift
  python_bin="${PLUTO_PYTHON:-/usr/bin/python}"
  if [ ! -x "$python_bin" ]; then
    fail "firmware Python not found or not executable: $python_bin"
  fi
  "$python_bin" "$script" "$@"
}

run_pass_generation() {
  OUTPUT="$1"
  HOURS="$2"
  LIMIT="$3"
  STEP_SECONDS="$4"
  START_UTC="$5"

  run_python \
    "${TOOLS_DIR}/update_pass_predictions.py" \
    --catalog "${DATA_DIR}/satellites.json" \
    --observer "${DEPLOY_DIR}/config/observer.json" \
    --output "$OUTPUT" \
    --hours "$HOURS" \
    --limit "$LIMIT" \
    --step-seconds "$STEP_SECONDS" \
    --start-utc "$START_UTC"
}

write_generated_status() {
  TARGET="$1"
  INPUT="$2"
  run_python \
    "${STATUS_UPDATER}" \
    --target "$TARGET" \
    --input "$INPUT" \
    --status-file "${STATUS_FILE}"
}

start_pass_worker_background() {
  mkdir -p "$LOG_DIR"
  WORKER_LOG="${LOG_DIR}/pass_refresh_worker.log"
  (
    PLUTO_DEPLOY_DIR="$DEPLOY_DIR" \
    PLUTO_SD_ROOT="$SD_ROOT" \
    PLUTO_REFRESH_START_UTC="$REFRESH_START_UTC" \
    PLUTO_PASS_QUICK_HOURS="$QUICK_HOURS" \
    PLUTO_PASS_QUICK_LIMIT="$QUICK_LIMIT" \
    PLUTO_PASS_QUICK_STEP_SECONDS="$QUICK_STEP_SECONDS" \
    PLUTO_PASS_FULL_HOURS="$FULL_HOURS" \
    PLUTO_PASS_FULL_LIMIT="$FULL_LIMIT" \
    PLUTO_PASS_STEP_SECONDS="$FULL_STEP_SECONDS" \
    PLUTO_PASS_QUICK_LOCK_MAX_AGE_SECONDS="$QUICK_LOCK_MAX_AGE" \
    PLUTO_PASS_FULL_LOCK_MAX_AGE_SECONDS="$FULL_LOCK_MAX_AGE" \
    /bin/sh "$0" passes_worker
  ) >"$WORKER_LOG" 2>&1 &
}

start_full_background() {
  mkdir -p "$LOG_DIR"
  FULL_LOG="${LOG_DIR}/pass_refresh_full.log"
  (
    PLUTO_DEPLOY_DIR="$DEPLOY_DIR" \
    PLUTO_SD_ROOT="$SD_ROOT" \
    PLUTO_REFRESH_START_UTC="$REFRESH_START_UTC" \
    PLUTO_PASS_FULL_HOURS="$FULL_HOURS" \
    PLUTO_PASS_FULL_LIMIT="$FULL_LIMIT" \
    PLUTO_PASS_STEP_SECONDS="$FULL_STEP_SECONDS" \
    PLUTO_PASS_FULL_LOCK_MAX_AGE_SECONDS="$FULL_LOCK_MAX_AGE" \
    /bin/sh "$0" passes_full
  ) >"$FULL_LOG" 2>&1 &
}

mkdir -p "$DATA_DIR" "$TOOLS_DIR" "$LOG_DIR"

case "$MODE" in
  passes)
    skip_recent_pass_refresh_if_fresh
    clear_pass_locks
    write_status "running" "passes" "Queued 1-hour pass scan on Pluto"
    start_pass_worker_background
    ;;
  passes_worker)
    acquire_lock "/tmp/pluto_refresh_passes_worker.lock" "passes" "Pass refresh worker already in progress" "$QUICK_LOCK_MAX_AGE"
    QUICK_TMP="${DATA_DIR}/passes.quick.tmp.$$"
    write_status "running" "passes" "Generating quick pass preview on Pluto"
    run_pass_generation "$QUICK_TMP" "$QUICK_HOURS" "$QUICK_LIMIT" "$QUICK_STEP_SECONDS" "$REFRESH_START_UTC" || fail "quick pass preview failed"
    mv "$QUICK_TMP" "${DATA_DIR}/passes.json"
    write_generated_status passes "${DATA_DIR}/passes.json" || fail "1-hour pass status update failed"
    write_status "idle" "passes" "Generated 1-hour pass scan; full background rebuild disabled for audio stability"
    ;;
  passes_full)
    write_status "idle" "passes" "Full pass rebuild disabled; using 1-hour pass scan every 30 minutes for audio stability"
    exit 0
    ;;
  catalog)
    acquire_lock "/tmp/pluto_refresh_catalog.lock" "catalog" "Catalog refresh already in progress" "$CATALOG_LOCK_MAX_AGE"
    write_status "running" "catalog" "Refreshing CelesTrak and SatNOGS catalog data on Pluto"
    CATALOG_TMP="${DATA_DIR}/satellites.tmp.$$"
    run_python \
      "${TOOLS_DIR}/update_satellite_catalog.py" \
      --output "$CATALOG_TMP" || fail "catalog refresh failed"
    mv "$CATALOG_TMP" "${DATA_DIR}/satellites.json"
    write_generated_status catalog "${DATA_DIR}/satellites.json" || fail "catalog status update failed"
    ;;
  all)
    "$0" catalog
    "$0" passes
    ;;
  *)
    fail "unknown refresh target: $MODE"
    ;;
esac
