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
    ("PASS_LOADING_SPINNER_V1 marker present", "PASS_LOADING_SPINNER_V1" in text),
    ("spinner CSS present", ".pass-loading-spinner" in text and "@keyframes passLoadingSpin" in text),
    ("initial passes spinner present", '<div id="passes" class="pass-loading">' in text),
    ("runtime helper present", "function showPassLoadingFeedbackV1(title, detail)" in text),
    ("startup sync feedback present", 'showPassLoadingFeedbackV1("Syncing Pluto time..."' in text),
    ("quick preview feedback present", 'showPassLoadingFeedbackV1("Loading quick pass preview..."' in text),
    ("manual regenerate feedback present", 'showPassLoadingFeedbackV1("Regenerating pass preview..."' in text),
    ("refresh request feedback present", 'showPassLoadingFeedbackV1("Requesting quick pass preview..."' in text),
    ("startup failure restores empty class", 'passesNode.className = "empty";' in text),
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
