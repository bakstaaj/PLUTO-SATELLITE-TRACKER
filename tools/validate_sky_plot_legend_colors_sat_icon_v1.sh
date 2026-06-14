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
    ("SKY_PLOT_LEGEND_COLORS_SAT_ICON_V1 marker present", "SKY_PLOT_LEGEND_COLORS_SAT_ICON_V1" in text),
    ("sky path class present", "sky-pass-path" in text),
    ("sky progress class present", "sky-pass-progress" in text),
    ("sky look line class present", "sky-look-line" in text),
    ("sky focus dot class present", "sky-focus-dot" in text),
    ("sky satellite helper present", "function renderSkySatelliteIcon(point)" in text),
    ("sky satellite icon used", "renderSkySatelliteIcon(liveSky)" in text),
    ("satellite icon svg classes present", "satellite-body" in text and "satellite-panel" in text and "satellite-boom" in text),
    ("legend label Path present", ">Path</span>" in text),
    ("legend label Progress present", ">Progress</span>" in text),
    ("legend label Look present", ">Look</span>" in text),
    ("legend label Satellite present", ">Satellite</span>" in text),
    ("old current track point legend removed", "Current track point" not in text),
    ("legacy long legend text removed", "Visible pass path" not in text and "Observer look line" not in text and "AOS-to-now progress" not in text),
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
