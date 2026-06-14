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
    ("NEXT_PASSES_SCROLL_PANEL_V1 marker present", "NEXT_PASSES_SCROLL_PANEL_V1" in text),
    ("passes max-height set", "#passes.pass-list" in text and "max-height: calc(100vh - 155px);" in text),
    ("passes vertical scroll enabled", "overflow-y: auto;" in text),
    ("passes horizontal overflow hidden", "overflow-x: hidden;" in text),
    ("pass filter sticky in scroll panel", "#passes.pass-list .filter-row" in text and "position: sticky;" in text),
    ("scrollbar styling present", "#passes::-webkit-scrollbar" in text and "scrollbar-width: thin;" in text),
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
