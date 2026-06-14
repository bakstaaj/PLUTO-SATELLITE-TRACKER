#!/usr/bin/env python3
# Dynamic map info and remove sky current-look readout v1b.
#
# Corrective version of v1:
#   - Does not rely on the old map-info block location.
#   - Finds the existing map-info block and changes only its displayed values
#     from focusPoint/static sample to livePoint/current sample.
#   - Removes the redundant skyLiveReadout block.
#   - Keeps the Listen panel under the azimuth plot.
#
# Run:
#   python tools/apply_dynamic_map_info_remove_sky_readout_v1b.py .

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "DYNAMIC_MAP_INFO_REMOVE_SKY_READOUT_V1B"


def remove_sky_live_readout(text: str) -> str:
    if 'id="skyLiveReadout"' not in text:
        return text

    marker = 'id="skyLiveReadout"'
    start = text.find(marker)
    div_start = text.rfind("<div", 0, start)
    if div_start < 0:
        raise RuntimeError("could not find skyLiveReadout opening div")

    line_start = text.rfind("\n", 0, div_start)
    if line_start < 0:
        line_start = div_start
    else:
        line_start += 1

    close = text.find("</div>", start)
    if close < 0:
        raise RuntimeError("could not find skyLiveReadout closing div")

    div_end = close + len("</div>")
    if div_end < len(text) and text[div_end] == "\n":
        div_end += 1

    return text[:line_start] + text[div_end:]


def find_map_info_region(text: str) -> tuple[int, int]:
    marker = '<div class="map-info">'
    div_start = text.find(marker)
    if div_start < 0:
        raise RuntimeError("could not find map-info div")

    # The map-info block is embedded in a JS template conditional:
    # ${focusPoint ? ` ... <div class="map-info"> ... </div> ... ` : ""}
    # Find the nearest template conditional before it.
    search_start = max(0, div_start - 1200)
    prefix = text[search_start:div_start]
    rel_cond = max(prefix.rfind("${focusPoint ? `"), prefix.rfind("${livePoint ? `"))
    if rel_cond < 0:
        raise RuntimeError("could not find map-info template conditional")

    region_start = search_start + rel_cond

    # End at the template conditional close. Prefer the explicit ` : ""}` marker.
    close_marker = '` : ""}'
    region_end = text.find(close_marker, div_start)
    if region_end < 0:
        raise RuntimeError("could not find map-info template conditional close")
    region_end += len(close_marker)

    return region_start, region_end


def make_map_info_dynamic(text: str) -> str:
    start, end = find_map_info_region(text)
    region = text[start:end]

    region = region.replace("${focusPoint ? `", "${livePoint ? `")

    replacements = {
        "formatDateTime(focusPoint.time_utc)": "formatDateTime(livePoint.time_utc)",
        "focusPoint.latitude_deg": "livePoint.latitude_deg",
        "focusPoint.longitude_deg": "livePoint.longitude_deg",
        "focusPoint.azimuth_deg": "livePoint.azimuth_deg",
        "focusPoint.elevation_deg": "livePoint.elevation_deg",
        "focusPoint.altitude_km": "livePoint.altitude_km",
        # Fallback for older label if Ground Point rename has not been applied yet.
        "<strong>Subsat</strong>": "<strong>Ground Point</strong>",
    }
    for old, new in replacements.items():
        region = region.replace(old, new)

    # Keep any old focused-control button blocks out of this live readout if
    # they somehow survived in this area.
    while '<div class="button-row">' in region and ("tuneFocusButton" in region or "trackFocusStepButton" in region or "followLivePointButton" in region):
        row_start = region.find('<div class="button-row">')
        row_end = region.find('</div>', row_start)
        if row_start < 0 or row_end < 0:
            break
        region = region[:row_start] + region[row_end + len('</div>'):]

    return text[:start] + region + text[end:]


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied dynamic map info / remove sky readout v1b.")
        return 0

    backup = html.with_name(html.name + ".bak-dynamic-map-info-remove-sky-readout-v1b")
    shutil.copy2(html, backup)

    try:
        insertion = "    @media (max-width: 760px) {"
        if insertion in text:
            text = text.replace(insertion, f"    /* {MARKER} */\n" + insertion, 1)
        else:
            text = f"<!-- {MARKER} -->\n" + text

        text = remove_sky_live_readout(text)
        text = make_map_info_dynamic(text)
    except RuntimeError as exc:
        print(f"ERROR: {exc}")
        print("No file written.")
        return 1

    html.write_text(text, encoding="utf-8", newline="\n")

    print("Applied dynamic map info / remove sky readout v1b.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
