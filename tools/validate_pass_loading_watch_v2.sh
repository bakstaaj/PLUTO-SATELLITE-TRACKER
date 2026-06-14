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
    ("PASS_LOADING_WATCH_V2 marker present", "PASS_LOADING_WATCH_V2" in text),
    ("quick-load stale detector present", "function passPayloadNeedsQuickLoadV2(payload)" in text),
    ("global loading helper present", "function showPassLoadingFeedbackV2(title, detail)" in text),
    ("watch condition present", "function shouldKeepPassLoadingSpinnerV2(passesPayload, refreshStatus)" in text),
    ("refresh uses watch condition", "shouldKeepPassLoadingSpinnerV2(passes, refreshStatus)" in text),
    ("spinner kept during running stale payload", "Refresh state: ${refreshStatus.state" in text),
    ("watch flag clears before normal render", "window.__plutoPassLoadingWatchV2 = false;" in text),
    ("normal render still present", "renderPasses(passes);" in text),
    ("v1 calls replaced", "showPassLoadingFeedbackV1(" not in text),
    ("v2 calls present", "showPassLoadingFeedbackV2(" in text),
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
