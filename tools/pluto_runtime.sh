#!/bin/sh

DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}"
TMP_ROOT="/tmp/pluto_sat_tracker"
BIN="${DEPLOY_DIR}/pluto_sat_tracker"
REFRESH_RUNNER="${SD_ROOT}/tools/pluto_refresh_data.sh"
PASS_REFRESH_LOOP="${SD_ROOT}/tools/pluto_pass_refresh_loop.sh"
OBSERVER_FILE="${DEPLOY_DIR}/config/observer.json"
CATALOG_FILE="${DEPLOY_DIR}/data/satellites.json"
PASSES_FILE="${TMP_ROOT}/passes.json"
STARTUP_REFRESH_LOG="${TMP_ROOT}/logs/startup_pass_refresh.log"
PASS_REFRESH_LOOP_LOG="${TMP_ROOT}/logs/pass_refresh_loop.log"

TIME_EPOCH_FILE="${DEPLOY_DIR}/last_time_epoch.txt"
TIME_UTC_FILE="${DEPLOY_DIR}/last_time_utc.txt"
STARTUP_TIMING_LOG="${TMP_ROOT}/logs/startup_timing.log"

tlog_runtime() {
  mkdir -p "${TMP_ROOT}/logs"
  printf "[%s] [runtime] %s\n" "$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || echo unknown)" "$*" >> "$STARTUP_TIMING_LOG"
  echo "$*"
}

# 2026-06-11 00:00:00 UTC
FALLBACK_UTC="2026.06.11-00:00:00"
FALLBACK_EPOCH="1781136000"

HOST_UTC=""
TRACKER_ARGS="--interactive --net"

while [ $# -gt 0 ]; do
  case "$1" in
    --host-time-utc)
      HOST_UTC="$2"
      shift 2
      ;;
    --)
      shift
      TRACKER_ARGS="$*"
      break
      ;;
    *)
      TRACKER_ARGS="$*"
      break
      ;;
  esac
done

is_valid_epoch() {
  E="$1"
  case "$E" in
    ""|*[!0-9]*) return 1 ;;
  esac
  [ "$E" -ge "$FALLBACK_EPOCH" ]
}

set_utc_string() {
  UTC_VALUE="$1"
  date -u -s "$UTC_VALUE" >/dev/null 2>&1
}

current_epoch() {
  date -u +%s 2>/dev/null || echo 0
}

save_time() {
  NOW_EPOCH="$(current_epoch)"
  if is_valid_epoch "$NOW_EPOCH"; then
    mkdir -p "$DEPLOY_DIR"
    echo "$NOW_EPOCH" > "$TIME_EPOCH_FILE"
    date -u "+%Y.%m.%d-%H:%M:%S" > "$TIME_UTC_FILE" 2>/dev/null || true
    sync
  fi
}

maybe_refresh_passes() {
  if [ ! -x "$REFRESH_RUNNER" ] && [ ! -r "$REFRESH_RUNNER" ]; then
    echo "Startup pass refresh: runner not available"
    return 0
  fi
  if [ ! -f "$OBSERVER_FILE" ]; then
    echo "Startup pass refresh: observer config not found"
    return 0
  fi
  if [ ! -f "$CATALOG_FILE" ]; then
    echo "Startup pass refresh: satellite catalog not found"
    return 0
  fi

  echo "Startup pass refresh: regenerating passes from saved observer config"
  (
    /bin/sh "$REFRESH_RUNNER" passes_boot
  ) >"$STARTUP_REFRESH_LOG" 2>&1 &
}

start_pass_refresh_loop() {
  if [ ! -x "$PASS_REFRESH_LOOP" ] && [ ! -r "$PASS_REFRESH_LOOP" ]; then
    echo "Background pass refresh loop: runner not available"
    return 0
  fi

  echo "Background pass refresh loop: starting"
  (
    /bin/sh "$PASS_REFRESH_LOOP"
  ) >"$PASS_REFRESH_LOOP_LOG" 2>&1 &
}

