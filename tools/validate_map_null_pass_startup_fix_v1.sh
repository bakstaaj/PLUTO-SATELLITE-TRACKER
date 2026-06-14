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
    ("MAP_NULL_PASS_STARTUP_FIX_V1 marker present", "MAP_NULL_PASS_STARTUP_FIX_V1" in text),
    ("liveLookPointForPass present", "function liveLookPointForPass(pass, activeTrackPoint, focusPoint)" in text),
    ("null pass guard present", "if (!pass) return focusPoint || null;" in text),
    ("empty ground track guard present", "if (!trackPoints.length) return focusPoint || null;" in text),
    ("passTimingState only after guard", text.find("if (!pass) return focusPoint || null;") < text.find("const timing = passTimingState(pass);")),
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
