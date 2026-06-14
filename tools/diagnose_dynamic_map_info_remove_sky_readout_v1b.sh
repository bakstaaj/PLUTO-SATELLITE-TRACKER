#!/usr/bin/env bash
set -u

ROOT="${1:-.}"
HTML="$ROOT/web/index.html"

if [[ ! -f "$HTML" ]]; then
  echo "FAIL: missing $HTML"
  exit 1
fi

python - "$HTML" <<'PY'
from pathlib import Path
import sys

text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
lines = text.splitlines()
needles = ["skyLiveReadout", "Current look angle", '<div class="map-info">', "formatDateTime(focusPoint", "focusPoint.", "livePoint."]
for i, line in enumerate(lines, start=1):
    if any(n in line for n in needles):
        print(f"{i}: {line}")
PY
