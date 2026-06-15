#!/bin/sh
# Install Pluto Satellite Tracker from an SD-card installer folder.
#
# Expected location on Pluto:
#   /media/mmcblk0p1/PlutoSatelliteTrackerInstall/install_from_sd_pluto_sat_tracker_v1.sh
#
# Run:
#   ./install_from_sd_pluto_sat_tracker_v1.sh --start
#
# INSTALL_RUNTIME_HARDENING_V1
# Hardened after SD-card field install testing:
#   - validates SD card is mounted read-write before install
#   - resets executable permissions on binaries/scripts/python runtime
#   - creates observer.json in SD config and deploy config
#   - clears stale pass refresh locks/status
#   - installs autorun and can start backend cleanly

set -eu

DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_RUNTIME_NAME="${PLUTO_SD_RUNTIME_NAME:-pluto_sat_tracker}"
START_APP=0
RESET_DATA=0

while [ "$#" -gt 0 ]; do
  case "$1" in
    --start) START_APP=1; shift ;;
    --no-start) START_APP=0; shift ;;
    --reset-data) RESET_DATA=1; shift ;;
    *) echo "ERROR: unknown option: $1" >&2; exit 2 ;;
  esac
done

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
LOG="/tmp/pluto_sat_tracker_sd_install.log"

log() { echo "$*" | tee -a "$LOG"; }
fail() { log "FAIL: $*"; exit 1; }
need_file() { [ -f "$1" ] || fail "missing required file: $1"; }
need_dir() { [ -d "$1" ] || fail "missing required directory: $1"; }

SD_MOUNT="$(df -P "$SCRIPT_DIR" 2>/dev/null | awk 'NR==2 {print $6}')"
if [ -z "$SD_MOUNT" ]; then
  SD_MOUNT="/media/mmcblk0p1"
fi
SD_DEV="${PLUTO_SD_DEV:-/dev/mmcblk0p1}"
SD_ROOT="${PLUTO_SD_ROOT:-$SD_MOUNT/$SD_RUNTIME_NAME}"

is_mounted() {
  grep -qs " $1 " /proc/mounts
}

mount_line() {
  grep " $SD_MOUNT " /proc/mounts | tail -1
}

is_rw_mount() {
  mount_line | grep -q " rw,"
}

ensure_sd_writable() {
  log "Checking SD card write access..."
  if is_mounted "$SD_MOUNT" && ! is_rw_mount; then
    log "WARN: SD appears mounted read-only; attempting remount,rw"
    mount -o remount,rw "$SD_MOUNT" >>"$LOG" 2>&1 || true
  fi

  mkdir -p "$SD_ROOT" "$SD_ROOT/data" "$SD_ROOT/config" "$SD_ROOT/logs" "$SD_ROOT/tmp" >>"$LOG" 2>&1 || {
    mount_line >>"$LOG" 2>&1 || true
    fail "cannot create SD runtime directories; SD may be read-only"
  }

  TEST="$SD_ROOT/.install_rw_test_$$"
  echo "rw-test" > "$TEST" 2>>"$LOG" || {
    mount_line >>"$LOG" 2>&1 || true
    fail "SD card is not writable. Repair the SD card, then rerun installer. On Windows run: chkdsk X: /f"
  }
  rm -f "$TEST" >>"$LOG" 2>&1 || true
  log "PASS: SD write test succeeded"
}

copy_tree_replace() {
  src="$1"; dst="$2"
  mkdir -p "$dst"
  rm -rf "$dst"
  mkdir -p "$dst"
  cp -a "$src"/. "$dst"/
}

copy_tree_missing_only() {
  src="$1"; dst="$2"
  mkdir -p "$dst"
  [ -d "$src" ] || return 0
  (cd "$src" && find . -type d -print) | while read d; do mkdir -p "$dst/$d"; done
  (cd "$src" && find . -type f -print) | while read f; do
    if [ "$RESET_DATA" -eq 1 ] || [ ! -f "$dst/$f" ]; then
      cp "$src/$f" "$dst/$f"
    fi
  done
}

reset_permissions() {
  log "Resetting executable permissions..."
  chmod +x "$DEPLOY_DIR/bin/pluto_sat_tracker" "$DEPLOY_DIR/bin/pluto_fm_receiver" 2>>"$LOG" || true
  find "$DEPLOY_DIR/tools" -type f -name '*.sh' -exec chmod +x {} \; 2>>"$LOG" || true
  find "$SD_ROOT/tools" -type f -name '*.sh' -exec chmod +x {} \; 2>>"$LOG" || true
  find "$SD_ROOT/python-runtime/bin" -type f -exec chmod +x {} \; 2>>"$LOG" || true
  find "$SD_ROOT/python/bin" -type f -exec chmod +x {} \; 2>>"$LOG" || true
  chmod +x /mnt/jffs2/autorun.sh 2>>"$LOG" || true
}

make_initial_files() {
  mkdir -p "$SD_ROOT/data" "$SD_ROOT/config" "$SD_ROOT/logs" "$SD_ROOT/tmp" "$DEPLOY_DIR/config"

  if [ ! -f "$SD_ROOT/data/passes.json" ]; then
    cat > "$SD_ROOT/data/passes.json" <<'EOF'
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

  if [ ! -f "$SD_ROOT/data/radio_target.json" ]; then
    cat > "$SD_ROOT/data/radio_target.json" <<'EOF'
{
  "ok": true,
  "state": "idle",
  "message": "No radio target planned."
}
EOF
  fi

  if [ ! -f "$SD_ROOT/data/refresh_status.json" ]; then
    cat > "$SD_ROOT/data/refresh_status.json" <<'EOF'
{
  "ok": true,
  "state": "idle",
  "target": "passes",
  "updated_utc": null,
  "message": "Installed. Waiting for browser time sync and pass refresh."
}
EOF
  fi
}

