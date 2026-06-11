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

HOST_UTC="$(date -u '+%Y.%m.%d-%H:%M:%S')"

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
SSHPASS=(sshpass -p "$PLUTO_PASS")

echo "Starting Pluto Satellite Tracker on ${PLUTO_IP}..."
echo "Passing host UTC time: ${HOST_UTC}"
echo "Browser URL after startup: http://${PLUTO_IP}:8080/SatelliteTracker/"
echo "Press Ctrl-C to stop."
echo

"${SSHPASS[@]}" ssh -tt "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" \
  "'${DEPLOY_DIR}/run_tracker.sh' --host-time-utc '${HOST_UTC}' -- --interactive --net"
