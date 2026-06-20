#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="repair_pluto_tmp_runtime_v2_8_1"
ROOT="${ROOT:-$HOME/sdrdev/PLUTO-SATELLITE-TRACKER}"
PLUTO_HOST="${PLUTO_HOST:-192.168.68.104}"
PLUTO_USER="${PLUTO_USER:-root}"
TARGET="${PLUTO_USER}@${PLUTO_HOST}"
API="http://${PLUTO_HOST}:8080"
TS="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="$ROOT/patch_logs"
LOG="$LOG_DIR/${SCRIPT_NAME}_${TS}.log"
mkdir -p "$LOG_DIR"

fail() { echo "RESULT: FAIL - $*" | tee -a "$LOG"; exit 1; }
pass() { echo "CHECK: PASS $*" | tee -a "$LOG"; }
info() { echo "$*" | tee -a "$LOG"; }
need_cmd() { command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"; }

info "== Pluto /tmp Runtime Repair v2.8.1 =="
info "Repo:   $ROOT"
info "Target: $TARGET"
info "API:    $API"
info "Log:    $LOG"
info "Scope:  install startup guard, patch Pluto startup hooks, validate runtime"

need_cmd sshpass
need_cmd ssh
need_cmd scp
need_cmd curl
need_cmd grep
need_cmd wc
need_cmd tr

[[ -f "$ROOT/tools/pluto_tmp_runtime_guard.sh" ]] || fail "missing $ROOT/tools/pluto_tmp_runtime_guard.sh"

PLUTO_PASS="${PLUTO_PASS:-}"
if [[ -z "$PLUTO_PASS" && -f "$ROOT/.pluto.env" ]]; then
  set +u
  # shellcheck disable=SC1091
  source "$ROOT/.pluto.env" || true
  set -u
  PLUTO_PASS="${PLUTO_PASS:-${PLUTO_PASSWORD:-${PLUTO_ROOT_PASSWORD:-}}}"
fi
PLUTO_PASS="${PLUTO_PASS:-analog}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8)
ssh_pluto() { sshpass -p "$PLUTO_PASS" ssh "${SSH_OPTS[@]}" "$TARGET" "$@"; }
scp_to_pluto() { sshpass -p "$PLUTO_PASS" scp -O "${SSH_OPTS[@]}" "$1" "$TARGET:$2"; }

ssh_pluto "echo connected" >>"$LOG" 2>&1 || fail "cannot SSH to Pluto at $TARGET"
pass "SSH connection to Pluto works"

ssh_pluto "mkdir -p /mnt/jffs2/pluto_sat_tracker/tools /tmp/pluto_sat_tracker" >>"$LOG" 2>&1 || fail "could not create Pluto runtime/tool directories"
scp_to_pluto "$ROOT/tools/pluto_tmp_runtime_guard.sh" "/mnt/jffs2/pluto_sat_tracker/tools/pluto_tmp_runtime_guard.sh.tmp" >>"$LOG" 2>&1 || fail "could not copy runtime guard to Pluto"
ssh_pluto "mv /mnt/jffs2/pluto_sat_tracker/tools/pluto_tmp_runtime_guard.sh.tmp /mnt/jffs2/pluto_sat_tracker/tools/pluto_tmp_runtime_guard.sh && chmod +x /mnt/jffs2/pluto_sat_tracker/tools/pluto_tmp_runtime_guard.sh && /bin/sh -n /mnt/jffs2/pluto_sat_tracker/tools/pluto_tmp_runtime_guard.sh" >>"$LOG" 2>&1 || fail "installed guard failed Pluto shell validation"
pass "runtime guard installed and shell-validated on Pluto"

ssh_pluto 'sh -s' <<'REMOTE_PATCH' >>"$LOG" 2>&1 || fail "could not patch Pluto startup hooks"
set -eu
TS="$(date +%Y%m%d_%H%M%S)"
GUARD="/mnt/jffs2/pluto_sat_tracker/tools/pluto_tmp_runtime_guard.sh"
CALL='[ -x /mnt/jffs2/pluto_sat_tracker/tools/pluto_tmp_runtime_guard.sh ] && /mnt/jffs2/pluto_sat_tracker/tools/pluto_tmp_runtime_guard.sh || true'
insert_guard_call() {
  f="$1"
  [ -f "$f" ] || return 0
  if grep -q 'pluto_tmp_runtime_guard.sh' "$f" 2>/dev/null; then
    /bin/sh -n "$f" 2>/dev/null || true
    return 0
  fi
  cp "$f" "$f.backup_tmp_runtime_guard_$TS"
  tmp="$f.tmp_runtime_guard_$TS"
  first=""
  if IFS= read -r first < "$f"; then
    :
  fi
  if printf '%s\n' "$first" | grep -q '^#!'; then
    {
      printf '%s\n' "$first"
      printf '%s\n' "$CALL"
      tail -n +2 "$f"
    } > "$tmp"
  else
    {
      printf '%s\n' "$CALL"
      cat "$f"
    } > "$tmp"
  fi
  chmod +x "$tmp"
  /bin/sh -n "$tmp" 2>/dev/null || true
  mv "$tmp" "$f"
}

[ -x "$GUARD" ] || exit 10
insert_guard_call /mnt/jffs2/pluto_sat_tracker/run_tracker.sh
insert_guard_call /mnt/jffs2/autorun.sh
"$GUARD"

[ -d /tmp/pluto_sat_tracker/data ] || exit 20
[ -d /tmp/pluto_sat_tracker/logs ] || exit 21
[ -d /tmp/pluto_sat_tracker/tmp ] || exit 22
[ -L /tmp/pluto_sat_tracker/tools ] || exit 23
[ "$(readlink /tmp/pluto_sat_tracker/tools)" = "/mnt/jffs2/pluto_sat_tracker/tools" ] || exit 24
[ -x /mnt/jffs2/pluto_sat_tracker/bin/pluto_sat_tracker ] || exit 25
[ -f /mnt/jffs2/pluto_sat_tracker/pluto_sat_tracker ] || exit 26
grep -q '/mnt/jffs2/pluto_sat_tracker/bin/pluto_sat_tracker' /mnt/jffs2/pluto_sat_tracker/pluto_sat_tracker || exit 27
/bin/sh -n /mnt/jffs2/pluto_sat_tracker/pluto_sat_tracker
REMOTE_PATCH
pass "Pluto startup hooks contain the runtime guard and runtime prerequisites exist"

STATUS_JSON="$(curl -fsS "$API/api/status" || true)"
[[ -n "$STATUS_JSON" ]] || fail "api/status did not respond"
echo "$STATUS_JSON" >>"$LOG"
echo "$STATUS_JSON" | grep -Eq '"ok"[[:space:]]*:[[:space:]]*true' || fail "api/status did not report ok=true"
echo "$STATUS_JSON" | grep -Eq '"data_dir"[[:space:]]*:[[:space:]]*"/tmp/pluto_sat_tracker/data"' || fail "api/status did not report /tmp data_dir"
if echo "$STATUS_JSON" | grep -q '"version"[[:space:]]*:[[:space:]]*"0\.1\.0"'; then
  fail "backend is still reporting stale version 0.1.0"
fi
pass "backend API reports healthy non-0.1.0 runtime using /tmp data_dir"

TMP_REFRESH="$LOG_DIR/${SCRIPT_NAME}_${TS}_refresh_response.json"
HTTP_CODE="$(curl -sS -o "$TMP_REFRESH" -w '%{http_code}' -X POST "$API/api/refresh/passes" || true)"
[[ "$HTTP_CODE" == "200" ]] || fail "api/refresh/passes returned HTTP $HTTP_CODE"
pass "api/refresh/passes accepted refresh request"

STATE_OK="no"
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
  REFRESH_STATUS="$(curl -sS "$API/api/refresh/status" || true)"
  echo "$REFRESH_STATUS" >>"$LOG"
  if echo "$REFRESH_STATUS" | grep -Eq '"state"[[:space:]]*:[[:space:]]*"ok"'; then
    STATE_OK="yes"
    break
  fi
  if echo "$REFRESH_STATUS" | grep -Eq '"state"[[:space:]]*:[[:space:]]*"error"'; then
    fail "pass refresh worker reported error; see $LOG"
  fi
  sleep 2
done
[[ "$STATE_OK" == "yes" ]] || fail "pass refresh worker did not reach ok state"
pass "pass refresh worker reached ok state"

PASS_BYTES="$(curl -sS "$API/api/passes" | wc -c | tr -d ' ')"
[[ "$PASS_BYTES" =~ ^[0-9]+$ ]] || fail "could not measure /api/passes size"
if (( PASS_BYTES < 1000 )); then
  fail "/api/passes is too small: ${PASS_BYTES} bytes"
fi
pass "/api/passes payload is non-trivial: ${PASS_BYTES} bytes"

echo "RESULT: PASS - runtime guard is installed, startup hooks are patched, and live Pluto refresh/passes validation succeeded" | tee -a "$LOG"