ensure_observer_json() {
  mkdir -p "$SD_ROOT/config" "$DEPLOY_DIR/config"

  if [ -s "$SD_ROOT/config/observer.json" ]; then
    cp "$SD_ROOT/config/observer.json" "$DEPLOY_DIR/config/observer.json" 2>>"$LOG" || true
    log "PASS: observer.json present in SD config"
    return 0
  fi

  if [ -s "$DEPLOY_DIR/config/observer.json" ]; then
    cp "$DEPLOY_DIR/config/observer.json" "$SD_ROOT/config/observer.json" 2>>"$LOG" || true
    log "PASS: observer.json copied from deploy config"
    return 0
  fi

  cat > "$SD_ROOT/config/observer.json" <<'EOF'
{
  "name": "Cripple Creek",
  "latitude_deg": 38.80063,
  "longitude_deg": -105.2,
  "altitude_m": 2805.0,
  "minimum_elevation_deg": 20.0
}
EOF
  cp "$SD_ROOT/config/observer.json" "$DEPLOY_DIR/config/observer.json" 2>>"$LOG" || true
  log "PASS: created default observer.json in SD and deploy config"
}

clear_stale_refresh_state() {
  log "Clearing stale refresh locks/status..."
  rm -f "$SD_ROOT/data/passes.quick.lock" \
        "$SD_ROOT/data/passes.full.lock" \
        "$SD_ROOT/data/catalog.lock" \
        "$SD_ROOT/data/passes.quick.tmp."* \
        "$SD_ROOT/data/passes.full.tmp."* 2>>"$LOG" || true

  cat > "$SD_ROOT/data/refresh_status.json" <<'EOF'
{
  "ok": true,
  "state": "idle",
  "target": "passes",
  "updated_utc": null,
  "message": "Install reset stale refresh state. Browser time sync will trigger current passes."
}
EOF
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

stop_existing() {
  kill_matching "pluto_refresh_data.sh"
  kill_matching "update_pass_predictions.py"
  kill_matching "python3.11 .*update_pass_predictions"
  kill_matching "pluto_sat_tracker"
  if command -v killall >/dev/null 2>&1; then
    killall pluto_sat_tracker 2>/dev/null || true
    killall pluto_fm_receiver 2>/dev/null || true
    killall iio_readdev 2>/dev/null || true
  fi
  sleep 1
}

log "===== Pluto Satellite Tracker SD install ====="
log "Installer dir: $SCRIPT_DIR"
log "SD mount:      $SD_MOUNT"
log "Runtime root:  $SD_ROOT"
log "Deploy dir:    $DEPLOY_DIR"

need_file "$SCRIPT_DIR/bin/pluto_sat_tracker"
need_file "$SCRIPT_DIR/bin/pluto_fm_receiver"
need_dir "$SCRIPT_DIR/web"
need_dir "$SCRIPT_DIR/tools"
need_file "$SCRIPT_DIR/autorun.sh"

ensure_sd_writable

mkdir -p /mnt/jffs2
mkdir -p "$DEPLOY_DIR" "$DEPLOY_DIR/bin" "$DEPLOY_DIR/web" "$DEPLOY_DIR/tools" "$DEPLOY_DIR/config"
mkdir -p "$SD_ROOT" "$SD_ROOT/data" "$SD_ROOT/config" "$SD_ROOT/tools" "$SD_ROOT/logs" "$SD_ROOT/tmp"

stop_existing

copy_tree_replace "$SCRIPT_DIR/bin" "$DEPLOY_DIR/bin"
copy_tree_replace "$SCRIPT_DIR/web" "$DEPLOY_DIR/web"
copy_tree_replace "$SCRIPT_DIR/tools" "$DEPLOY_DIR/tools"

copy_tree_missing_only "$SCRIPT_DIR/data" "$SD_ROOT/data"
copy_tree_missing_only "$SCRIPT_DIR/config" "$SD_ROOT/config"
copy_tree_replace "$SCRIPT_DIR/tools" "$SD_ROOT/tools"

cp "$SCRIPT_DIR/autorun.sh" /mnt/jffs2/autorun.sh

reset_permissions
make_initial_files
ensure_observer_json
clear_stale_refresh_state

cat > /mnt/jffs2/pluto_sat_tracker.env <<EOF
PLUTO_DEPLOY_DIR="$DEPLOY_DIR"
PLUTO_SD_ROOT="$SD_ROOT"
PLUTO_SD_MOUNT="$SD_MOUNT"
PLUTO_SD_DEV="$SD_DEV"
PLUTO_WEB_DIR="$DEPLOY_DIR/web"
PLUTO_DATA_DIR="$SD_ROOT/data"
PLUTO_CONFIG_DIR="$SD_ROOT/config"
PLUTO_PORT="8080"
PLUTO_BIND_ADDR="0.0.0.0"
EOF

log "PASS: installed /mnt/jffs2/autorun.sh"
log "PASS: wrote /mnt/jffs2/pluto_sat_tracker.env"

if [ "$START_APP" -eq 1 ]; then
  log "Starting app through autorun..."
  /mnt/jffs2/autorun.sh >>"$LOG" 2>&1 || fail "autorun start failed"
fi

sync
log "PASS: SD install complete"
log "Web UI: http://192.168.2.1:8080/SatelliteTracker/"
