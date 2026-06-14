#!/usr/bin/env python3
# Sky plot legend colors and satellite icon v1.
#
# UI-only patch:
#   - Makes the azimuth / sky plot use the same semantic colors as the legend.
#   - Replaces the current live satellite dot on the sky plot with a small satellite icon.
#   - Updates legend wording from Current track point / Satellite variants to Satellite.
#
# Run:
#   python tools/apply_sky_plot_legend_colors_sat_icon_v1.py .

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

MARKER = "SKY_PLOT_LEGEND_COLORS_SAT_ICON_V1"

CSS = r'''
    /* SKY_PLOT_LEGEND_COLORS_SAT_ICON_V1 */
    .sky-pass-path {
      stroke: #c4471c;
    }

    .sky-pass-progress {
      stroke: #00a7c7;
    }

    .sky-look-line {
      stroke: #3e6b85;
    }

    .sky-focus-dot {
      fill: #102030;
      stroke: #ffffff;
    }

    .sky-sample-dot {
      fill: #c4471c;
      stroke: #ffffff;
    }

    .sky-satellite-icon .satellite-body {
      fill: #00a7c7;
      stroke: #ffffff;
      stroke-width: 1.4;
    }

    .sky-satellite-icon .satellite-panel {
      fill: #00a7c7;
      stroke: #ffffff;
      stroke-width: 1;
    }

    .sky-satellite-icon .satellite-boom {
      stroke: #ffffff;
      stroke-width: 1.2;
      stroke-linecap: round;
    }
'''

HELPER = r'''
    /* SKY_PLOT_LEGEND_COLORS_SAT_ICON_V1 */
    function renderSkySatelliteIcon(point) {
      if (!point) return "";
      const x = Number(point.x || 0).toFixed(1);
      const y = Number(point.y || 0).toFixed(1);
      return `
        <g class="sky-satellite-icon" transform="translate(${x} ${y}) rotate(-25)">
          <line class="satellite-boom" x1="-10" y1="0" x2="10" y2="0" />
          <rect class="satellite-panel" x="-17" y="-5" width="8" height="10" rx="1.2" />
          <rect class="satellite-panel" x="9" y="-5" width="8" height="10" rx="1.2" />
          <rect class="satellite-body" x="-6" y="-6" width="12" height="12" rx="2" />
          <line class="satellite-boom" x1="0" y1="-8" x2="0" y2="-13" />
        </g>
      `;
    }
'''

def replace_required(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"could not find {label}")
    return text.replace(old, new, 1)

def patch_svg_colors_and_icon(text: str) -> str:
    # Add semantic classes to sky plot elements while preserving dimensions.
    text = text.replace(
        '<circle class="track-sample-dot" data-time="${escapeHtml(point.time_utc)}" cx="${xy.x.toFixed(1)}" cy="${xy.y.toFixed(1)}" r="${isFocus ? "5.5" : "3.2"}" fill="${isFocus ? "#102030" : "#c4471c"}" stroke="#fff" stroke-width="${isFocus ? "2" : "1.2"}" />',
        '<circle class="${isFocus ? "sky-focus-dot" : "sky-sample-dot"} track-sample-dot" data-time="${escapeHtml(point.time_utc)}" cx="${xy.x.toFixed(1)}" cy="${xy.y.toFixed(1)}" r="${isFocus ? "5.5" : "3.2"}" fill="${isFocus ? "#102030" : "#c4471c"}" stroke="#fff" stroke-width="${isFocus ? "2" : "1.2"}" />',
        1,
    )
    text = text.replace(
        '<polyline fill="none" stroke="#c4471c" stroke-width="3" stroke-linecap="round" stroke-linejoin="round" points="${skyPath}" />',
        '<polyline class="sky-pass-path" fill="none" stroke="#c4471c" stroke-width="3" stroke-linecap="round" stroke-linejoin="round" points="${skyPath}" />',
        1,
    )
    text = text.replace(
        '<polyline fill="none" stroke="#00a7c7" stroke-width="4" stroke-linecap="round" stroke-linejoin="round" stroke-dasharray="8 6" points="${skyProgressPath}" />',
        '<polyline class="sky-pass-progress" fill="none" stroke="#00a7c7" stroke-width="4" stroke-linecap="round" stroke-linejoin="round" stroke-dasharray="8 6" points="${skyProgressPath}" />',
        1,
    )
    text = text.replace(
        '<line x1="${skyCenter}" y1="${skyCenter}" x2="${liveSky.x.toFixed(1)}" y2="${liveSky.y.toFixed(1)}" stroke="#3e6b85" stroke-width="2" stroke-dasharray="5 4" />',
        '<line class="sky-look-line" x1="${skyCenter}" y1="${skyCenter}" x2="${liveSky.x.toFixed(1)}" y2="${liveSky.y.toFixed(1)}" stroke="#3e6b85" stroke-width="2" stroke-dasharray="5 4" />',
        1,
    )
    text = text.replace(
        '<circle cx="${focusSky.x.toFixed(1)}" cy="${focusSky.y.toFixed(1)}" r="6" fill="#102030" stroke="#fff" stroke-width="2" />',
        '<circle class="sky-focus-dot" cx="${focusSky.x.toFixed(1)}" cy="${focusSky.y.toFixed(1)}" r="6" fill="#102030" stroke="#fff" stroke-width="2" />',
        1,
    )

    # Replace live/current satellite dot with a satellite icon.
    old_live_dot = '${liveSky ? `<circle cx="${liveSky.x.toFixed(1)}" cy="${liveSky.y.toFixed(1)}" r="6" fill="#00a7c7" stroke="#fff" stroke-width="2" />` : ""}'
    if old_live_dot in text:
        text = text.replace(old_live_dot, '${renderSkySatelliteIcon(liveSky)}', 1)
    else:
        # More robust fallback for slightly changed live circle markup.
        text, count = re.subn(
            r'\$\{liveSky \? `\s*<circle[^`]*fill="#00a7c7"[^`]*>` : ""\}',
            '${renderSkySatelliteIcon(liveSky)}',
            text,
            count=1,
            flags=re.DOTALL,
        )
        if count == 0 and "renderSkySatelliteIcon(liveSky)" not in text:
            raise RuntimeError("could not find live sky satellite dot")
    return text

def patch_legend_labels(text: str) -> str:
    # Keep compact labels if they exist; otherwise shorten legacy labels.
    replacements = {
        "Visible pass path": "Path",
        "AOS-to-now progress": "Progress",
        "Observer look line": "Look",
        "Focused sample": "Focus",
        "Current track point": "Satellite",
        "Current satellite": "Satellite",
    }
    for old, new in replacements.items():
        text = text.replace(old, new)
    return text

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied sky plot legend colors and satellite icon v1.")
        return 0

    backup = html.with_name(html.name + ".bak-sky-plot-legend-colors-sat-icon-v1")
    shutil.copy2(html, backup)

    try:
        insertion = "    @media (max-width: 760px) {"
        text = replace_required(text, insertion, CSS + "\n" + insertion, "CSS insertion point")

        helper_insertion = "    function renderMapPanel(pass, config) {"
        text = replace_required(text, helper_insertion, HELPER + "\n" + helper_insertion, "renderMapPanel insertion point")

        text = patch_svg_colors_and_icon(text)
        text = patch_legend_labels(text)
    except RuntimeError as exc:
        print(f"ERROR: {exc}")
        print("No file written.")
        return 1

    html.write_text(text, encoding="utf-8", newline="\n")

    print("Applied sky plot legend colors and satellite icon v1.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
