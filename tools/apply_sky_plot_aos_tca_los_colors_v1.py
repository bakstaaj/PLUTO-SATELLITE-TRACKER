#!/usr/bin/env python3
# Sky plot AOS/TCA/LOS marker colors v1.
#
# UI-only patch:
#   - Applies legend AOS/TCA/LOS colors to the azimuth / sky plot markers.
#   - AOS = green
#   - TCA = orange/brown
#   - LOS = purple
#   - Normal path samples remain path orange.
#   - Focused sample remains dark focus color unless it is also one of the
#     AOS/TCA/LOS special markers, where the AOS/TCA/LOS identity is kept.
#
# Run:
#   python tools/apply_sky_plot_aos_tca_los_colors_v1.py .

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path

MARKER = "SKY_PLOT_AOS_TCA_LOS_COLORS_V1"

CSS = r'''
    /* SKY_PLOT_AOS_TCA_LOS_COLORS_V1 */
    .sky-aos-dot {
      fill: #1a7f37 !important;
      stroke: #ffffff !important;
    }

    .sky-tca-dot {
      fill: #a15c00 !important;
      stroke: #ffffff !important;
    }

    .sky-los-dot {
      fill: #7d3ad3 !important;
      stroke: #ffffff !important;
    }

    .sky-special-dot {
      r: 5.2;
    }
'''

HELPER = r'''
    /* SKY_PLOT_AOS_TCA_LOS_COLORS_V1 */
    function skySpecialMarkerClassForPoint(pass, point, index, total) {
      if (!point || !pass) return "";
      const pointTime = point.time_utc || "";

      if (index === 0 || pointTime === pass.aos_utc) {
        return "sky-aos-dot sky-special-dot";
      }

      if (index === total - 1 || pointTime === pass.los_utc) {
        return "sky-los-dot sky-special-dot";
      }

      if (pass.tca_utc && pointTime === pass.tca_utc) {
        return "sky-tca-dot sky-special-dot";
      }

      /*
       * High-resolution sample generation can mean the exact TCA timestamp is
       * not one of the sample times. In that case, make the sample nearest TCA
       * carry the TCA color.
       */
      const target = Date.parse(pass.tca_utc || "");
      if (Number.isFinite(target)) {
        const points = (pass.ground_track || []);
        let bestIndex = -1;
        let bestDelta = Infinity;
        points.forEach((candidate, candidateIndex) => {
          const candidateTime = Date.parse(candidate.time_utc || "");
          if (!Number.isFinite(candidateTime)) return;
          const delta = Math.abs(candidateTime - target);
          if (delta < bestDelta) {
            bestDelta = delta;
            bestIndex = candidateIndex;
          }
        });
        if (index === bestIndex) {
          return "sky-tca-dot sky-special-dot";
        }
      }

      return "";
    }
'''

def replace_required(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"could not find {label}")
    return text.replace(old, new, 1)

