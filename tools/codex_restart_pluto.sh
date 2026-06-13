#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$ROOT_DIR"
source "$ROOT_DIR/.pluto.env"

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null)
SSHPASS=(sshpass -p "$PLUTO_PASS")
LOG_FILE="${PLUTO_SD_ROOT}/logs/pluto_sat_tracker.log"

"$ROOT_DIR/tools/stop_on_pluto.sh"

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" \
  "sh -c 'nohup ${PLUTO_DEPLOY_DIR}/run_tracker.sh -- --net >${LOG_FILE} 2>&1 </dev/null &'"

sleep 2

"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "${PLUTO_USER}@${PLUTO_IP}" \
  "pidof pluto_sat_tracker || true; tail -n 20 '${LOG_FILE}' 2>/dev/null || true"
