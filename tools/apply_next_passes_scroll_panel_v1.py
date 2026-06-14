#!/usr/bin/env python3
# Next Passes scroll panel v1.
#
# Makes the Next Passes list scroll inside its panel when the pass list
# would extend beyond the bottom of the browser window.
#
# Run:
#   python tools/apply_next_passes_scroll_panel_v1.py .

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "NEXT_PASSES_SCROLL_PANEL_V1"

CSS = r'''
    /* NEXT_PASSES_SCROLL_PANEL_V1 */
    #passes.pass-list,
    #passes.pass-loading,
    #passes.empty {
      max-height: calc(100vh - 155px);
      overflow-y: auto;
      overflow-x: hidden;
      padding-right: 4px;
      overscroll-behavior: contain;
      scrollbar-width: thin;
    }

    #passes.pass-list {
      align-content: start;
    }

    #passes.pass-list .filter-row {
      position: sticky;
      top: 0;
      z-index: 3;
      margin-bottom: 4px;
      padding: 0 0 6px;
      background: var(--panel);
    }

    #passes::-webkit-scrollbar {
      width: 8px;
    }

    #passes::-webkit-scrollbar-track {
      background: #0f1d2a;
      border-radius: 999px;
    }

    #passes::-webkit-scrollbar-thumb {
      background: var(--line);
      border-radius: 999px;
    }

    #passes::-webkit-scrollbar-thumb:hover {
      background: var(--muted);
    }
'''

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied next passes scroll panel v1.")
        return 0

    backup = html.with_name(html.name + ".bak-next-passes-scroll-panel-v1")
    shutil.copy2(html, backup)

    insertion = "    @media (max-width: 760px) {"
    if insertion not in text:
        print("ERROR: could not find CSS insertion point.")
        print("No file written.")
        return 1

    text = text.replace(insertion, CSS + "\n" + insertion, 1)

    html.write_text(text, encoding="utf-8", newline="\n")

    print("Applied next passes scroll panel v1.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
