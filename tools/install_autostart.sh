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

AUTORUN_SCRIPT="$ROOT_DIR/tools/pluto_autorun.sh"

for required in "$AUTORUN_SCRIPT"; do
  if [[ ! -f "$required" ]]; then
    echo "Missing required autostart file: $required"
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

echo "== Installing Pluto Satellite Tracker autostart =="
echo "Target: ${PLUTO_USER}@${PLUTO_IP}"

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" \
  "mkdir -p '$DEPLOY_DIR'"

"${SSHPASS[@]}" scp -O "${SSH_OPTS[@]}" \
  "$AUTORUN_SCRIPT" "${PLUTO_USER}@${PLUTO_IP}:${DEPLOY_DIR}/autorun.sh.tmp"

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" "
  set -e
  mv '${DEPLOY_DIR}/autorun.sh.tmp' '${DEPLOY_DIR}/autorun.sh'
  chmod 755 '${DEPLOY_DIR}/autorun.sh'

  if [ -f /mnt/jffs2/autorun.sh ] && ! grep -q 'pluto_sat_tracker' /mnt/jffs2/autorun.sh 2>/dev/null; then
    cp /mnt/jffs2/autorun.sh /mnt/jffs2/autorun.sh.before-pluto-sat-tracker
  fi

  cp '${DEPLOY_DIR}/autorun.sh' /mnt/jffs2/autorun.sh
  chmod 755 /mnt/jffs2/autorun.sh
  /bin/sh /mnt/jffs2/autorun.sh
  echo 'Autostart mode: /mnt/jffs2/autorun.sh'
  sync
"

echo
echo "Autostart install complete."
