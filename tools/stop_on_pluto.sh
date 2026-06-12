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

if [[ -z "$PLUTO_PASS" ]]; then
  echo "PLUTO_PASS is not set."
  exit 1
fi

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
SSHPASS=(sshpass -p "$PLUTO_PASS")

echo "Stopping Pluto Satellite Tracker on ${PLUTO_IP}..."

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" '
  PIDS="$(pidof pluto_sat_tracker 2>/dev/null || true)"
  if [ -z "$PIDS" ]; then
    PIDS="$(ps | awk "/pluto_sat_tracker/ && !/awk/ { print \$1 }")"
  fi
  if [ -z "$PIDS" ]; then
    echo "No pluto_sat_tracker process found."
    exit 0
  fi
  echo "Killing: $PIDS"
  kill $PIDS
  sleep 1

  LEFTOVERS="$(pidof pluto_sat_tracker 2>/dev/null || true)"
  if [ -z "$LEFTOVERS" ]; then
    LEFTOVERS="$(ps | awk "/pluto_sat_tracker/ && !/awk/ { print \$1 }")"
  fi
  if [ -n "$LEFTOVERS" ]; then
    echo "Force killing: $LEFTOVERS"
    kill -9 $LEFTOVERS
  fi
'
