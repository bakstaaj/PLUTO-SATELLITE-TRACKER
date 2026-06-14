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

def find_function_span(source: str, name: str):
    marker = f"function {name}("
    start = source.find(marker)
    if start < 0:
        return -1, -1
    brace = source.find("{", start)
    if brace < 0:
        return -1, -1

    depth = 0
    in_string = ""
    escape = False
    for i in range(brace, len(source)):
        ch = source[i]

        if in_string:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == in_string:
                in_string = ""
            continue

        if ch in ("'", '"', "`"):
            in_string = ch
            continue

        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return start, i + 1
    return -1, -1

start, end = find_function_span(text, "liveLookPointForPass")
body = text[start:end] if start >= 0 and end > start else ""

null_guard_pos = body.find("if (!pass) return focusPoint || null;")
track_guard_pos = body.find("if (!trackPoints.length) return focusPoint || null;")
timing_pos = body.find("const timing = passTimingState(pass);")

checks = [
    ("MAP_NULL_PASS_STARTUP_FIX_V1 marker present", "MAP_NULL_PASS_STARTUP_FIX_V1" in text),
    ("liveLookPointForPass present", bool(body)),
    ("null pass guard present inside function", null_guard_pos >= 0),
    ("empty ground track guard present inside function", track_guard_pos >= 0),
    ("passTimingState occurs inside function", timing_pos >= 0),
    ("passTimingState only after null guard inside function", null_guard_pos >= 0 and timing_pos > null_guard_pos),
    ("passTimingState only after ground-track guard inside function", track_guard_pos >= 0 and timing_pos > track_guard_pos),
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
