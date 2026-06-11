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

WEB_HTML="$ROOT_DIR/web/index.html"
REPOSITORIES="$ROOT_DIR/data/repositories.json"
SATELLITES="$ROOT_DIR/data/satellites.json"

for required in "$WEB_HTML" "$REPOSITORIES" "$SATELLITES"; do
  if [[ ! -f "$required" ]]; then
    echo "Missing required runtime asset: $required"
    exit 1
  fi
done

if [[ -z "$PLUTO_PASS" ]]; then
  echo "PLUTO_PASS is not set."
  exit 1
fi

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
SSHPASS=(sshpass -p "$PLUTO_PASS")

echo "== Local runtime assets =="
wc -c "$WEB_HTML" "$REPOSITORIES" "$SATELLITES"
echo

echo "== Copying runtime assets to Pluto+ =="
"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" \
  "mkdir -p '$DEPLOY_DIR/web' '$SD_ROOT/data'"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$WEB_HTML" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/web/index.html.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$REPOSITORIES" "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/data/repositories.json.tmp"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$SATELLITES" "${PLUTO_USER}@${PLUTO_IP}:${SD_ROOT}/data/satellites.json.tmp"

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" "
  set -e
  mv '${DEPLOY_DIR}/web/index.html.tmp' '${DEPLOY_DIR}/web/index.html'
  mv '${SD_ROOT}/data/repositories.json.tmp' '${SD_ROOT}/data/repositories.json'
  mv '${SD_ROOT}/data/satellites.json.tmp' '${SD_ROOT}/data/satellites.json'
  sync
  echo '== Remote runtime assets =='
  wc -c '${DEPLOY_DIR}/web/index.html' \
        '${SD_ROOT}/data/repositories.json' \
        '${SD_ROOT}/data/satellites.json'
"

echo
echo "Runtime asset deployment complete."
