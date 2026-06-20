#!/usr/bin/env bash
set -euo pipefail

# Repair Pluto Satellite Tracker runtime layout for v2.8.x.
# Creates a persistent-code + /tmp-transactional-data layout and restarts backend.
# Intended host environment: MSYS2/UCRT64 or Linux with sshpass/scp/ssh/curl.

ROOT="${ROOT:-$HOME/sdrdev/PLUTO-SATELLITE-TRACKER}"
PLUTO_HOST="${PLUTO_HOST:-192.168.68.104}"
PLUTO_USER="${PLUTO_USER:-root}"
PLUTO_PASS="${PLUTO_PASS:-}"
APP="${APP:-/mnt/jffs2/pluto_sat_tracker}"
TMP_ROOT="${TMP_ROOT:-/tmp/pluto_sat_tracker}"
API="http://${PLUTO_HOST}:8080"

if [[ -z "$PLUTO_PASS" && -f "$ROOT/.pluto.env" ]]; then
  set +u
  # shellcheck disable=SC1090
  source "$ROOT/.pluto.env" || true
  set -u
  PLUTO_PASS="${PLUTO_PASS:-${PLUTO_PASSWORD:-${PLUTO_ROOT_PASSWORD:-}}}"
fi
PLUTO_PASS="${PLUTO_PASS:-analog}"
TARGET="${PLUTO_USER}@${PLUTO_HOST}"
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=8)
ssh_pluto() { sshpass -p "$PLUTO_PASS" ssh "${SSH_OPTS[@]}" "$TARGET" "$@"; }

fail() { echo "RESULT: FAIL - $*"; exit 1; }
pass() { echo "CHECK: PASS $*"; }
warn() { echo "CHECK: WARN $*"; }
need_cmd() { command -v "$1" >/dev/null 2>&1 || fail "missing required command: $1"; }
need_cmd sshpass
need_cmd ssh
need_cmd curl

cat <<INFO
== Repair Pluto tmp runtime layout (v2.8) ==
Target: $TARGET
App: $APP
Tmp runtime: $TMP_ROOT
API: $API
INFO

ssh_pluto "APP='$APP' TMP_ROOT='$TMP_ROOT' sh -s" <<'REMOTE'
set -eu
mkdir -p "$TMP_ROOT/data" "$TMP_ROOT/logs" "$TMP_ROOT/tmp"
chmod 755 "$TMP_ROOT" "$TMP_ROOT/data" "$TMP_ROOT/logs" "$TMP_ROOT/tmp" 2>/dev/null || true

rm -rf "$TMP_ROOT/tools"
ln -s "$APP/tools" "$TMP_ROOT/tools"

# Link optional bundled Python/runtime folders from persistent or SD locations.
for name in python python-runtime; do
  rm -rf "$TMP_ROOT/$name"
  for src in "$APP/$name" "/media/mmcblk0p1/pluto_sat_tracker/$name"; do
    if [ -e "$src" ]; then
      ln -s "$src" "$TMP_ROOT/$name"
      break
    fi
  done
done

# Seed runtime data with the best available catalog and pass cache.
seed_best() {
  file="$1"
  best=""
  best_size=0
  for src in \
    "$TMP_ROOT/data/$file" \
    "$APP/data/$file" \
    "/media/mmcblk0p1/pluto_sat_tracker/data/$file"; do
    if [ -s "$src" ]; then
      size=$(wc -c < "$src" 2>/dev/null || echo 0)
      if [ "$size" -gt "$best_size" ]; then
        best="$src"
        best_size="$size"
      fi
    fi
  done
  if [ -n "$best" ]; then
    cp "$best" "$TMP_ROOT/data/$file.tmp.$$"
    mv "$TMP_ROOT/data/$file.tmp.$$" "$TMP_ROOT/data/$file"
    echo "SEEDED $file from $best size=$best_size"
  else
    echo "WARN no source found for $file"
  fi
}
seed_best satellites.json
seed_best passes.json

# Ensure refresh status exists.
if [ ! -s "$TMP_ROOT/data/refresh_status.json" ]; then
  cat > "$TMP_ROOT/data/refresh_status.json" <<EOF_STATUS
{"ok":true,"state":"idle","target":"passes","updated_utc":"$(date -u +%Y-%m-%dT%H:%M:%SZ)","message":"Runtime initialized on /tmp"}
EOF_STATUS
fi

