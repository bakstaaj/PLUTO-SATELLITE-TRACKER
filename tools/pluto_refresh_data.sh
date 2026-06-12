#!/bin/sh

DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}"
DATA_DIR="${SD_ROOT}/data"
TOOLS_DIR="${SD_ROOT}/tools"
PYTHON_DIR="${SD_ROOT}/python"
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

  if ! command -v python3 >/dev/null 2>&1; then
    fail "python3 is not installed on this Pluto image"
  fi
  if [ ! -f "$SCRIPT" ]; then
    fail "refresh script not found: $SCRIPT"
  fi

  PYTHONPATH="${PYTHON_DIR}${PYTHONPATH:+:${PYTHONPATH}}" python3 "$SCRIPT" "$@"
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
