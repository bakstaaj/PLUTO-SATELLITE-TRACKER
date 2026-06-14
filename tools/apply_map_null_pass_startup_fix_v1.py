#!/usr/bin/env python3
"""
Hotfix map startup null-pass crash.

Symptom:
  Cannot read properties of null (reading 'aos_utc')

Cause:
  The map UI can render before any pass is selected. The map patch's
  liveLookPointForPass() helper calls passTimingState(pass), and passTimingState()
  expects a real pass with aos_utc/los_utc.

Fix:
  Guard liveLookPointForPass() so a null/empty pass returns the focus point/null
  without calling passTimingState().

Run:
  python tools/apply_map_null_pass_startup_fix_v1.py .
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "MAP_NULL_PASS_STARTUP_FIX_V1"

NEW_FUNCTION = """    /* MAP_NULL_PASS_STARTUP_FIX_V1 */
    function liveLookPointForPass(pass, activeTrackPoint, focusPoint) {
      if (activeTrackPoint) return activeTrackPoint;
      if (!pass) return focusPoint || null;

      const trackPoints = (pass && pass.ground_track) || [];
      if (!trackPoints.length) return focusPoint || null;

      const timing = passTimingState(pass);
      if (timing === "active") {
        return findNearestTrackPoint(pass, new Date().toISOString()) || focusPoint || trackPoints[0] || null;
      }
      return focusPoint || trackPoints[0] || null;
    }

"""

def find_function_span(text: str, name: str) -> tuple[int, int]:
    marker = f"function {name}("
    start = text.find(marker)
    if start < 0:
        return -1, -1
    brace = text.find("{", start)
    if brace < 0:
        return -1, -1

    depth = 0
    in_string = ""
    escape = False
    for i in range(brace, len(text)):
        ch = text[i]

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
                end = i + 1
                while end < len(text) and text[end] in "\r\n":
                    end += 1
                return start, end
    return -1, -1

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied map null-pass startup fix v1.")
        return 0

    start, end = find_function_span(text, "liveLookPointForPass")
    if start < 0:
        print("ERROR: could not find liveLookPointForPass().")
        print("The map UI controls patch may not be applied, or the function name changed.")
        return 1

    backup = html.with_name(html.name + ".bak-map-null-pass-startup-fix-v1")
    shutil.copy2(html, backup)
    text = text[:start] + NEW_FUNCTION + text[end:]
    html.write_text(text, encoding="utf-8", newline="\n")

    print("Applied map null-pass startup fix v1.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