# Write deterministic startup wrapper.
cat > "$APP/run_tracker.sh.tmp.$$" <<'EOF_RUN'
#!/bin/sh
set -eu
APP=/mnt/jffs2/pluto_sat_tracker
TMP_ROOT=/tmp/pluto_sat_tracker
mkdir -p "$TMP_ROOT/data" "$TMP_ROOT/logs" "$TMP_ROOT/tmp"
chmod 755 "$TMP_ROOT" "$TMP_ROOT/data" "$TMP_ROOT/logs" "$TMP_ROOT/tmp" 2>/dev/null || true
rm -rf "$TMP_ROOT/tools"
ln -s "$APP/tools" "$TMP_ROOT/tools"
for name in python python-runtime; do
  rm -rf "$TMP_ROOT/$name"
  for src in "$APP/$name" "/media/mmcblk0p1/pluto_sat_tracker/$name"; do
    if [ -e "$src" ]; then ln -s "$src" "$TMP_ROOT/$name"; break; fi
  done
done
if [ ! -s "$TMP_ROOT/data/satellites.json" ]; then
  for src in "$APP/data/satellites.json" "/media/mmcblk0p1/pluto_sat_tracker/data/satellites.json"; do
    if [ -s "$src" ]; then cp "$src" "$TMP_ROOT/data/satellites.json"; break; fi
  done
fi
while [ "$#" -gt 0 ]; do
  case "$1" in
    --host-time-utc)
      shift
      if [ "$#" -gt 0 ]; then date -u -s "$1" >/dev/null 2>&1 || true; fi
      ;;
    --) shift; break ;;
  esac
  [ "$#" -gt 0 ] && shift || true
done
BIN="$APP/bin/pluto_sat_tracker"
if [ ! -x "$BIN" ]; then BIN="$APP/pluto_sat_tracker"; fi
echo "== Pluto Satellite Tracker runtime =="
echo "Deploy dir: $APP"
echo "Transactional data dir: $TMP_ROOT/data"
echo "Refresh runner: $TMP_ROOT/tools/pluto_refresh_data.sh"
exec "$BIN" --web-dir "$APP/web" --config-dir "$APP/config" --data-dir "$TMP_ROOT/data" --net
EOF_RUN
chmod +x "$APP/run_tracker.sh.tmp.$$"
mv "$APP/run_tracker.sh.tmp.$$" "$APP/run_tracker.sh"

# Stop stale backend and refresh loops, then restart.
for p in $(pidof pluto_sat_tracker 2>/dev/null || true); do kill "$p" 2>/dev/null || true; done
for p in $(ps w | awk '/[p]luto_pass_refresh_loop/ {print $1}'); do kill "$p" 2>/dev/null || true; done
sleep 2
for p in $(pidof pluto_sat_tracker 2>/dev/null || true); do kill -9 "$p" 2>/dev/null || true; done
nohup "$APP/run_tracker.sh" > "$TMP_ROOT/logs/backend_start_v2_8.log" 2>&1 &
sleep 4
ps w | grep -E '[p]luto_sat_tracker|[p]luto_pass_refresh_loop' || true
REMOTE

STATUS="$(curl -fsS --max-time 8 "$API/api/status" || true)"
[ -n "$STATUS" ] || fail "backend did not answer /api/status"
echo "STATUS $STATUS"
echo "$STATUS" | grep -q '"data_dir":"/tmp/pluto_sat_tracker/data"' && pass "backend reports /tmp transactional data_dir" || warn "backend data_dir is not /tmp"
echo "$STATUS" | grep -q '"version":"0.1.0"' && fail "backend is stale 0.1.0" || pass "backend is not stale 0.1.0"

HTTP_CODE="$(curl -sS --max-time 30 -X POST -o /tmp/pluto_v2_8_refresh_body.$$ -w '%{http_code}' "$API/api/refresh/passes" || true)"
echo "HTTP_CODE_REFRESH $HTTP_CODE"
cat /tmp/pluto_v2_8_refresh_body.$$ 2>/dev/null || true
rm -f /tmp/pluto_v2_8_refresh_body.$$
[ "$HTTP_CODE" = "200" ] || fail "/api/refresh/passes returned HTTP $HTTP_CODE"

sleep 4
PASSES_BYTES="$(curl -sS --max-time 8 "$API/api/passes" | wc -c | tr -d ' ' || echo 0)"
echo "PASSES_BYTES $PASSES_BYTES"
[ "${PASSES_BYTES:-0}" -gt 1000 ] && pass "/api/passes is non-trivial" || warn "/api/passes is still small after queued refresh"
echo "RESULT: PASS - Pluto tmp runtime layout repaired"
