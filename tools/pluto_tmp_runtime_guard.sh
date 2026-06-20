#!/bin/sh
# Pluto Satellite Tracker startup guard.
# Recreates volatile /tmp runtime prerequisites before the backend starts.
set -eu

APP_ROOT="${APP_ROOT:-/mnt/jffs2/pluto_sat_tracker}"
RUNTIME_ROOT="${RUNTIME_ROOT:-/tmp/pluto_sat_tracker}"
DATA_DIR="${DATA_DIR:-$RUNTIME_ROOT/data}"
LOG_DIR="${LOG_DIR:-$RUNTIME_ROOT/logs}"
TMP_DIR="${TMP_DIR:-$RUNTIME_ROOT/tmp}"
TOOLS_SRC="${TOOLS_SRC:-$APP_ROOT/tools}"
CANONICAL_BIN="${CANONICAL_BIN:-$APP_ROOT/bin/pluto_sat_tracker}"
ROOT_WRAPPER="${ROOT_WRAPPER:-$APP_ROOT/pluto_sat_tracker}"

mkdir -p "$RUNTIME_ROOT" "$DATA_DIR" "$LOG_DIR" "$TMP_DIR"

if [ -d "$TOOLS_SRC" ]; then
  ln -sfn "$TOOLS_SRC" "$RUNTIME_ROOT/tools"
fi

link_first_existing_dir() {
  dst="$1"
  shift
  for src in "$@"; do
    if [ -d "$src" ]; then
      ln -sfn "$src" "$dst"
      return 0
    fi
  done
  return 0
}

link_first_existing_dir "$RUNTIME_ROOT/python" \
  "$APP_ROOT/python" \
  "$APP_ROOT/vendor/python" \
  "/mnt/sd/pluto_sat_tracker/python" \
  "/mnt/sdcard/pluto_sat_tracker/python" \
  "/media/sd/pluto_sat_tracker/python"

link_first_existing_dir "$RUNTIME_ROOT/python-runtime" \
  "$APP_ROOT/python-runtime" \
  "$APP_ROOT/vendor/python-runtime" \
  "/mnt/sd/pluto_sat_tracker/python-runtime" \
  "/mnt/sdcard/pluto_sat_tracker/python-runtime" \
  "/media/sd/pluto_sat_tracker/python-runtime"

if [ ! -s "$DATA_DIR/satellites.json" ]; then
  for src in \
    "$APP_ROOT/data/satellites.json" \
    "$APP_ROOT/config/satellites.json" \
    "$APP_ROOT/tools/satellites.json" \
    "/mnt/sd/pluto_sat_tracker/data/satellites.json" \
    "/mnt/sdcard/pluto_sat_tracker/data/satellites.json" \
    "/media/sd/pluto_sat_tracker/data/satellites.json"; do
    if [ -s "$src" ]; then
      cp "$src" "$DATA_DIR/satellites.json"
      break
    fi
  done
fi

if [ -x "$CANONICAL_BIN" ]; then
  if [ ! -f "$ROOT_WRAPPER" ] || ! grep -q "$CANONICAL_BIN" "$ROOT_WRAPPER" 2>/dev/null; then
    if [ -e "$ROOT_WRAPPER" ]; then
      cp "$ROOT_WRAPPER" "$ROOT_WRAPPER.backup_tmp_runtime_guard_$(date +%Y%m%d_%H%M%S)" 2>/dev/null || true
    fi
    cat > "$ROOT_WRAPPER" <<WRAPPER_EOF
#!/bin/sh
exec $CANONICAL_BIN "\$@"
WRAPPER_EOF
    chmod +x "$ROOT_WRAPPER"
  fi
fi

exit 0
