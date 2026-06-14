#!/usr/bin/env python3
# Live RX frequency map-info patch v2.
#
# Built against the actual current web/index.html structure:
#   function renderMapPanel(pass, config)
#   <div><strong>Altitude</strong>${escapeHtml(`${livePoint.altitude_km} km`)}</div>
#
# Run:
#   python tools/apply_live_rx_frequency_actual_v2.py .

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "LIVE_RX_FREQUENCY_ACTUAL_V2"

HELPER = """
    /* LIVE_RX_FREQUENCY_ACTUAL_V2 */
    function liveRxLabelForMapInfoActualV2(point) {
      const rxHz = Number((lastTrackState && lastTrackState.rx_hz) || (point && point.rx_hz) || 0);
      return Number.isFinite(rxHz) && rxHz > 0 ? formatHz(rxHz) : "-";
    }

"""

ALTITUDE_ROW = """              <div><strong>Altitude</strong>${escapeHtml(`${livePoint.altitude_km} km`)}</div>"""
LIVE_RX_ROW = """              <div><strong>Live RX</strong>${escapeHtml(liveRxLabelForMapInfoActualV2(livePoint))}</div>"""


def add_marker(text: str) -> str:
    if MARKER in text:
        return text
    anchor = "    /* LISTEN_FOLLOWS_DOPPLER_PLAN_V1 */"
    if anchor in text:
        return text.replace(anchor, f"    /* {MARKER} */\n{anchor}", 1)
    anchor = "    @media (max-width: 760px) {"
    if anchor in text:
        return text.replace(anchor, f"    /* {MARKER} */\n{anchor}", 1)
    return f"<!-- {MARKER} -->\n{text}"


def insert_helper(text: str) -> str:
    if "function liveRxLabelForMapInfoActualV2(point)" in text:
        return text

    target = "    function renderMapPanel(pass, config) {"
    pos = text.find(target)
    if pos < 0:
        raise RuntimeError("could not find function renderMapPanel(pass, config)")

    return text[:pos] + HELPER + text[pos:]


def normalize_old_rows(text: str) -> str:
    for old in (
        "liveRxLabelForMapInfoV1B(livePoint)",
        "liveRxLabelForMapInfoV1C(livePoint)",
        "liveRxLabelForMapInfoV1D(livePoint)",
        "liveRxLabelForMapInfoV1E(livePoint)",
        "liveRxLabelForMapInfoActualV1(livePoint)",
    ):
        text = text.replace(old, "liveRxLabelForMapInfoActualV2(livePoint)")
    return text


def insert_live_rx_row(text: str) -> str:
    text = normalize_old_rows(text)
    if "<strong>Live RX</strong>" in text:
        return text

    pos = text.find(ALTITUDE_ROW)
    if pos < 0:
        raise RuntimeError("could not find exact livePoint Altitude row")

    insert_at = pos + len(ALTITUDE_ROW)
    return text[:insert_at] + "\n" + LIVE_RX_ROW + text[insert_at:]


def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text and "<strong>Live RX</strong>" in text and "function liveRxLabelForMapInfoActualV2(point)" in text:
        print("Already applied live RX frequency actual v2.")
        return 0

    backup = html.with_name(html.name + ".bak-live-rx-frequency-actual-v2")
    shutil.copy2(html, backup)

    try:
        text = add_marker(text)
        text = insert_helper(text)
        text = insert_live_rx_row(text)
    except RuntimeError as exc:
        print(f"ERROR: {exc}")
        print("No file written.")
        return 1

    html.write_text(text, encoding="utf-8")
    print("Applied live RX frequency actual v2.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
