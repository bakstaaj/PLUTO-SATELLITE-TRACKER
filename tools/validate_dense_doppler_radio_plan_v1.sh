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
    ("DENSE_DOPPLER_RADIO_PLAN_V1 marker present", "DENSE_DOPPLER_RADIO_PLAN_V1" in text),
    ("5 second radio plan constant present", "const RADIO_DOPPLER_STEP_SECONDS_V1 = 5;" in text),
    ("doppler interpolation helper present", "function interpolateDopplerPointV1(a, b, epochMs)" in text),
    ("dense plan helper present", "function densifyDopplerPlanForRadioV1(plan" in text),
    ("dense metadata original count present", "original_point_count" in text),
    ("dense metadata point count present", "dense_point_count" in text),
    ("planDopplerTrack uses dense plan", "async function planDopplerTrack" in text and "const plan = densifyDopplerPlanForRadioV1(pass.doppler_plan);" in text),
    ("dopplerTrackPayload uses dense plan", "function dopplerTrackPayload(pass)" in text and text.count("const plan = densifyDopplerPlanForRadioV1(pass.doppler_plan);") >= 2),
    ("track planned button includes dense count", "denseDopplerPointCountTextV1(plan)" in text),
    ("radio track plan endpoint still used", "/api/radio/track/plan" in text),
    ("auto audio still uses dopplerTrackPayload", "dopplerTrackPayload(pass)" in text),
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
