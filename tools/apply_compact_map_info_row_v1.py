#!/usr/bin/env python3
# Compact map-info row v1.
#
# Keeps Sample -> Live RX on one compact line:
#   Sample | Ground Point | Look Angles | Altitude | Live RX
#
# CSS-only patch. Uses final override rules so it applies after previous map-info
# rules, including earlier compact layout patches.
#
# Run:
#   python tools/apply_compact_map_info_row_v1.py .

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "COMPACT_MAP_INFO_ROW_V1"

CSS = (
    "\n"
    "    /* COMPACT_MAP_INFO_ROW_V1 */\n"
    "    .map-info {\n"
    "      grid-template-columns:\n"
    "        minmax(112px, 1.25fr)\n"
    "        minmax(130px, 1.35fr)\n"
    "        minmax(120px, 1.25fr)\n"
    "        minmax(82px, 0.85fr)\n"
    "        minmax(96px, 0.95fr) !important;\n"
    "      gap: 3px 6px !important;\n"
    "      padding: 5px 7px !important;\n"
    "      font-size: 9.8px !important;\n"
    "      line-height: 1.08 !important;\n"
    "      align-items: center !important;\n"
    "    }\n\n"
    "    .map-info div {\n"
    "      min-width: 0 !important;\n"
    "      white-space: nowrap !important;\n"
    "      overflow: hidden !important;\n"
    "      text-overflow: ellipsis !important;\n"
    "    }\n\n"
    "    .map-info strong {\n"
    "      display: inline !important;\n"
    "      color: var(--muted);\n"
    "      font-size: 9px !important;\n"
    "      font-weight: 650 !important;\n"
    "      margin-right: 3px !important;\n"
    "      white-space: nowrap !important;\n"
    "    }\n\n"
    "    @media (max-width: 760px) {\n"
    "      .map-info {\n"
    "        grid-template-columns:\n"
    "          minmax(92px, 1.05fr)\n"
    "          minmax(108px, 1.15fr)\n"
    "          minmax(104px, 1.15fr)\n"
    "          minmax(70px, 0.75fr)\n"
    "          minmax(86px, 0.9fr) !important;\n"
    "        overflow-x: auto !important;\n"
    "        scrollbar-width: thin;\n"
    "      }\n"
    "    }\n\n"
)


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied compact map-info row v1.")
        return 0

    backup = html.with_name(html.name + ".bak-compact-map-info-row-v1")
    shutil.copy2(html, backup)

    anchor = "    @media (max-width: 760px) {"
    pos = text.find(anchor)
    if pos < 0:
        print("ERROR: could not find responsive @media anchor in style block.")
        print("No file written.")
        return 1

    text = text[:pos] + CSS + text[pos:]
    html.write_text(text, encoding="utf-8")

    print("Applied compact map-info row v1.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
