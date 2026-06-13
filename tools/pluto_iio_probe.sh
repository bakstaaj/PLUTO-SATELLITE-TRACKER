#!/bin/sh
set -eu

FREQ_HZ="${1:-145980000}"
SAMPLES="${2:-1024}"
BUFFER="${3:-256}"
OUT_FILE="/tmp/pluto_iq.bin"
ERR_FILE="/tmp/pluto_iq.err"
LOG_FILE="/tmp/pluto_iio_probe.log"

rm -f "$OUT_FILE" "$ERR_FILE"
exec >"$LOG_FILE" 2>&1

echo "== Pluto IIO probe =="
echo "frequency_hz=$FREQ_HZ samples=$SAMPLES buffer=$BUFFER"

echo "-- configure rx --"
/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 sampling_frequency 2400000 || true
/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 rf_bandwidth 200000 || true
/usr/bin/iio_attr -u local: -c ad9361-phy voltage0 gain_control_mode slow_attack || true
echo "$FREQ_HZ" > /sys/bus/iio/devices/iio:device1/out_altvoltage0_RX_LO_frequency || true

echo "-- read back rx --"
/usr/bin/iio_info -u local: 2>/dev/null | sed -n '/iio:device4: cf-ad9361-lpc/,/No trigger on this device/p'

echo "-- capture --"
if /usr/bin/iio_readdev -u local: -b "$BUFFER" -s "$SAMPLES" cf-ad9361-lpc >"$OUT_FILE" 2>"$ERR_FILE"; then
  echo "capture_exit=0"
else
  echo "capture_exit=$?"
fi

echo "-- results --"
ls -l "$OUT_FILE" "$ERR_FILE" 2>/dev/null || true
wc -c "$OUT_FILE" "$ERR_FILE" 2>/dev/null || true
echo "--- stderr ---"
cat "$ERR_FILE" 2>/dev/null || true
