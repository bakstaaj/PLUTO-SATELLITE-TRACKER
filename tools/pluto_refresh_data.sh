#!/bin/sh

DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}"
DATA_DIR="${SD_ROOT}/data"
TOOLS_DIR="${SD_ROOT}/tools"
PYTHON_DIR="${SD_ROOT}/python"
PYTHON_RUNTIME_DIR="${SD_ROOT}/python-runtime"
STATUS_FILE="${DATA_DIR}/refresh_status.json"

MODE="${1:-passes}"

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

run_python() {
  SCRIPT="$1"
  shift
  PYTHON_BIN="${PLUTO_PYTHON:-}"

  if [ -n "$PYTHON_BIN" ] && [ ! -x "$PYTHON_BIN" ]; then
    fail "configured Python runtime is not executable: $PYTHON_BIN"
  fi
  if [ -z "$PYTHON_BIN" ] && [ -x "${PYTHON_RUNTIME_DIR}/bin/python3" ]; then
    PYTHON_BIN="${PYTHON_RUNTIME_DIR}/bin/python3"
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

  if [ "$PYTHON_BIN" = "${PYTHON_RUNTIME_DIR}/bin/python3" ]; then
    export PYTHONHOME="${PYTHON_RUNTIME_DIR}"
    export LD_LIBRARY_PATH="${PYTHON_RUNTIME_DIR}/lib:${PYTHON_RUNTIME_DIR}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
  fi

  PYTHONPATH="${PYTHON_DIR}${PYTHONPATH:+:${PYTHONPATH}}" "$PYTHON_BIN" "$SCRIPT" "$@"
}

mkdir -p "$DATA_DIR" "$TOOLS_DIR" "$PYTHON_DIR"

case "$MODE" in
  passes)
    write_status "running" "passes" "Regenerating pass predictions on Pluto"
    run_python \
      "${TOOLS_DIR}/update_pass_predictions.py" \
      --catalog "${DATA_DIR}/satellites.json" \
      --observer "${DEPLOY_DIR}/config/observer.json" \
      --output "${DATA_DIR}/passes.json"
    write_status "ok" "passes" "Pass predictions regenerated on Pluto"
    ;;
  catalog)
    write_status "running" "catalog" "Refreshing CelesTrak and SatNOGS catalog data on Pluto"
    run_python \
      "${TOOLS_DIR}/update_satellite_catalog.py" \
      --output "${DATA_DIR}/satellites.json"
    write_status "ok" "catalog" "Catalog and TLE data refreshed on Pluto"
    ;;
  all)
    "$0" catalog
    "$0" passes
    ;;
  *)
    fail "unknown refresh target: $MODE"
    ;;
esac