try_host_time() {
  if [ -n "$HOST_UTC" ]; then
    if set_utc_string "$HOST_UTC"; then
      echo "Time source: host-passed UTC time ($HOST_UTC)"
      return 0
    fi
    echo "Warning: host-passed time failed: $HOST_UTC"
  fi
  return 1
}

try_internet_time() {
  echo "Trying internet time..."

  if command -v ntpd >/dev/null 2>&1; then
    ntpd -q -p pool.ntp.org >/dev/null 2>&1
    if is_valid_epoch "$(current_epoch)"; then
      echo "Time source: internet ntpd"
      return 0
    fi
  fi

  if command -v busybox >/dev/null 2>&1; then
    busybox ntpd -q -p pool.ntp.org >/dev/null 2>&1
    if is_valid_epoch "$(current_epoch)"; then
      echo "Time source: internet busybox ntpd"
      return 0
    fi
  fi

  return 1
}

try_saved_time() {
  if [ -f "$TIME_UTC_FILE" ]; then
    SAVED_UTC="$(head -1 "$TIME_UTC_FILE" 2>/dev/null)"
    if [ -n "$SAVED_UTC" ]; then
      if set_utc_string "$SAVED_UTC"; then
        echo "Time source: saved previous UTC time ($SAVED_UTC)"
        return 0
      fi
    fi
  fi

  return 1
}

use_fallback_time() {
  set_utc_string "$FALLBACK_UTC"
  echo "Time source: fallback UTC time ($FALLBACK_UTC)"
}

trap "save_time; exit 130" INT
trap "save_time; exit 143" TERM
trap "save_time" EXIT

echo "== Pluto Satellite Tracker runtime =="
echo "Deploy dir: $DEPLOY_DIR"
echo "SD root:    $SD_ROOT (read-only OK)"
echo "Tmp root:   $TMP_ROOT"

# Kill any existing tracker instance before starting
if command -v killall >/dev/null 2>&1; then
  killall pluto_sat_tracker 2>/dev/null || true
else
  kill "$(cat /var/run/pluto_sat_tracker.pid 2>/dev/null)" 2>/dev/null || true
  ps | awk '/pluto_sat_tracker/ && !/awk/ { print $1 }' | while read -r pid; do
    kill "$pid" 2>/dev/null || true
  done
fi
sleep 1

# jffs2 dirs (persistent, writable)
mkdir -p "${DEPLOY_DIR}/data"
# transient dirs (reset on reboot, always writable)
mkdir -p "${TMP_ROOT}/logs"

tlog_runtime "boot start"
BOOT_TIME_RELIABLE=0
if try_host_time; then
  BOOT_TIME_RELIABLE=1
  tlog_runtime "time source: host"
elif try_internet_time; then
  BOOT_TIME_RELIABLE=1
  tlog_runtime "time source: internet/ntp"
elif try_saved_time; then
  BOOT_TIME_RELIABLE=1
  tlog_runtime "time source: saved"
else
  use_fallback_time
  BOOT_TIME_RELIABLE=0
  tlog_runtime "time source: fallback (June 11) — pass gen deferred to browser"
fi

tlog_runtime "clock set: $(date -u 2>/dev/null || true)"
if [ "$BOOT_TIME_RELIABLE" = "1" ]; then
  tlog_runtime "maybe_refresh_passes: starting"
  maybe_refresh_passes
  tlog_runtime "maybe_refresh_passes: queued (running in background)"
else
  tlog_runtime "maybe_refresh_passes: skipped (fallback clock)"
fi
tlog_runtime "starting pass refresh loop"
start_pass_refresh_loop
echo "Starting: $BIN --web-dir $DEPLOY_DIR/web --config-dir $DEPLOY_DIR/config --data-dir $DEPLOY_DIR/data --transient-dir $TMP_ROOT --sd-root $SD_ROOT $TRACKER_ARGS"
echo

exec "$BIN" \
  --web-dir "$DEPLOY_DIR/web" \
  --config-dir "$DEPLOY_DIR/config" \
  --data-dir "$DEPLOY_DIR/data" \
  --transient-dir "$TMP_ROOT" \
  --sd-root "$SD_ROOT" \
  $TRACKER_ARGS
