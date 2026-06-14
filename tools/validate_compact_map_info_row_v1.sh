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
    ("COMPACT_MAP_INFO_ROW_V1 marker present", "COMPACT_MAP_INFO_ROW_V1" in text),
    ("map-info five-column override present", "minmax(112px, 1.25fr)" in text and "minmax(96px, 0.95fr)" in text),
    ("map-info nowrap present", "white-space: nowrap !important;" in text),
    ("map-info ellipsis present", "text-overflow: ellipsis !important;" in text),
    ("map-info labels inline", ".map-info strong" in text and "display: inline !important;" in text),
    ("compact font size present", "font-size: 9.8px !important;" in text),
    ("mobile compact override present", "@media (max-width: 760px)" in text and "overflow-x: auto !important;" in text),
    ("Live RX row still present", "<strong>Live RX</strong>" in text),
    ("Altitude row still present", "<strong>Altitude</strong>" in text),
]
failed = False
for name, ok in checks:
    print(("PASS: " if ok else "FAIL: ") + name)
    failed = failed or not ok

print("Validation failed." if failed else "Validation passed.")
sys.exit(1 if failed else 0)
PY
