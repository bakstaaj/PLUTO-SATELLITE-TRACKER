#!/usr/bin/env bash
set -u

ROOT="${1:-.}"
SCRIPT="$ROOT/tools/update_pass_predictions.py"

if [[ ! -f "$SCRIPT" ]]; then
  echo "FAIL: missing $SCRIPT"
  exit 1
fi

python - "$SCRIPT" <<'PY'
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")

checks = [
    ("PASS_JSON_ATOMIC_REFRESH_V1 marker present", "PASS_JSON_ATOMIC_REFRESH_V1" in text),
    ("os import present", "import os\n" in text),
    ("json body built before write", "body = json.dumps(predictions" in text),
    ("json validates before replace", "json.loads(body)" in text),
    ("temp output path present", 'tmp_output = output.with_name(output.name + ".tmp")' in text),
    ("fsync present", "os.fsync(handle.fileno())" in text),
    ("atomic replace present", "os.replace(tmp_output, output)" in text),
    ("tmp cleanup present", "tmp_output.unlink()" in text),
    ("old direct write removed", "output.write_text(json.dumps(predictions" not in text),
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
