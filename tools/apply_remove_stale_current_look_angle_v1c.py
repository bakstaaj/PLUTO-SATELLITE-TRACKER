#!/usr/bin/env python3
# Remove stale Current Look Angle label v1c.
#
# Corrective patch after DYNAMIC_MAP_INFO_REMOVE_SKY_READOUT_V1B:
#   - v1b successfully moved map-info to livePoint.
#   - This removes residual "Current look angle" text that may remain in
#     comments, templates, aria labels, or stale helper text.
#   - It does not change map-info, Listen, or backend behavior.
#
# Run:
#   python tools/apply_remove_stale_current_look_angle_v1c.py .

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

MARKER = "REMOVE_STALE_CURRENT_LOOK_ANGLE_V1C"


def scrub_current_look_angle(text: str) -> str:
    # Remove any remaining skyLiveReadout block if an earlier patch did not
    # match a rearranged version.
    while 'id="skyLiveReadout"' in text:
        marker = 'id="skyLiveReadout"'
        start = text.find(marker)
        div_start = text.rfind("<div", 0, start)
        if div_start < 0:
            break
        line_start = text.rfind("\n", 0, div_start)
        if line_start < 0:
            line_start = div_start
        else:
            line_start += 1
        close = text.find("</div>", start)
        if close < 0:
            break
        div_end = close + len("</div>")
        if div_end < len(text) and text[div_end] == "\n":
            div_end += 1
        text = text[:line_start] + text[div_end:]

    # Remove or neutralize residual plain text occurrences.
    text = text.replace("Current look angle", "Live sample")
    text = text.replace("current look angle", "live sample")

    # If any exact old sky readout HTML remains in a minified/rearranged form,
    # remove that fragment too.
    text = re.sub(
        r'<strong>\s*Live sample\s*</strong>\s*<span>\$\{escapeHtml\(liveLabel\)\}</span>',
        "",
        text,
        flags=re.DOTALL,
    )

    return text


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied stale Current look angle cleanup v1c.")
        return 0

    backup = html.with_name(html.name + ".bak-remove-stale-current-look-angle-v1c")
    shutil.copy2(html, backup)

    insertion = "    @media (max-width: 760px) {"
    if insertion in text:
        text = text.replace(insertion, f"    /* {MARKER} */\n" + insertion, 1)
    else:
        text = f"<!-- {MARKER} -->\n" + text

    text = scrub_current_look_angle(text)

    if 'id="skyLiveReadout"' in text or "Current look angle" in text:
        print("ERROR: skyLiveReadout or Current look angle still remains.")
        print("No file written.")
        return 1

    html.write_text(text, encoding="utf-8", newline="\n")

    print("Applied stale Current look angle cleanup v1c.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
