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

needles = ["skyLiveReadout", "Current look angle", "current look angle"]
lines = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace").splitlines()
found = False
for i, line in enumerate(lines, start=1):
    if any(needle in line for needle in needles):
        found = True
        print(f"{i}: {line}")
if not found:
    print("PASS: no stale current-look readout text found")
PY
