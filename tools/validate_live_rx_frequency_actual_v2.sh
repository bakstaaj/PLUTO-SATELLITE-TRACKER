#!/usr/bin/env bash
set -u

ROOT="${1:-.}"
HTML="$ROOT/web/index.html"

if [[ ! -f "$HTML" ]]; then
  echo "FAIL: missing $HTML"
  exit 1
fi

python - "$HTML" <<'PY'
import sys
from pathlib import Path

text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")

altitude_row = '<div><strong>Altitude</strong>${escapeHtml(`${livePoint.altitude_km} km`)}</div>'
live_rx_row = '<div><strong>Live RX</strong>${escapeHtml(liveRxLabelForMapInfoActualV2(livePoint))}</div>'

alt_pos = text.find(altitude_row)
rx_pos = text.find(live_rx_row)

checks = [
    ("LIVE_RX_FREQUENCY_ACTUAL_V2 marker present", "LIVE_RX_FREQUENCY_ACTUAL_V2" in text),
    ("actual renderMapPanel still present", "function renderMapPanel(pass, config)" in text),
    ("live RX helper present", "function liveRxLabelForMapInfoActualV2(point)" in text),
    ("helper uses track state first", "lastTrackState && lastTrackState.rx_hz" in text),
    ("helper falls back to live point", "point && point.rx_hz" in text),
    ("helper formats Hz", "formatHz(rxHz)" in text),
    ("exact livePoint Altitude row present", alt_pos >= 0),
    ("Live RX row present", rx_pos >= 0),
    ("Live RX row is after Altitude row", alt_pos >= 0 and rx_pos > alt_pos),
    ("Live RX row is immediately near Altitude row", alt_pos >= 0 and rx_pos > alt_pos and (rx_pos - alt_pos) < 250),
]
failed = False
for name, ok in checks:
    print(("PASS: " if ok else "FAIL: ") + name)
    failed = failed or not ok

print("Validation failed." if failed else "Validation passed.")
sys.exit(1 if failed else 0)
PY
