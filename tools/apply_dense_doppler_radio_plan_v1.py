#!/usr/bin/env python3
# Dense Doppler radio plan v1.
#
# Adds client-side densification of the Doppler plan before posting it to
# /api/radio/track/plan. This improves actual LO tracking cadence, not just
# display movement.
#
# The backend still owns LO writes. The UI only sends a finer plan.
#
# Run:
#   python tools/apply_dense_doppler_radio_plan_v1.py .

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "DENSE_DOPPLER_RADIO_PLAN_V1"

HELPERS = '''    /* DENSE_DOPPLER_RADIO_PLAN_V1 */
    const RADIO_DOPPLER_STEP_SECONDS_V1 = 5;

    function interpolateNumberDopplerV1(a, b, ratio) {
      const av = Number(a);
      const bv = Number(b);
      if (!Number.isFinite(av) && !Number.isFinite(bv)) return undefined;
      if (!Number.isFinite(av)) return bv;
      if (!Number.isFinite(bv)) return av;
      return av + ((bv - av) * ratio);
    }

    function roundHzDopplerV1(value) {
      if (!Number.isFinite(Number(value))) return undefined;
      return Math.round(Number(value));
    }

    function isoWithoutMillisDopplerV1(epochMs) {
      return new Date(epochMs).toISOString().replace(".000Z", "Z");
    }

    function interpolateDopplerPointV1(a, b, epochMs) {
      const aMs = Date.parse(a.time_utc || "");
      const bMs = Date.parse(b.time_utc || "");
      const span = bMs - aMs;
      const ratio = span > 0 ? Math.max(0, Math.min(1, (epochMs - aMs) / span)) : 0;
      const point = {
        ...a,
        time_utc: isoWithoutMillisDopplerV1(epochMs),
        interpolated: true
      };

      for (const key of [
        "rx_hz",
        "tx_hz",
        "rx_offset_hz",
        "tx_offset_hz",
        "range_rate_m_s",
        "range_km",
        "azimuth_deg",
        "elevation_deg"
      ]) {
        const value = interpolateNumberDopplerV1(a[key], b[key], ratio);
        if (value === undefined) continue;
        point[key] = key.endsWith("_hz") ? roundHzDopplerV1(value) : Number(value.toFixed(6));
      }

      return point;
    }

    function densifyDopplerPlanForRadioV1(plan, stepSeconds = RADIO_DOPPLER_STEP_SECONDS_V1) {
      const points = (plan && plan.points) || [];
      if (points.length < 2) return plan || { points: [] };

      const sorted = points
        .map((point) => ({ point, epochMs: Date.parse(point.time_utc || "") }))
        .filter((item) => Number.isFinite(item.epochMs))
        .sort((a, b) => a.epochMs - b.epochMs);

      if (sorted.length < 2) return plan;

      const stepMs = Math.max(1000, Number(stepSeconds || 5) * 1000);
      const densePoints = [];
      const pushUnique = (point) => {
        if (!point || !point.time_utc) return;
        if (densePoints.length && densePoints[densePoints.length - 1].time_utc === point.time_utc) return;
        densePoints.push(point);
      };

      for (let index = 0; index < sorted.length - 1; index += 1) {
        const current = sorted[index];
        const next = sorted[index + 1];
        pushUnique({ ...current.point, interpolated: false });

        for (let epochMs = current.epochMs + stepMs; epochMs < next.epochMs; epochMs += stepMs) {
          pushUnique(interpolateDopplerPointV1(current.point, next.point, epochMs));
        }
      }

      pushUnique({ ...sorted[sorted.length - 1].point, interpolated: false });

      return {
        ...(plan || {}),
        points: densePoints,
        original_point_count: points.length,
        dense_point_count: densePoints.length,
        dense_step_seconds: Math.round(stepMs / 1000),
        dense_generated_by: "browser"
      };
    }

    function denseDopplerPointCountTextV1(plan) {
      if (!plan || !plan.dense_point_count) return "";
      return ` | radio plan ${plan.dense_point_count} pts @ ${plan.dense_step_seconds || RADIO_DOPPLER_STEP_SECONDS_V1}s`;
    }

'''

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
        print("Already applied dense Doppler radio plan v1.")
        return 0

    backup = html.with_name(html.name + ".bak-dense-doppler-radio-plan-v1")
    shutil.copy2(html, backup)

    insertion = "    async function planDopplerTrack(pass, button) {"
    if insertion not in text:
        print("ERROR: could not find planDopplerTrack insertion point.")
        return 1
    text = text.replace(insertion, HELPERS + "\n" + insertion, 1)

    # Patch planDopplerTrack() only.
    start, end = find_function_span(text, "planDopplerTrack")
    if start < 0:
        print("ERROR: could not find planDopplerTrack().")
        return 1
    block = text[start:end]
    if "const plan = pass.doppler_plan;" not in block:
        print("ERROR: could not find plan assignment in planDopplerTrack().")
        return 1
    block = block.replace("const plan = pass.doppler_plan;", "const plan = densifyDopplerPlanForRadioV1(pass.doppler_plan);", 1)
    block = block.replace(
        'button.textContent = "Track planned";',
        'button.textContent = `Track planned${denseDopplerPointCountTextV1(plan)}`;',
        1
    )
    text = text[:start] + block + text[end:]

    # Patch dopplerTrackPayload() only.
    start, end = find_function_span(text, "dopplerTrackPayload")
    if start < 0:
        print("ERROR: could not find dopplerTrackPayload().")
        return 1
    block = text[start:end]
    if "const plan = pass.doppler_plan;" not in block:
        print("ERROR: could not find plan assignment in dopplerTrackPayload().")
        return 1
    block = block.replace("const plan = pass.doppler_plan;", "const plan = densifyDopplerPlanForRadioV1(pass.doppler_plan);", 1)
    text = text[:start] + block + text[end:]

    html.write_text(text, encoding="utf-8", newline="\n")

    print("Applied dense Doppler radio plan v1.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
