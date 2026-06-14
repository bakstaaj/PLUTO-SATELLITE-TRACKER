#!/usr/bin/env bash
set -u

ROOT="${1:-.}"
PY="$ROOT/tools/update_pass_predictions.py"

if [[ ! -f "$PY" ]]; then
  echo "FAIL: missing $PY"
  exit 1
fi

python - "$PY" <<'PY'
import ast
import sys
from pathlib import Path

path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8", errors="replace")

checks = [
    ("HIGH_RESOLUTION_PASS_SAMPLES_V1 marker present", "HIGH_RESOLUTION_PASS_SAMPLES_V1" in text),
    ("dense sample builder present", "def build_visible_pass_samples(" in text),
    ("predict function accepts pass_sample_seconds", "pass_sample_seconds: int" in text),
    ("close path rebuilds dense samples", 'current["samples"] = build_visible_pass_samples(' in text),
    ("predict call passes args.pass_sample_seconds", "args.pass_sample_seconds" in text),
    ("metadata records pass_sample_seconds", '"pass_sample_seconds": args.pass_sample_seconds' in text),
    ("CLI option present", 'parser.add_argument("--pass-sample-seconds", type=int, default=5)' in text),
    ("doppler plan still built from row samples", 'doppler_plan = build_doppler_plan(row.get("samples", []), radio)' in text),
    ("ground track still built from row samples", '"ground_track": ground_track' in text),
]

try:
    ast.parse(text)
    syntax_ok = True
except SyntaxError as exc:
    syntax_ok = False
    print(f"FAIL: Python syntax error: {exc}")

failed = not syntax_ok
for name, ok in checks:
    print(("PASS: " if ok else "FAIL: ") + name)
    failed = failed or not ok

if failed:
    print("Validation failed.")
else:
    print("Validation passed.")
sys.exit(1 if failed else 0)
PY
