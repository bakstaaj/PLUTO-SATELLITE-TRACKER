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
    ("SKY_PLOT_AOS_TCA_LOS_COLORS_V1 marker present", "SKY_PLOT_AOS_TCA_LOS_COLORS_V1" in text),
    ("AOS sky dot CSS present", ".sky-aos-dot" in text and "#1a7f37" in text),
    ("TCA sky dot CSS present", ".sky-tca-dot" in text and "#a15c00" in text),
    ("LOS sky dot CSS present", ".sky-los-dot" in text and "#7d3ad3" in text),
    ("sky special helper present", "function skySpecialMarkerClassForPoint(pass, point, index, total)" in text),
    ("skyTrackDots uses index", "trackPoints.map((point, index)" in text),
    ("skyTrackDots uses special class helper", "skySpecialMarkerClassForPoint(pass, point, index, trackPoints.length)" in text),
    ("AOS class emitted", "sky-aos-dot sky-special-dot" in text),
    ("TCA class emitted", "sky-tca-dot sky-special-dot" in text),
    ("LOS class emitted", "sky-los-dot sky-special-dot" in text),
    ("legend AOS color still present", "background:#1a7f37" in text),
    ("legend TCA color still present", "background:#a15c00" in text),
    ("legend LOS color still present", "background:#7d3ad3" in text),
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
