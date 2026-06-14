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

map_info_pos = text.find('<div class="map-info">')
region = ""
if map_info_pos >= 0:
    start = max(0, text.rfind("${", 0, map_info_pos))
    end = text.find('` : ""}', map_info_pos)
    if end >= 0:
        region = text[start:end + len('` : ""}')]

checks = [
    ("DYNAMIC_MAP_INFO_REMOVE_SKY_READOUT_V1B marker present", "DYNAMIC_MAP_INFO_REMOVE_SKY_READOUT_V1B" in text),
    ("sky current look angle readout removed", 'id="skyLiveReadout"' not in text and "Current look angle" not in text),
    ("Listen panel remains present", 'class="listen-panel"' in text and 'id="analogAudioToggleButton"' in text),
    ("map info block still present", '<div class="map-info">' in text),
    ("map info now uses livePoint conditional", '${livePoint ? `' in region),
    ("map info Sample uses livePoint", "formatDateTime(livePoint.time_utc)" in region),
    ("map info Ground Point uses livePoint", "Ground Point" in region and "livePoint.latitude_deg" in region and "livePoint.longitude_deg" in region),
    ("map info Look Angles uses livePoint", "Look Angles" in region and "livePoint.azimuth_deg" in region and "livePoint.elevation_deg" in region),
    ("map info Altitude uses livePoint", "livePoint.altitude_km" in region),
    ("map info no longer displays focusPoint", "focusPoint." not in region and "${focusPoint ? `" not in region),
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
