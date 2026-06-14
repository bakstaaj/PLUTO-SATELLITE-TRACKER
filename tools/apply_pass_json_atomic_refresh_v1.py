#!/usr/bin/env python3
"""
Apply atomic pass JSON refresh patch v1.

Root cause:
  tools/update_pass_predictions.py writes data/passes.json directly.
  If the browser requests /api/passes while the file is being written, it can read
  a partial JSON document and fail with errors around block boundaries such as
  position 32768.

Fix:
  - Serialize JSON into memory.
  - Validate it with json.loads before touching the final file.
  - Write to passes.json.tmp.
  - Flush/fsync.
  - Atomic os.replace(tmp, final).
  - Remove tmp file on failure.

Run from repo root:
  python tools/apply_pass_json_atomic_refresh_v1.py .
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path


MARKER = "PASS_JSON_ATOMIC_REFRESH_V1"

OLD_IMPORT = "import math\n"
NEW_IMPORT = "import math\nimport os\n"

OLD_BLOCK = """    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(predictions, indent=2, sort_keys=True) + "\\n", encoding="utf-8")
"""

NEW_BLOCK = """    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    # PASS_JSON_ATOMIC_REFRESH_V1:
    # Never expose a partially written passes.json to the HTTP backend.
    # The UI polls /api/passes while refreshes are running, so direct writes can
    # produce transient or persistent invalid JSON. Build and validate the full
    # JSON document first, then atomically replace the final file.
    body = json.dumps(predictions, indent=2, sort_keys=True) + "\\n"
    json.loads(body)

    tmp_output = output.with_name(output.name + ".tmp")
    try:
        with tmp_output.open("w", encoding="utf-8") as handle:
            handle.write(body)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_output, output)
    except Exception:
        try:
            tmp_output.unlink()
        except FileNotFoundError:
            pass
        raise
"""


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    path = root / "tools" / "update_pass_predictions.py"
    if not path.exists():
        print(f"ERROR: missing {path}")
        return 1

    text = path.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied pass JSON atomic refresh v1.")
        return 0

    if "import os\n" not in text:
        if OLD_IMPORT not in text:
            print("ERROR: could not find import insertion point.")
            return 1
        text = text.replace(OLD_IMPORT, NEW_IMPORT, 1)

    if OLD_BLOCK not in text:
        print("ERROR: could not find direct passes.json write block.")
        print("Expected block:")
        print(OLD_BLOCK)
        return 1

    backup = path.with_name(path.name + ".bak-pass-json-atomic-refresh-v1")
    shutil.copy2(path, backup)
    text = text.replace(OLD_BLOCK, NEW_BLOCK, 1)
    path.write_text(text, encoding="utf-8", newline="\n")

    print("Applied pass JSON atomic refresh v1.")
    print(f"Updated: {path}")
    print(f"Backup:  {backup}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
