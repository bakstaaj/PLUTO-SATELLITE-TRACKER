#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ -f "$ROOT_DIR/.pluto.env" ]]; then
  # shellcheck disable=SC1091
  source "$ROOT_DIR/.pluto.env"
fi

PLUTO_IP="${PLUTO_IP:-192.168.3.1}"
PLUTO_USER="${PLUTO_USER:-root}"
PLUTO_PASS="${PLUTO_PASS:-}"
DEPLOY_DIR="${PLUTO_DEPLOY_DIR:-/mnt/jffs2/pluto_sat_tracker}"
SD_ROOT="${PLUTO_SD_ROOT:-/media/mmcblk0p1/pluto_sat_tracker}"

BIN="$ROOT_DIR/dist/pluto_sat_tracker"
FM_HELPER="$ROOT_DIR/dist/pluto_fm_receiver"
DECODER="$ROOT_DIR/dist/pluto_digital_decoder"
RUNTIME="$ROOT_DIR/tools/pluto_runtime.sh"
WEB_HTML="$ROOT_DIR/web/index.html"
WEB_JS="$ROOT_DIR/web/app.js"
WEB_CSS="$ROOT_DIR/web/app.css"
OBSERVER_CONFIG="$ROOT_DIR/config/observer.example.json"
REPOSITORIES="$ROOT_DIR/data/repositories.json"
SATELLITES="$ROOT_DIR/data/satellites.json"
PASSES="$ROOT_DIR/data/passes.json"
REFRESH_RUNNER="$ROOT_DIR/tools/pluto_refresh_data.sh"
PASS_REFRESH_LOOP="$ROOT_DIR/tools/pluto_pass_refresh_loop.sh"
PASS_UPDATER="$ROOT_DIR/tools/update_pass_predictions.py"
CATALOG_UPDATER="$ROOT_DIR/tools/update_satellite_catalog.py"
REFRESH_STATUS_WRITER="$ROOT_DIR/tools/write_refresh_status.py"
SGP4_PACKAGE="$ROOT_DIR/.python-deps/sgp4"
PYTHON_RUNTIME_TARBALL="${PLUTO_PYTHON_RUNTIME_TARBALL:-$ROOT_DIR/runtime/python-pluto-armhf.tar.gz}"

for required in "$BIN" "$FM_HELPER" "$DECODER" "$RUNTIME" "$WEB_HTML" "$WEB_JS" "$WEB_CSS" "$OBSERVER_CONFIG" "$REPOSITORIES" "$SATELLITES" "$REFRESH_RUNNER" "$PASS_REFRESH_LOOP" "$PASS_UPDATER" "$CATALOG_UPDATER" "$REFRESH_STATUS_WRITER"; do
  if [[ ! -f "$required" ]]; then
    echo "Missing required deploy file: $required"
    exit 1
  fi
done
if [[ ! -d "$SGP4_PACKAGE" ]]; then
  echo "Missing required Python package directory: $SGP4_PACKAGE"
  exit 1
fi

if ! command -v sshpass >/dev/null 2>&1; then
  echo "Missing sshpass. Install it with:"
  echo "  pacman -S --needed sshpass"
  exit 1
fi

if [[ -z "$PLUTO_PASS" ]]; then
  echo "PLUTO_PASS is not set."
  echo "Create $ROOT_DIR/.pluto.env or export PLUTO_PASS before running."
  exit 1
fi

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
SSHPASS=(sshpass -p "$PLUTO_PASS")
DEPLOY_SEED_DATA="${PLUTO_DEPLOY_SEED_DATA:-0}"

echo "== Deploying Pluto Satellite Tracker =="
echo "jffs2:  ${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR} (binaries, web, config, catalog)"
echo "SD:     ${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT} (tools, python — read-only OK)"
echo "Passes/state will be generated at runtime in /tmp/pluto_sat_tracker"
if [[ -f "$PYTHON_RUNTIME_TARBALL" ]]; then
  echo "Python runtime: $PYTHON_RUNTIME_TARBALL"
else
  echo "Python runtime: not staged; refresh will use system python3 if present"
