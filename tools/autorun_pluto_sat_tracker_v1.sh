#!/bin/sh
# Pluto Satellite Tracker autorun - reboot survival v3
# AUTORUN_RUNTIME_HARDENING_V1
# REBOOT_SURVIVAL_HARDENING_V2
set +e

APP_ROOT="${APP_ROOT:-/mnt/jffs2/pluto_sat_tracker}"
BIN="${BIN:-$APP_ROOT/bin/pluto_sat_tracker}"
if [ ! -x "$BIN" ] && [ -x "$APP_ROOT/pluto_sat_tracker" ]; then
  BIN="$APP_ROOT/pluto_sat_tracker"
fi

BIND_ADDR="${PLUTO_BIND_ADDR:-0.0.0.0}"
PORT="${PLUTO_PORT:-8080}"
SD_DEV="${PLUTO_SD_DEV:-/dev/mmcblk0p1}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1}"
RUNTIME_ROOT="${PLUTO_RUNTIME_ROOT:-$SD_ROOT/pluto_sat_tracker}"
DATA_DIR="${PLUTO_DATA_DIR:-$RUNTIME_ROOT/data}"
CONFIG_DIR="${PLUTO_CONFIG_DIR:-$RUNTIME_ROOT/config}"
LOG_DIR="${PLUTO_LOG_DIR:-$RUNTIME_ROOT/logs}"
TMP_DIR="${PLUTO_TMP_DIR:-$RUNTIME_ROOT/tmp}"
WEB_DIR="${PLUTO_WEB_DIR:-$APP_ROOT/web}"
ENV_FILE="${PLUTO_ENV_FILE:-/mnt/jffs2/pluto_sat_tracker.env}"
BOOT_LOG="/tmp/pluto_sat_tracker_autorun_boot.log"
LOG="$BOOT_LOG"

log() {
  TS="$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo 1970-01-01T00:00:00Z)"
  echo "$TS $*" >> "$LOG"
}

ensure_sd_mounted_rw() {
  mkdir -p "$SD_ROOT" 2>/dev/null || true
  mount "$SD_DEV" "$SD_ROOT" 2>/dev/null || true

  if touch "$SD_ROOT/.pluto_sat_tracker_rw_test" 2>/dev/null; then
    rm -f "$SD_ROOT/.pluto_sat_tracker_rw_test" 2>/dev/null || true
    return 0
  fi

  log "SD not writable; attempting repair/remount"
  sync 2>/dev/null || true
  umount "$SD_ROOT" 2>/dev/null || true

  if command -v fsck.vfat >/dev/null 2>&1; then
    fsck.vfat -p "$SD_DEV" >>/tmp/pluto_sat_tracker_sd_fsck.log 2>&1 || true
  elif command -v dosfsck >/dev/null 2>&1; then
    dosfsck -a "$SD_DEV" >>/tmp/pluto_sat_tracker_sd_fsck.log 2>&1 || true
  fi

  mount -o rw "$SD_DEV" "$SD_ROOT" 2>/dev/null || mount "$SD_DEV" "$SD_ROOT" 2>/dev/null || true

  if touch "$SD_ROOT/.pluto_sat_tracker_rw_test" 2>/dev/null; then
    rm -f "$SD_ROOT/.pluto_sat_tracker_rw_test" 2>/dev/null || true
    log "SD writable after repair/remount"
    return 0
  fi

  log "WARN: SD still not writable after repair/remount"
  return 1
}

seed_json_if_empty() {
  file="$1"
  kind="$2"
  if [ -s "$file" ]; then
    return 0
  fi

  mkdir -p "$(dirname "$file")" 2>/dev/null || true

  case "$kind" in
    track_state)
      cat > "$file" <<'EOF' 2>/dev/null || true
{"ok":true,"state":"idle","message":"No active Doppler track after reboot","name":"","point_index":-1,"point_time_utc":"","rx_hz":0,"lo_path":"","seconds_until_aos":-1,"seconds_until_los":-1,"seconds_until_next":-1,"seconds_until_point":-1,"lo_write_result":"idle"}
EOF
      ;;
    refresh_status)
      cat > "$file" <<'EOF' 2>/dev/null || true
{"ok":true,"state":"idle","target":"","message":"Idle after reboot","started_utc":"","finished_utc":"","updated_utc":""}
EOF
      ;;
  esac
}

