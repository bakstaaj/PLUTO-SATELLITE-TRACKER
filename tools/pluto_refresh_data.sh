#!/bin/sh
# ASYNC_PASS_REFRESH_RUNNER_V4
# Refresh Pluto satellite catalog/pass data.
#
# MODE=passes is intentionally non-blocking for browser/UI use:
#   1. Return quickly after queueing a background quick-preview worker.
#   2. The worker generates a small current preview and publishes it atomically.
#   3. The worker then queues a full 24-hour rebuild in the background.
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
# Persistent static data on jffs2 (satellites.json written by catalog refresh)
STATIC_DATA_DIR="${DEPLOY_DIR}/data"
# Ephemeral runtime data in /tmp (passes, status, logs — reset on reboot)
TRANSIENT_DIR="${PLUTO_SAT_TRANSIENT_DIR:-/tmp/pluto_sat_tracker}"
TOOLS_DIR="${SD_ROOT}/tools"
PYTHON_DIR="${SD_ROOT}/python"
PYTHON_RUNTIME_DIR="${SD_ROOT}/python-runtime"
MODE="${1:-passes}"
STATUS_FILE="${TRANSIENT_DIR}/refresh_status.json"
STATUS_UPDATER="${TOOLS_DIR}/write_refresh_status.py"
LOG_DIR="${TRANSIENT_DIR}/logs"

QUICK_HOURS="${PLUTO_PASS_QUICK_HOURS:-8}"
QUICK_LIMIT="${PLUTO_PASS_QUICK_LIMIT:-10}"
QUICK_STEP_SECONDS="${PLUTO_PASS_QUICK_STEP_SECONDS:-120}"
FULL_HOURS="${PLUTO_PASS_FULL_HOURS:-24}"
FULL_LIMIT="${PLUTO_PASS_FULL_LIMIT:-80}"
FULL_STEP_SECONDS="${PLUTO_PASS_STEP_SECONDS:-60}"
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

  mkdir -p "$TRANSIENT_DIR"
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

worker_is_running_and_fresh() {
  # Returns 0 (true) if the pass worker is already running and within QUICK_LOCK_MAX_AGE.
  # This prevents double-start when the UI calls /api/refresh/passes multiple times.
  WORKER_LOCK="/tmp/pluto_refresh_passes_worker.lock"
  if [ ! -d "$WORKER_LOCK" ]; then return 1; fi
  OLD_PID=""
  if [ -f "${WORKER_LOCK}/pid" ]; then
    OLD_PID="$(cat "${WORKER_LOCK}/pid" 2>/dev/null || true)"
  fi
  if [ -z "$OLD_PID" ] || ! kill -0 "$OLD_PID" 2>/dev/null; then return 1; fi
  AGE="$(lock_age_seconds "$WORKER_LOCK")"
  if [ "$AGE" -le "$QUICK_LOCK_MAX_AGE" ] 2>/dev/null; then return 0; fi
  return 1
}

clear_stale_pass_locks() {
  # Clear only STALE (not actively running) pass locks so boot-time zombie
  # locks don't survive. Does NOT kill a fresh running worker.
  for LOCK_DIR in /tmp/pluto_refresh_passes_quick.lock /tmp/pluto_refresh_passes_full.lock /tmp/pluto_refresh_passes_worker.lock /tmp/pluto_refresh_passes.lock; do
    OLD_PID=""
    if [ -f "${LOCK_DIR}/pid" ]; then
      OLD_PID="$(cat "${LOCK_DIR}/pid" 2>/dev/null || true)"
    fi
    # Skip if PID is alive and lock is fresh
    if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
      AGE="$(lock_age_seconds "$LOCK_DIR")"
      if [ "$AGE" -le "$QUICK_LOCK_MAX_AGE" ] 2>/dev/null; then continue; fi
      # Stale running process — kill it
      kill "$OLD_PID" >/dev/null 2>&1 || true
      sleep 1
      kill -9 "$OLD_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$LOCK_DIR" >/dev/null 2>&1 || true
  done
}

