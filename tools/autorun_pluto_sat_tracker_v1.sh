#!/bin/sh
# Pluto Satellite Tracker autorun.
#
# Installed to:
#   /mnt/jffs2/autorun.sh
#
# AUTORUN_RUNTIME_HARDENING_V1
# It mounts SD, validates read/write runtime, fixes execute permissions,
# ensures observer.json exists, and starts the tracker backend.

set +e

ENV_FILE="/mnt/jffs2/pluto_sat_tracker.env"
if [ -f "$ENV_FILE" ]; then
  . "$ENV_FILE"
fi

DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}"
SD_MOUNT="${PLUTO_SD_MOUNT:-/media/mmcblk0p1}"
SD_DEV="${PLUTO_SD_DEV:-/dev/mmcblk0p1}"
WEB_DIR="${PLUTO_WEB_DIR:-$DEPLOY_DIR/web}"
DATA_DIR="${PLUTO_DATA_DIR:-$SD_ROOT/data}"
CONFIG_DIR="${PLUTO_CONFIG_DIR:-$SD_ROOT/config}"
BIND_ADDR="${PLUTO_BIND_ADDR:-0.0.0.0}"
PORT="${PLUTO_PORT:-8080}"
LOG_DIR="$SD_ROOT/logs"
LOG_FILE="/tmp/pluto_sat_tracker_autorun.log"

log() {
  echo "$(date -u '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || date) $*" >> "$LOG_FILE"
}

is_mounted() {
  grep -qs " $1 " /proc/mounts
}

mount_line() {
  grep " $SD_MOUNT " /proc/mounts | tail -1
}

is_rw_mount() {
  mount_line | grep -q " rw,"
}

kill_matching() {
  pattern="$1"
  ps w 2>/dev/null | awk -v pat="$pattern" '$0 ~ pat && $0 !~ /awk/ {print $1}' | while read pid; do
    case "$pid" in
      ''|*[!0-9]*) ;;
      *) kill "$pid" 2>/dev/null || true ;;
    esac
  done
}

ensure_sd_mounted_rw() {
  mkdir -p "$SD_MOUNT"
  if ! is_mounted "$SD_MOUNT" && [ -b "$SD_DEV" ]; then
    mount "$SD_DEV" "$SD_MOUNT" >>"$LOG_FILE" 2>&1
  fi

  if is_mounted "$SD_MOUNT" && ! is_rw_mount; then
    log "WARN: SD mounted read-only; attempting remount,rw"
    mount -o remount,rw "$SD_MOUNT" >>"$LOG_FILE" 2>&1 || true
  fi

  mkdir -p "$DATA_DIR" "$CONFIG_DIR" "$LOG_DIR" "$SD_ROOT/tmp" >>"$LOG_FILE" 2>&1 || {
    log "ERROR: cannot create SD runtime dirs; SD may be read-only"
    exit 1
  }

  TEST="$SD_ROOT/.autorun_rw_test_$$"
  echo "rw-test" > "$TEST" 2>>"$LOG_FILE" || {
    log "ERROR: SD runtime is not writable; repair SD card and rerun install"
    mount_line >>"$LOG_FILE" 2>&1 || true
    exit 1
  }
  rm -f "$TEST" 2>>"$LOG_FILE" || true
}