reset_permissions() {
  chmod +x "$BIN" 2>/dev/null || true
  find "$APP_ROOT/bin" -type f -exec chmod +x {} \; 2>/dev/null || true
  find "$APP_ROOT/tools" -type f -name '*.sh' -exec chmod +x {} \; 2>/dev/null || true
}

ensure_initial_files() {
  mkdir -p "$DATA_DIR" "$CONFIG_DIR" "$LOG_DIR" "$TMP_DIR" 2>/dev/null || true

  if [ ! -s "$CONFIG_DIR/observer.json" ]; then
    cat > "$CONFIG_DIR/observer.json" <<'EOF' 2>/dev/null || true
{
  "name": "Default Observer",
  "latitude_deg": 0.0,
  "longitude_deg": 0.0,
  "altitude_m": 0.0,
  "grid": "",
  "minimum_elevation_deg": 10.0
}
EOF
  fi

  seed_json_if_empty "$DATA_DIR/radio_track_state.json" track_state
  seed_json_if_empty "$DATA_DIR/refresh_status.json" refresh_status

  for f in "$DATA_DIR/radio_track.json" "$DATA_DIR/refresh.lock" "$TMP_DIR/refresh.lock"; do
    if [ -e "$f" ] && [ ! -s "$f" ]; then
      rm -f "$f" 2>/dev/null || true
    fi
  done
}

prepare_runtime() {
  if [ -f "$ENV_FILE" ]; then
    . "$ENV_FILE" 2>/dev/null || true
  fi

  mkdir -p "$LOG_DIR" 2>/dev/null || true
  LOG="$LOG_DIR/autorun.log"
  log "logging moved to $LOG"

  reset_permissions
  ensure_initial_files
}

start_backend() {
  if [ ! -x "$BIN" ]; then
    log "ERROR: backend binary missing or not executable: $BIN"
    return 1
  fi
  if [ ! -d "$WEB_DIR" ]; then
    log "ERROR: web dir missing: $WEB_DIR"
    return 1
  fi

  if wget -qO- "http://127.0.0.1:${PORT}/api/status" >/dev/null 2>&1; then
    log "backend already live on port $PORT"
    return 0
  fi

  for pid in $(pidof pluto_sat_tracker 2>/dev/null); do
    kill "$pid" 2>/dev/null || true
  done
  sleep 1

  log "starting $BIN"
  log "bind=$BIND_ADDR port=$PORT data=$DATA_DIR web=$WEB_DIR config=$CONFIG_DIR"

  nohup "$BIN" \
    --bind "$BIND_ADDR" \
    --port "$PORT" \
    --data-dir "$DATA_DIR" \
    --web-dir "$WEB_DIR" \
    --config-dir "$CONFIG_DIR" \
    >> "$LOG" 2>&1 &

  BACKEND_PID="$!"
  echo "$BACKEND_PID" > /tmp/pluto_sat_tracker.pid 2>/dev/null || true
  log "started pid $BACKEND_PID"

  sleep 3
  if wget -qO- "http://127.0.0.1:${PORT}/api/status" >/dev/null 2>&1; then
    log "backend API is live"
    return 0
  fi

  if ps w | grep "[p]luto_sat_tracker" >/dev/null 2>&1; then
    log "backend process exists but API did not answer yet"
    return 0
  fi

  log "ERROR: backend did not survive startup"
  return 1
}

ensure_sd_mounted_rw || true
prepare_runtime
start_backend
exit 0
