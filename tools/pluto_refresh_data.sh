#!/bin/sh
# CLEAN_JFFS2_TMP_PASS_REFRESH_RUNNER_V3
# Quiet API-safe refresh runner for Pluto Satellite Tracker.
# Persistent app data: /mnt/jffs2/pluto_sat_tracker
# Transactional temp/log data: /tmp/pluto_sat_tracker
# Intentionally disables the old full 24-hour background rebuild.

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
DATA_DIR="$(sanitize_path "${PLUTO_SAT_DATA_DIR:-${DEPLOY_DIR}/data}")"
CONFIG_DIR="$(sanitize_path "${PLUTO_SAT_CONFIG_DIR:-${DEPLOY_DIR}/config}")"
TOOLS_DIR="$(sanitize_path "${PLUTO_TOOLS_DIR:-${DEPLOY_DIR}/tools}")"
PYTHON_DIR="$(sanitize_path "${PLUTO_PYTHON_DIR:-${DEPLOY_DIR}/python}")"
TXN_DIR="$(sanitize_path "${PLUTO_TXN_DIR:-/tmp/pluto_sat_tracker}")"
LOG_DIR="${TXN_DIR}/logs"

MODE="${1:-passes}"
STATUS_FILE="${DATA_DIR}/refresh_status.json"
STATUS_UPDATER="${TOOLS_DIR}/write_refresh_status.py"
PASSES_JSON="${DATA_DIR}/passes.json"

QUICK_HOURS="${PLUTO_PASS_QUICK_HOURS:-1}"
QUICK_LIMIT="${PLUTO_PASS_QUICK_LIMIT:-20}"
QUICK_STEP_SECONDS="${PLUTO_PASS_QUICK_STEP_SECONDS:-120}"
PASS_SAMPLE_SECONDS="${PLUTO_PASS_SAMPLE_SECONDS:-5}"
QUICK_LOCK_MAX_AGE="${PLUTO_PASS_QUICK_LOCK_MAX_AGE_SECONDS:-180}"
REFRESH_START_UTC="${PLUTO_REFRESH_START_UTC:-$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo '')}"

mkdir -p "$DATA_DIR" "$CONFIG_DIR" "$TOOLS_DIR" "$PYTHON_DIR" "$TXN_DIR" "$LOG_DIR"

json_clean() {
  printf "%s" "$1" | tr '\r\n"' '   ' | cut -c 1-240
}

now_utc() {
  date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo ''
}

write_status() {
  STATE="$1"
  TARGET="$2"
  MESSAGE="$(json_clean "$3")"
  NOW="$(now_utc)"
  TMP="${TXN_DIR}/refresh_status.$$.json"
  cat >"$TMP" <<EOF
{
  "ok": true,
  "state": "${STATE}",
  "target": "${TARGET}",
  "updated_utc": "${NOW}",
  "message": "${MESSAGE}"
}
EOF
  mv "$TMP" "$STATUS_FILE"
}

write_error_status() {
  TARGET="$1"
  MESSAGE="$(json_clean "$2")"
  NOW="$(now_utc)"
  TMP="${TXN_DIR}/refresh_status.error.$$.json"
  cat >"$TMP" <<EOF
{
  "ok": false,
  "state": "error",
  "target": "${TARGET}",
  "updated_utc": "${NOW}",
  "message": "${MESSAGE}"
}
EOF
  mv "$TMP" "$STATUS_FILE"
}

emit_existing_status_clean() {
  # If an older corrupted status file exists, replace it before the API reads it.
  if [ ! -s "$STATUS_FILE" ]; then
    write_status "idle" "${MODE}" "Refresh idle"
  fi
  if ! json_validate "$STATUS_FILE" >/dev/null 2>&1; then
    write_status "idle" "${MODE}" "Refresh status reset after invalid JSON"
  fi
  cat "$STATUS_FILE"
}

fail() {
  MSG="$1"
  write_error_status "$MODE" "$MSG"
  echo "$MSG" >&2
  exit 1
}

now_epoch() {
  date -u '+%s' 2>/dev/null || echo 0
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
    trap 'rm -rf "$LOCK_DIR" >/dev/null 2>&1 || true' EXIT INT TERM
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
    trap 'rm -rf "$LOCK_DIR" >/dev/null 2>&1 || true' EXIT INT TERM
    return 0
  fi

  write_status "running" "$TARGET" "$BUSY_MESSAGE"
  exit 0
}

