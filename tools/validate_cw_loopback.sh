#!/usr/bin/env bash
set -euo pipefail

REPO="${REPO:-$HOME/sdrdev/PLUTO-SATELLITE-TRACKER}"
PLUTO_IP="${PLUTO_IP:-192.168.68.104}"
PLUTO_USER="${PLUTO_USER:-root}"
PLUTO_PASS="${PLUTO_PASS:-analog}"
PLUTO_APP_DIR="${PLUTO_APP_DIR:-/mnt/jffs2/pluto_sat_tracker}"
WORKDIR="${WORKDIR:-/media/mmcblk0p1/pluto_test_iq}"
DOCKER_IMAGE="${DOCKER_IMAGE:-pluto-adsb-tracker-cross:v0.39}"

FREQ_HZ="${FREQ_HZ:-435000000}"
OFFSET_HZ="${OFFSET_HZ:-25000}"
BANDWIDTH_HZ="${BANDWIDTH_HZ:-200000}"
RX_CHANNEL="${RX_CHANNEL:-1}"
TX_CHANNEL="${TX_CHANNEL:-1}"
TX_SECONDS="${TX_SECONDS:-24}"
CW_TEXT="${CW_TEXT:-CQ TEST}"
DECODER_CW_WPM="${DECODER_CW_WPM:-8}"
TX_CW_WPM="${TX_CW_WPM:-5}"
CW_TIMING_SCALE="${CW_TIMING_SCALE:-1.38}"
CW_ABS_LOW="${CW_ABS_LOW:-5}"
CW_ABS_HIGH="${CW_ABS_HIGH:-40}"

cd "$REPO"

for cmd in docker cygpath sshpass ssh scp; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "FAIL: required command not found in MSYS2 PATH: $cmd"
    exit 1
  fi
done

if [[ ! -f src/pluto_digital_decoder.c || ! -f src/pluto_test_tx.c ]]; then
  echo "FAIL: source files missing; run from repo root or set REPO"
  exit 1
fi

export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL='*'
repo_win="$(cygpath -w "$PWD")"

echo "BUILD: Docker cross-compile using $DOCKER_IMAGE"
docker run --rm \
  -v "${repo_win}:/work" \
  -w /work \
  "$DOCKER_IMAGE" \
  /bin/sh -lc '
set -e
mkdir -p dist
arm-linux-gnueabihf-gcc -std=c99 -O2 -g -Wall -Wextra -D_POSIX_C_SOURCE=200809L \
  -o dist/pluto_digital_decoder src/pluto_digital_decoder.c -lm
arm-linux-gnueabihf-gcc -std=c99 -O2 -g -Wall -Wextra -D_POSIX_C_SOURCE=200809L \
  -o dist/pluto_test_tx src/pluto_test_tx.c -lm
chmod +x dist/pluto_digital_decoder dist/pluto_test_tx
test -x dist/pluto_digital_decoder
test -x dist/pluto_test_tx
'
echo "PASS: Docker build completed"

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
SCP_OPTS=(-O -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR)
SSHPASS=(sshpass -p "$PLUTO_PASS")
REMOTE="${PLUTO_USER}@${PLUTO_IP}"

sq() {
  local s="$1"
  printf "'%s'" "${s//\'/\'\\\'\'}"
}

