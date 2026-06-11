#!/bin/sh

DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}"
BIN="${DEPLOY_DIR}/pluto_sat_tracker"

TIME_EPOCH_FILE="${DEPLOY_DIR}/last_time_epoch.txt"
TIME_UTC_FILE="${DEPLOY_DIR}/last_time_utc.txt"

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
echo "SD root:    $SD_ROOT"

mkdir -p "$SD_ROOT/data" "$SD_ROOT/cache" "$SD_ROOT/logs"

if try_host_time; then
  :
elif try_internet_time; then
  :
elif try_saved_time; then
  :
else
  use_fallback_time
fi

echo "Current Pluto UTC time: $(date -u 2>/dev/null || true)"
echo "Starting: $BIN --web-dir $DEPLOY_DIR/web --config-dir $DEPLOY_DIR/config --data-dir $SD_ROOT/data $TRACKER_ARGS"
echo

exec "$BIN" \
  --web-dir "$DEPLOY_DIR/web" \
  --config-dir "$DEPLOY_DIR/config" \
  --data-dir "$SD_ROOT/data" \
  $TRACKER_ARGS