clear_pass_locks() {
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

python_bin() {
  if [ -n "${PLUTO_PYTHON:-}" ] && [ -x "${PLUTO_PYTHON:-}" ]; then
    printf "%s" "$PLUTO_PYTHON"
    return 0
  fi
  if [ -x /usr/bin/python ]; then
    printf "%s" /usr/bin/python
    return 0
  fi
  if command -v python3 >/dev/null 2>&1; then
    command -v python3
    return 0
  fi
  if command -v python >/dev/null 2>&1; then
    command -v python
    return 0
  fi
  return 1
}

run_python() {
  SCRIPT="$1"
  shift
  PYTHON_BIN="$(python_bin)" || fail "Python runtime not found"
  [ -f "$SCRIPT" ] || fail "refresh script not found: $SCRIPT"
  PYTHONPATH="${PYTHON_DIR}${PYTHONPATH:+:${PYTHONPATH}}" "$PYTHON_BIN" "$SCRIPT" "$@"
}

json_validate() {
  FILE="$1"
  PYTHON_BIN="$(python_bin)" || return 1
  "$PYTHON_BIN" - "$FILE" <<'PY'
import json, sys
with open(sys.argv[1], 'r') as f:
    json.load(f)
PY
}

run_pass_generation() {
  OUTPUT="$1"
  run_python \
    "${TOOLS_DIR}/update_pass_predictions.py" \
    --catalog "${DATA_DIR}/satellites.json" \
    --observer "${CONFIG_DIR}/observer.json" \
    --output "$OUTPUT" \
    --hours "$QUICK_HOURS" \
    --limit "$QUICK_LIMIT" \
    --step-seconds "$QUICK_STEP_SECONDS" \
    --pass-sample-seconds "$PASS_SAMPLE_SECONDS" \
    --start-utc "$REFRESH_START_UTC"
}

write_generated_status() {
  TARGET="$1"
  INPUT="$2"
  if [ -f "$STATUS_UPDATER" ]; then
    run_python \
      "$STATUS_UPDATER" \
      --target "$TARGET" \
      --input "$INPUT" \
      --status-file "$STATUS_FILE"
  else
    write_status "ok" "$TARGET" "Pass predictions regenerated on Pluto"
  fi
}

publish_generated_file() {
  INPUT="$1"
  OUTPUT="$2"
  [ -s "$INPUT" ] || fail "generated pass file is empty: $INPUT"
  json_validate "$INPUT" || fail "generated pass file is not valid JSON: $INPUT"
  TMP_OUT="${TXN_DIR}/$(basename "$OUTPUT").publish.$$.json"
  cp "$INPUT" "$TMP_OUT" || fail "copy to transaction temp failed"
  json_validate "$TMP_OUT" || fail "published temp file failed JSON validation"
  FINAL_TMP="${OUTPUT}.tmp.$$"
  cp "$TMP_OUT" "$FINAL_TMP" || fail "copy to final JFFS2 temp failed"
  mv "$FINAL_TMP" "$OUTPUT" || fail "atomic publish to $OUTPUT failed"
  rm -f "$TMP_OUT" >/dev/null 2>&1 || true
}

start_pass_worker_background() {
  WORKER_LOG="${LOG_DIR}/pass_refresh_worker.log"
  SCRIPT_PATH="${TOOLS_DIR}/pluto_refresh_data.sh"
  (
    export PLUTO_DEPLOY_DIR="$DEPLOY_DIR"
    export PLUTO_SAT_DATA_DIR="$DATA_DIR"
    export PLUTO_SAT_CONFIG_DIR="$CONFIG_DIR"
    export PLUTO_TOOLS_DIR="$TOOLS_DIR"
    export PLUTO_PYTHON_DIR="$PYTHON_DIR"
    export PLUTO_TXN_DIR="$TXN_DIR"
    export PLUTO_REFRESH_START_UTC="$REFRESH_START_UTC"
    export PLUTO_PASS_QUICK_HOURS="$QUICK_HOURS"
    export PLUTO_PASS_QUICK_LIMIT="$QUICK_LIMIT"
    export PLUTO_PASS_QUICK_STEP_SECONDS="$QUICK_STEP_SECONDS"
    export PLUTO_PASS_SAMPLE_SECONDS="$PASS_SAMPLE_SECONDS"
    /bin/sh "$SCRIPT_PATH" passes_worker
  ) >"$WORKER_LOG" 2>&1 &
}

case "$MODE" in
  status|refresh_status)
    emit_existing_status_clean
    ;;
  passes)
    clear_pass_locks
    write_status "running" "passes" "Queued 1-hour pass scan on Pluto"
    start_pass_worker_background
    emit_existing_status_clean
    ;;
  passes_worker)
    acquire_lock "/tmp/pluto_refresh_passes_worker.lock" "passes" "Pass refresh worker already in progress" "$QUICK_LOCK_MAX_AGE"
    QUICK_TMP="${TXN_DIR}/passes.quick.$$.json"
    rm -f "$QUICK_TMP" "${QUICK_TMP}.tmp" >/dev/null 2>&1 || true
    write_status "running" "passes" "Generating 1-hour pass scan on Pluto"
    run_pass_generation "$QUICK_TMP" || fail "1-hour pass scan failed"
    publish_generated_file "$QUICK_TMP" "$PASSES_JSON"
    write_generated_status passes "$PASSES_JSON" || fail "pass status update failed"
    rm -f "$QUICK_TMP" >/dev/null 2>&1 || true
    ;;
  passes_full)
    write_status "idle" "passes" "Full 24-hour pass rebuild disabled for audio stability"
    ;;
  catalog)
    fail "catalog refresh not changed by this repair script"
    ;;
  all)
    "$0" passes
    ;;
  *)
    fail "unknown refresh target: $MODE"
    ;;
esac