def patch_sky_track_dots(text: str) -> str:
    # Preferred target after the sky legend color patch.
    old = '''      const skyTrackDots = trackPoints.map((point) => {
        const xy = projectSkyPoint(point.azimuth_deg, point.elevation_deg, skySize);
        const isFocus = focusPoint && point.time_utc === focusPoint.time_utc;
        return `<circle class="${isFocus ? "sky-focus-dot" : "sky-sample-dot"} track-sample-dot" data-time="${escapeHtml(point.time_utc)}" cx="${xy.x.toFixed(1)}" cy="${xy.y.toFixed(1)}" r="${isFocus ? "5.5" : "3.2"}" fill="${isFocus ? "#102030" : "#c4471c"}" stroke="#fff" stroke-width="${isFocus ? "2" : "1.2"}" />`;
      }).join("");'''
    new = '''      const skyTrackDots = trackPoints.map((point, index) => {
        const xy = projectSkyPoint(point.azimuth_deg, point.elevation_deg, skySize);
        const isFocus = focusPoint && point.time_utc === focusPoint.time_utc;
        const specialClass = skySpecialMarkerClassForPoint(pass, point, index, trackPoints.length);
        const className = specialClass || (isFocus ? "sky-focus-dot" : "sky-sample-dot");
        const radius = specialClass ? "5.2" : (isFocus ? "5.5" : "3.2");
        const strokeWidth = specialClass || isFocus ? "2" : "1.2";
        return `<circle class="${className} track-sample-dot" data-time="${escapeHtml(point.time_utc)}" cx="${xy.x.toFixed(1)}" cy="${xy.y.toFixed(1)}" r="${radius}" fill="${isFocus && !specialClass ? "#102030" : "#c4471c"}" stroke="#fff" stroke-width="${strokeWidth}" />`;
      }).join("");'''
    if old in text:
        return text.replace(old, new, 1)

    # Fallback target before the sky legend color patch.
    old2 = '''      const skyTrackDots = trackPoints.map((point) => {
        const xy = projectSkyPoint(point.azimuth_deg, point.elevation_deg, skySize);
        const isFocus = focusPoint && point.time_utc === focusPoint.time_utc;
        return `<circle class="track-sample-dot" data-time="${escapeHtml(point.time_utc)}" cx="${xy.x.toFixed(1)}" cy="${xy.y.toFixed(1)}" r="${isFocus ? "5.5" : "3.2"}" fill="${isFocus ? "#102030" : "#c4471c"}" stroke="#fff" stroke-width="${isFocus ? "2" : "1.2"}" />`;
      }).join("");'''
    new2 = '''      const skyTrackDots = trackPoints.map((point, index) => {
        const xy = projectSkyPoint(point.azimuth_deg, point.elevation_deg, skySize);
        const isFocus = focusPoint && point.time_utc === focusPoint.time_utc;
        const specialClass = skySpecialMarkerClassForPoint(pass, point, index, trackPoints.length);
        const className = specialClass || (isFocus ? "sky-focus-dot" : "sky-sample-dot");
        const radius = specialClass ? "5.2" : (isFocus ? "5.5" : "3.2");
        const strokeWidth = specialClass || isFocus ? "2" : "1.2";
        return `<circle class="${className} track-sample-dot" data-time="${escapeHtml(point.time_utc)}" cx="${xy.x.toFixed(1)}" cy="${xy.y.toFixed(1)}" r="${radius}" fill="${isFocus && !specialClass ? "#102030" : "#c4471c"}" stroke="#fff" stroke-width="${strokeWidth}" />`;
      }).join("");'''
    if old2 in text:
        return text.replace(old2, new2, 1)

    # Last fallback: detect the block structurally.
    pattern = re.compile(
        r'      const skyTrackDots = trackPoints\.map\(\(point\) => \{.*?      \}\)\.join\(""\);',
        re.DOTALL,
    )
    replacement = new
    text, count = pattern.subn(replacement, text, count=1)
    if count == 0:
        raise RuntimeError("could not find skyTrackDots block")
    return text

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied sky plot AOS/TCA/LOS colors v1.")
        return 0

    backup = html.with_name(html.name + ".bak-sky-plot-aos-tca-los-colors-v1")
    shutil.copy2(html, backup)

    try:
        insertion = "    @media (max-width: 760px) {"
        text = replace_required(text, insertion, CSS + "\n" + insertion, "CSS insertion point")

        helper_insertion = "    function renderMapPanel(pass, config) {"
        text = replace_required(text, helper_insertion, HELPER + "\n" + helper_insertion, "renderMapPanel insertion point")

        text = patch_sky_track_dots(text)
    except RuntimeError as exc:
        print(f"ERROR: {exc}")
        print("No file written.")
        return 1

    html.write_text(text, encoding="utf-8", newline="\n")

    print("Applied sky plot AOS/TCA/LOS colors v1.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