echo "DEPLOY: stopping active Pluto IIO users and preparing directories"
"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "$REMOTE" "set +e
pkill -KILL pluto_digital_decoder 2>/dev/null || true
pkill -KILL pluto_test_tx 2>/dev/null || true
pkill -KILL iio_readdev 2>/dev/null || true
pkill -KILL iio_writedev 2>/dev/null || true
for f in /sys/bus/iio/devices/iio:device*/buffer/enable; do
  [ -e \"\$f\" ] || continue
  echo 0 > \"\$f\" 2>/dev/null || true
done
mkdir -p $(sq "$PLUTO_APP_DIR") $(sq "$WORKDIR")
"

deploy_one() {
  local bin="$1"
  local tmp="/tmp/${bin}.$$"
  echo "DEPLOY: uploading $bin with scp -O"
  "${SSHPASS[@]}" scp "${SCP_OPTS[@]}" "dist/$bin" "$REMOTE:$tmp"
  "${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "$REMOTE" "set -e
chmod 700 $(sq "$tmp")
mv -f $(sq "$tmp") $(sq "$PLUTO_APP_DIR")/$bin
chmod 700 $(sq "$PLUTO_APP_DIR")/$bin
test -x $(sq "$PLUTO_APP_DIR")/$bin
"
}

deploy_one pluto_digital_decoder
deploy_one pluto_test_tx
echo "PASS: deployed decoder and TX generator to $PLUTO_APP_DIR"

remote_cmd="/bin/sh -s -- $(sq "$PLUTO_APP_DIR") $(sq "$WORKDIR") $(sq "$FREQ_HZ") $(sq "$OFFSET_HZ") $(sq "$BANDWIDTH_HZ") $(sq "$RX_CHANNEL") $(sq "$TX_CHANNEL") $(sq "$TX_SECONDS") $(sq "$CW_TEXT") $(sq "$DECODER_CW_WPM") $(sq "$TX_CW_WPM") $(sq "$CW_TIMING_SCALE") $(sq "$CW_ABS_LOW") $(sq "$CW_ABS_HIGH")"

echo "VALIDATE: Pluto CW TX/RX loopback"
"${SSHPASS[@]}" ssh "${SSH_OPTS[@]}" "$REMOTE" "$remote_cmd" <<'REMOTE_VALIDATE'
APP_DIR="$1"
WORKDIR="$2"
FREQ_HZ="$3"
OFFSET_HZ="$4"
BANDWIDTH_HZ="$5"
RX_CHANNEL="$6"
TX_CHANNEL="$7"
TX_SECONDS="$8"
CW_TEXT="$9"
DECODER_CW_WPM="${10}"
TX_CW_WPM="${11}"
CW_TIMING_SCALE="${12}"
CW_ABS_LOW="${13}"
CW_ABS_HIGH="${14}"

set -eu
stamp="$(date +%Y%m%d_%H%M%S)"
out="$WORKDIR/cw_loopback_${stamp}.ndjson"
dec_log="$WORKDIR/cw_loopback_decoder_${stamp}.log"
tx_log="$WORKDIR/cw_loopback_tx_${stamp}.log"
summary="$WORKDIR/cw_loopback_summary_${stamp}.txt"

dec_pid=""
cleanup() {
  if [ -n "$dec_pid" ]; then
    kill "$dec_pid" 2>/dev/null || true
    wait "$dec_pid" 2>/dev/null || true
  fi
}
trap cleanup EXIT INT TERM

pkill -KILL pluto_digital_decoder 2>/dev/null || true
pkill -KILL pluto_test_tx 2>/dev/null || true
pkill -KILL iio_readdev 2>/dev/null || true
pkill -KILL iio_writedev 2>/dev/null || true
for f in /sys/bus/iio/devices/iio:device*/buffer/enable; do
  [ -e "$f" ] || continue
  echo 0 > "$f" 2>/dev/null || true
done
mkdir -p "$WORKDIR"

"$APP_DIR/pluto_digital_decoder" \
  --freq-hz "$FREQ_HZ" \
  --mode cw \
  --output "$out" \
  --rx-channel "$RX_CHANNEL" \
  --offset-hz "$OFFSET_HZ" \
  --bandwidth-hz "$BANDWIDTH_HZ" \
  --cw-wpm "$DECODER_CW_WPM" \
  --cw-abs-low "$CW_ABS_LOW" \
  --cw-abs-high "$CW_ABS_HIGH" \
  --debug >"$dec_log" 2>&1 &
dec_pid="$!"

sleep 2

"$APP_DIR/pluto_test_tx" \
  --mode cw \
  --tx-channel "$TX_CHANNEL" \
  --freq-hz "$FREQ_HZ" \
  --offset-hz "$OFFSET_HZ" \
  --seconds "$TX_SECONDS" \
  --cw-text "$CW_TEXT" \
  --cw-wpm "$TX_CW_WPM" \
  --cw-timing-scale "$CW_TIMING_SCALE" \
  --workdir "$WORKDIR" \
  --file-backed >"$tx_log" 2>&1

sleep 3
cleanup
trap - EXIT INT TERM

if [ ! -s "$out" ]; then
  echo "FAIL: decoder produced no CW NDJSON frames"
  echo "decoder_log=$dec_log"
  echo "tx_log=$tx_log"
  echo "--- decoder log tail ---"
  tail -80 "$dec_log" 2>/dev/null || true
  echo "--- tx log tail ---"
  tail -80 "$tx_log" 2>/dev/null || true
  exit 1
fi

decoded="$(sed -n 's/.*"text":"\([^"]*\)".*/\1/p' "$out" | tr -d '\n' | sed 's/[[:space:]][[:space:]]*/ /g; s/^ //; s/ $//')"
expected="$(printf '%s' "$CW_TEXT" | tr '[:lower:]' '[:upper:]' | sed 's/[[:space:]][[:space:]]*/ /g; s/^ //; s/ $//')"

echo "decoded=$decoded" > "$summary"
echo "expected=$expected" >> "$summary"
echo "ndjson=$out" >> "$summary"
echo "decoder_log=$dec_log" >> "$summary"
echo "tx_log=$tx_log" >> "$summary"

if [ "$decoded" = "$expected" ]; then
  echo "PASS: decoded expected CW text: $decoded"
  echo "summary=$summary"
  exit 0
fi

echo "FAIL: decoded CW text did not match expected"
echo "decoded:  $decoded"
echo "expected: $expected"
echo "summary=$summary"
echo "decoder_log=$dec_log"
echo "tx_log=$tx_log"
echo "--- NDJSON ---"
cat "$out" 2>/dev/null || true
echo "--- decoder log tail ---"
tail -120 "$dec_log" 2>/dev/null || true
echo "--- tx log tail ---"
tail -120 "$tx_log" 2>/dev/null || true
exit 1
REMOTE_VALIDATE
