#!/usr/bin/env python3
# Rename Subsat label to Ground Point v1.
#
# Label-only UI patch. No behavior changes.
#
# Run:
#   python tools/apply_rename_subsat_ground_point_v1.py .

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "RENAME_SUBSAT_GROUND_POINT_V1"

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied rename Subsat to Ground Point v1.")
        return 0

    if "Subsat" not in text:
        print("ERROR: did not find Subsat label in web/index.html")
        print("No file written.")
        return 1

    backup = html.with_name(html.name + ".bak-rename-subsat-ground-point-v1")
    shutil.copy2(html, backup)

    text = text.replace("Subsat", "Ground Point")

    insertion = "    @media (max-width: 760px) {"
    if insertion in text:
        text = text.replace(insertion, f"    /* {MARKER} */\n" + insertion, 1)
    else:
        text = f"<!-- {MARKER} -->\n" + text

    html.write_text(text, encoding="utf-8", newline="\n")

    print("Applied rename Subsat to Ground Point v1.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