ensure_initial_files() {
  if [ ! -f "$DATA_DIR/passes.json" ]; then
    cat > "$DATA_DIR/passes.json" <<'EOF'
{
  "ok": true,
  "passes": [],
  "generated_utc": null,
  "metadata": {
    "initial_install": true,
    "message": "Open the web UI so browser time sync can run, then use Refresh Passes."
  }
}
EOF
  fi

  if [ ! -f "$DATA_DIR/refresh_status.json" ]; then
    cat > "$DATA_DIR/refresh_status.json" <<'EOF'
{
  "ok": true,
  "state": "idle",
  "target": "passes",
  "updated_utc": null,
  "message": "Waiting for browser time sync and pass refresh."
}
EOF
  fi

  mkdir -p "$DEPLOY_DIR/config" "$CONFIG_DIR"
  if [ -s "$CONFIG_DIR/observer.json" ]; then
    cp "$CONFIG_DIR/observer.json" "$DEPLOY_DIR/config/observer.json" 2>>"$LOG_FILE" || true
  elif [ -s "$DEPLOY_DIR/config/observer.json" ]; then
    cp "$DEPLOY_DIR/config/observer.json" "$CONFIG_DIR/observer.json" 2>>"$LOG_FILE" || true
  else
    cat > "$CONFIG_DIR/observer.json" <<'EOF'
{
  "name": "Cripple Creek",
  "latitude_deg": 38.80063,
  "longitude_deg": -105.2,
  "altitude_m": 2805.0,
  "minimum_elevation_deg": 20.0
}
EOF
    cp "$CONFIG_DIR/observer.json" "$DEPLOY_DIR/config/observer.json" 2>>"$LOG_FILE" || true
  fi
}

reset_permissions() {
  chmod +x "$DEPLOY_DIR/bin/pluto_sat_tracker" "$DEPLOY_DIR/bin/pluto_fm_receiver" 2>>"$LOG_FILE" || true
  find "$DEPLOY_DIR/tools" -type f -name '*.sh' -exec chmod +x {} \; 2>>"$LOG_FILE" || true
  find "$SD_ROOT/tools" -type f -name '*.sh' -exec chmod +x {} \; 2>>"$LOG_FILE" || true
  find "$SD_ROOT/python-runtime/bin" -type f -exec chmod +x {} \; 2>>"$LOG_FILE" || true
  find "$SD_ROOT/python/bin" -type f -exec chmod +x {} \; 2>>"$LOG_FILE" || true
}

log "===== autorun start ====="

ensure_sd_mounted_rw

if [ -d "$LOG_DIR" ]; then
  LOG_FILE="$LOG_DIR/autorun.log"
  log "logging moved to $LOG_FILE"
fi

reset_permissions
ensure_initial_files

if [ ! -x "$DEPLOY_DIR/bin/pluto_sat_tracker" ]; then
  log "ERROR: missing backend binary $DEPLOY_DIR/bin/pluto_sat_tracker"
  exit 1
fi

if [ ! -d "$WEB_DIR" ]; then
  log "ERROR: missing web dir $WEB_DIR"
  exit 1
fi

kill_matching "pluto_sat_tracker"
if command -v killall >/dev/null 2>&1; then
  killall pluto_sat_tracker 2>/dev/null || true
fi
sleep 1

export PATH="$DEPLOY_DIR/bin:$DEPLOY_DIR/tools:$PATH"
export PLUTO_DEPLOY_DIR="$DEPLOY_DIR"
export PLUTO_SD_ROOT="$SD_ROOT"
export PLUTO_WEB_DIR="$WEB_DIR"
export PLUTO_DATA_DIR="$DATA_DIR"
export PLUTO_CONFIG_DIR="$CONFIG_DIR"

log "starting $DEPLOY_DIR/bin/pluto_sat_tracker"
log "bind=$BIND_ADDR port=$PORT data=$DATA_DIR web=$WEB_DIR config=$CONFIG_DIR"

nohup "$DEPLOY_DIR/bin/pluto_sat_tracker" \
  --bind "$BIND_ADDR" \
  --port "$PORT" \
  --data-dir "$DATA_DIR" \
  --web-dir "$WEB_DIR" \
  --config-dir "$CONFIG_DIR" \
  >> "$LOG_FILE" 2>&1 &

echo $! > "$SD_ROOT/pluto_sat_tracker.pid"
log "started pid $(cat "$SD_ROOT/pluto_sat_tracker.pid" 2>/dev/null)"
exit 0
