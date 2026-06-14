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
    ("PASS_LOADING_FILE_WATCH_V3 marker present", "PASS_LOADING_FILE_WATCH_V3" in text),
    ("pass timestamp helper present", "function passPayloadTimestampV3(payload)" in text),
    ("start file watch helper present", "function startPassFileWatchV3(title, detail)" in text),
    ("current browser payload check present", "function passPayloadLooksCurrentForBrowserV3(payload)" in text),
    ("new-enough payload check present", "function passPayloadLooksNewEnoughV3(payload)" in text),
    ("spinner gate present", "function shouldKeepPassFileSpinnerV3(passesPayload)" in text),
    ("refresh uses file-watch gate", "shouldKeepPassFileSpinnerV3(passes)" in text),
    ("refresh watches api passes wording", "Watching /api/passes for a new pass file" in text),
    ("normal render still present", "renderPasses(passes);" in text),
    ("manual quick loading starts watch", 'startPassFileWatchV3("Regenerating pass preview..."' in text),
    ("request quick loading starts watch", 'startPassFileWatchV3("Requesting quick pass preview..."' in text),
    ("sync loading starts watch", 'startPassFileWatchV3("Syncing Pluto time..."' in text),
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