fi

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" "
  set -e
  # jffs2: remove symlinks left by old migrations before mkdir
  # Use unlink (not rm -f) — BusyBox rm refuses to remove symlinks to dirs
  for D in '$DEPLOY_DIR/data' '$DEPLOY_DIR/logs'; do
    if [ -L \"\$D\" ]; then
      echo \"Removing stale symlink: \$D -> \$(readlink \"\$D\")\"
      unlink \"\$D\"
    fi
  done
  mkdir -p '$DEPLOY_DIR/web' '$DEPLOY_DIR/config' '$DEPLOY_DIR/data'
  # SD card: tools and python only (read-only after deploy is fine)
  SD_PARENT=\$(dirname '$SD_ROOT')
  if [ -d \"\$SD_PARENT\" ]; then
    mkdir -p '$SD_ROOT/tools' '$SD_ROOT/python' '$SD_ROOT/cache'
  else
    echo 'SD card not mounted — skipping SD dirs (tools/python will not be available)'
  fi
"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$BIN" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/pluto_sat_tracker.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$FM_HELPER" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/pluto_fm_receiver.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$DECODER" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/pluto_digital_decoder.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$RUNTIME" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/run_tracker.sh.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$WEB_HTML" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/web/index.html.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$WEB_JS" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/web/app.js.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$WEB_CSS" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/web/app.css.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$OBSERVER_CONFIG" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/config/observer.json.tmp"

# Static data goes on jffs2 — always deploy repositories and catalog seed
"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$REPOSITORIES" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/data/repositories.json.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$SATELLITES" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/data/satellites.json.tmp"

# passes.json is generated at runtime in /tmp — never deployed

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$REFRESH_RUNNER" "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/tools/pluto_refresh_data.sh.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$PASS_REFRESH_LOOP" "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/tools/pluto_pass_refresh_loop.sh.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$PASS_UPDATER" "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/tools/update_pass_predictions.py.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$CATALOG_UPDATER" "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/tools/update_satellite_catalog.py.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$REFRESH_STATUS_WRITER" "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/tools/write_refresh_status.py.tmp"

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" "
  set -e
  rm -rf '${SD_ROOT}/python/sgp4.tmp'
  mkdir -p '${SD_ROOT}/python/sgp4.tmp'
"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" "$SGP4_PACKAGE"/*.py \
  "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/python/sgp4.tmp/"

if [[ -f "$PYTHON_RUNTIME_TARBALL" ]]; then
  "${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
    "$PYTHON_RUNTIME_TARBALL" "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/cache/python-runtime.tar.gz.tmp"
fi

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" "
  set -e
  chmod +x '${DEPLOY_DIR}/pluto_sat_tracker.tmp' '${DEPLOY_DIR}/pluto_fm_receiver.tmp' '${DEPLOY_DIR}/pluto_digital_decoder.tmp' '${DEPLOY_DIR}/run_tracker.sh.tmp'
  chmod +x '${SD_ROOT}/tools/pluto_refresh_data.sh.tmp' '${SD_ROOT}/tools/pluto_pass_refresh_loop.sh.tmp'
  mv '${DEPLOY_DIR}/pluto_sat_tracker.tmp' '${DEPLOY_DIR}/pluto_sat_tracker'
  mv '${DEPLOY_DIR}/pluto_fm_receiver.tmp' '${DEPLOY_DIR}/pluto_fm_receiver'
  mv '${DEPLOY_DIR}/pluto_digital_decoder.tmp' '${DEPLOY_DIR}/pluto_digital_decoder'
  mv '${DEPLOY_DIR}/run_tracker.sh.tmp' '${DEPLOY_DIR}/run_tracker.sh'
  mv '${DEPLOY_DIR}/web/index.html.tmp' '${DEPLOY_DIR}/web/index.html'
  mv '${DEPLOY_DIR}/web/app.js.tmp' '${DEPLOY_DIR}/web/app.js'
  mv '${DEPLOY_DIR}/web/app.css.tmp' '${DEPLOY_DIR}/web/app.css'
  if [ -f '${DEPLOY_DIR}/config/observer.json' ]; then
    rm -f '${DEPLOY_DIR}/config/observer.json.tmp'
  else
    mv '${DEPLOY_DIR}/config/observer.json.tmp' '${DEPLOY_DIR}/config/observer.json'
  fi
  mv '${DEPLOY_DIR}/data/repositories.json.tmp' '${DEPLOY_DIR}/data/repositories.json'
  mv '${DEPLOY_DIR}/data/satellites.json.tmp' '${DEPLOY_DIR}/data/satellites.json'
  mv '${SD_ROOT}/tools/pluto_refresh_data.sh.tmp' '${SD_ROOT}/tools/pluto_refresh_data.sh'
  mv '${SD_ROOT}/tools/pluto_pass_refresh_loop.sh.tmp' '${SD_ROOT}/tools/pluto_pass_refresh_loop.sh'
  mv '${SD_ROOT}/tools/update_pass_predictions.py.tmp' '${SD_ROOT}/tools/update_pass_predictions.py'
  mv '${SD_ROOT}/tools/update_satellite_catalog.py.tmp' '${SD_ROOT}/tools/update_satellite_catalog.py'
  mv '${SD_ROOT}/tools/write_refresh_status.py.tmp' '${SD_ROOT}/tools/write_refresh_status.py'
  rm -rf '${SD_ROOT}/python/sgp4'
  mv '${SD_ROOT}/python/sgp4.tmp' '${SD_ROOT}/python/sgp4'
  if [ -f '${SD_ROOT}/cache/python-runtime.tar.gz.tmp' ]; then
    rm -rf '${SD_ROOT}/python-runtime.tmp'
    mkdir -p '${SD_ROOT}/python-runtime.tmp'
    gzip -dc '${SD_ROOT}/cache/python-runtime.tar.gz.tmp' | tar -xf - -C '${SD_ROOT}/python-runtime.tmp'
    if [ ! -x '${SD_ROOT}/python-runtime.tmp/bin/python3' ]; then
      echo 'Python runtime archive must contain bin/python3 at its root.'
      exit 1
    fi
    rm -rf '${SD_ROOT}/python-runtime'
    mv '${SD_ROOT}/python-runtime.tmp' '${SD_ROOT}/python-runtime'
    mv '${SD_ROOT}/cache/python-runtime.tar.gz.tmp' '${SD_ROOT}/cache/python-runtime.tar.gz'
  fi
  sync
  echo '== Remote files (jffs2) =='
  ls -lh '${DEPLOY_DIR}/pluto_sat_tracker' \
         '${DEPLOY_DIR}/pluto_fm_receiver' \
         '${DEPLOY_DIR}/pluto_digital_decoder' \
         '${DEPLOY_DIR}/run_tracker.sh' \
         '${DEPLOY_DIR}/web/index.html' \
         '${DEPLOY_DIR}/config/observer.json' \
         '${DEPLOY_DIR}/data/repositories.json' \
         '${DEPLOY_DIR}/data/satellites.json'
  echo '== Remote files (SD card) =='
  ls -lh '${SD_ROOT}/tools/pluto_refresh_data.sh' \
         '${SD_ROOT}/tools/pluto_pass_refresh_loop.sh' \
         '${SD_ROOT}/tools/update_pass_predictions.py' \
         '${SD_ROOT}/tools/update_satellite_catalog.py' \
         '${SD_ROOT}/tools/write_refresh_status.py' 2>/dev/null || echo '  (SD card not mounted or tools not deployed)'
  if [ -f '${SD_ROOT}/python-runtime/bin/python3.11' ]; then
    '${SD_ROOT}/python-runtime/bin/python3.11' --version 2>/dev/null || \
      /lib/ld-linux-armhf.so.3 '${SD_ROOT}/python-runtime/bin/python3.11' --version || true
  fi
"

echo
echo "Deploy complete. Rebooting Pluto..."
"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" "reboot" 2>/dev/null || true

echo "Waiting for Pluto to come back online..."
sleep 15
for i in $(seq 1 20); do
  if "${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" -o ConnectTimeout=3 "${PLUTO_USER}@${PLUTO_IP}" "echo ok" >/dev/null 2>&1; then
    echo "Pluto is back online. Tracker starting via autorun."
    echo "Browse to: http://${PLUTO_IP}:8080/SatelliteTracker/"
    exit 0
  fi
  echo "  ...still waiting (${i}/20)"
  sleep 5
done
echo "Pluto did not respond after reboot — check manually at http://${PLUTO_IP}:8080/SatelliteTracker/"
