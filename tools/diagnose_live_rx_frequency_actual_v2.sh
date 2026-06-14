#!/usr/bin/env bash
set -u
ROOT="${1:-.}"
HTML="$ROOT/web/index.html"
if [[ ! -f "$HTML" ]]; then echo "FAIL: missing $HTML"; exit 1; fi

python - "$HTML" <<'PY'
from pathlib import Path
import sys

needles = [
    "LIVE_RX_FREQUENCY_ACTUAL_V2",
    "function renderMapPanel",
    "liveRxLabelForMapInfoActualV2",
    "<strong>Altitude</strong>",
    "<strong>Live RX</strong>",
    "lastTrackState && lastTrackState.rx_hz",
    "point && point.rx_hz",
]
lines = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace").splitlines()
for i, line in enumerate(lines, start=1):
    if any(n in line for n in needles):
        print(f"{i}: {line}")
PY
