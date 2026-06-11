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
RUNTIME="$ROOT_DIR/tools/pluto_runtime.sh"
WEB_HTML="$ROOT_DIR/web/index.html"
OBSERVER_CONFIG="$ROOT_DIR/config/observer.example.json"
REPOSITORIES="$ROOT_DIR/data/repositories.json"
SATELLITES="$ROOT_DIR/data/satellites.json"

for required in "$BIN" "$RUNTIME" "$WEB_HTML" "$OBSERVER_CONFIG" "$REPOSITORIES" "$SATELLITES"; do
  if [[ ! -f "$required" ]]; then
    echo "Missing required deploy file: $required"
    exit 1
  fi
done

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

echo "== Deploying Pluto Satellite Tracker =="
echo "Runtime: ${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}"
echo "SD data: ${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}"

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" "
  set -e
  SD_PARENT=\$(dirname '$SD_ROOT')
  if [ ! -d \"\$SD_PARENT\" ]; then
    echo \"SD mount parent not found: \$SD_PARENT\"
    exit 1
  fi
  mkdir -p '$DEPLOY_DIR/web' '$DEPLOY_DIR/config' '$SD_ROOT/data' '$SD_ROOT/cache' '$SD_ROOT/logs'
"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$BIN" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/pluto_sat_tracker.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$RUNTIME" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/run_tracker.sh.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$WEB_HTML" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/web/index.html.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$OBSERVER_CONFIG" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/config/observer.json.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$REPOSITORIES" "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/data/repositories.json.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$SATELLITES" "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/data/satellites.json.tmp"

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" "
  set -e
  chmod +x '${DEPLOY_DIR}/pluto_sat_tracker.tmp' '${DEPLOY_DIR}/run_tracker.sh.tmp'
  mv '${DEPLOY_DIR}/pluto_sat_tracker.tmp' '${DEPLOY_DIR}/pluto_sat_tracker'
  mv '${DEPLOY_DIR}/run_tracker.sh.tmp' '${DEPLOY_DIR}/run_tracker.sh'
  mv '${DEPLOY_DIR}/web/index.html.tmp' '${DEPLOY_DIR}/web/index.html'
  mv '${DEPLOY_DIR}/config/observer.json.tmp' '${DEPLOY_DIR}/config/observer.json'
  mv '${SD_ROOT}/data/repositories.json.tmp' '${SD_ROOT}/data/repositories.json'
  mv '${SD_ROOT}/data/satellites.json.tmp' '${SD_ROOT}/data/satellites.json'
  sync
  echo '== Remote files =='
  ls -lh '${DEPLOY_DIR}/pluto_sat_tracker' \
         '${DEPLOY_DIR}/run_tracker.sh' \
         '${DEPLOY_DIR}/web/index.html' \
         '${DEPLOY_DIR}/config/observer.json' \
         '${SD_ROOT}/data/repositories.json' \
         '${SD_ROOT}/data/satellites.json'
"

echo
echo "Deploy complete."
