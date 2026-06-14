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

checks = [
    ("REMOVE_STALE_CURRENT_LOOK_ANGLE_V1C marker present", "REMOVE_STALE_CURRENT_LOOK_ANGLE_V1C" in text),
    ("skyLiveReadout id removed", 'id="skyLiveReadout"' not in text),
    ("Current look angle text removed", "Current look angle" not in text),
    ("dynamic map-info v1b still present", "DYNAMIC_MAP_INFO_REMOVE_SKY_READOUT_V1B" in text),
    ("map info still uses livePoint", "formatDateTime(livePoint.time_utc)" in text and "livePoint.azimuth_deg" in text),
    ("Listen panel still present", 'class="listen-panel"' in text and 'id="analogAudioToggleButton"' in text),
]
failed = False
for name, ok in checks:
    print(("PASS: " if ok else "FAIL: ") + name)
    failed = failed or not ok

if failed:
    print("Validation failed.")
else:
    print("Validation passed.")
sys.exit(1 if failed else 0)
PY