run_python() {
  SCRIPT="$1"
  shift
  PYTHON_BIN="${PLUTO_PYTHON:-}"
  SD_RUNTIME=0

  if [ -n "$PYTHON_BIN" ] && [ ! -x "$PYTHON_BIN" ]; then
    fail "configured Python runtime is not executable: $PYTHON_BIN"
  fi
  if [ -z "$PYTHON_BIN" ] && [ -f "${PYTHON_RUNTIME_DIR}/bin/python3.11" ]; then
    PYTHON_BIN="${PYTHON_RUNTIME_DIR}/bin/python3.11"
    SD_RUNTIME=1
  fi
  if [ -z "$PYTHON_BIN" ] && command -v python3 >/dev/null 2>&1; then
    PYTHON_BIN="$(command -v python3)"
  fi
  if [ -z "$PYTHON_BIN" ]; then
    fail "python3 runtime not found; deploy runtime/python-pluto-armhf.tar.gz to the SD card"
  fi
  if [ ! -f "$SCRIPT" ]; then
    fail "refresh script not found: $SCRIPT"
  fi

  if [ "$SD_RUNTIME" = "1" ]; then
    export PYTHONHOME="${PYTHON_RUNTIME_DIR}"
    export LD_LIBRARY_PATH="${PYTHON_RUNTIME_DIR}/lib:${PYTHON_RUNTIME_DIR}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    if PYTHONPATH="${PYTHON_DIR}${PYTHONPATH:+:${PYTHONPATH}}" "$PYTHON_BIN" "$SCRIPT" "$@" 2>/tmp/pluto_python_direct.err; then
      rm -f /tmp/pluto_python_direct.err
      return 0
    fi
    if [ -x /lib/ld-linux-armhf.so.3 ]; then
      PYTHONPATH="${PYTHON_DIR}${PYTHONPATH:+:${PYTHONPATH}}" /lib/ld-linux-armhf.so.3 "$PYTHON_BIN" "$SCRIPT" "$@"
      return $?
    fi
    cat /tmp/pluto_python_direct.err >&2 2>/dev/null || true
    fail "SD-card Python runtime is present but could not be executed"
  fi

  PYTHONPATH="${PYTHON_DIR}${PYTHONPATH:+:${PYTHONPATH}}" "$PYTHON_BIN" "$SCRIPT" "$@"
}

run_pass_generation() {
  OUTPUT="$1"
  HOURS="$2"
  LIMIT="$3"
  STEP_SECONDS="$4"
  START_UTC="$5"

  run_python \
    "${TOOLS_DIR}/update_pass_predictions.py" \
    --catalog "${STATIC_DATA_DIR}/satellites.json" \
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

mkdir -p "$TRANSIENT_DIR" "$LOG_DIR" "$STATIC_DATA_DIR"

case "$MODE" in
  passes)
    # If a fresh worker is already running, do nothing — avoids double-start
    # when the UI (bootstrap + periodic timer + rescue) all call this quickly.
    if worker_is_running_and_fresh; then
      write_status "running" "passes" "Pass refresh already in progress — waiting for results"
      exit 0
    fi
    clear_stale_pass_locks
    write_status "running" "passes" "Queued pass preview on Pluto"
    start_pass_worker_background
    ;;
  passes_worker)
    acquire_lock "/tmp/pluto_refresh_passes_worker.lock" "passes" "Pass refresh worker already in progress" "$QUICK_LOCK_MAX_AGE"
    QUICK_TMP="${TRANSIENT_DIR}/passes.quick.tmp.$$"
    write_status "running" "passes" "Generating ${QUICK_LIMIT} passes across ${QUICK_HOURS}h on Pluto"
    run_pass_generation "$QUICK_TMP" "$QUICK_HOURS" "$QUICK_LIMIT" "$QUICK_STEP_SECONDS" "$REFRESH_START_UTC" || fail "pass generation failed"
    mv "$QUICK_TMP" "${TRANSIENT_DIR}/passes.json"
    write_generated_status passes "${TRANSIENT_DIR}/passes.json" || fail "pass status update failed"
    # No full background run — 10 passes every 15 min is sufficient.
    ;;
  passes_full)
    # Retained 