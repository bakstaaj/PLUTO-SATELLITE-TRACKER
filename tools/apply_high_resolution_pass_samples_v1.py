#!/usr/bin/env python3
# High-resolution pass samples v1.

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "HIGH_RESOLUTION_PASS_SAMPLES_V1"

DENSE_FUNCTION = '''
# HIGH_RESOLUTION_PASS_SAMPLES_V1
def build_visible_pass_samples(
    satrec: Satrec,
    lat: float,
    lon: float,
    alt_m: float,
    aos: dt.datetime,
    los: dt.datetime,
    sample_seconds: int,
) -> list[dict[str, Any]]:
    sample_seconds = max(1, int(sample_seconds or 5))
    step = dt.timedelta(seconds=sample_seconds)
    samples: list[dict[str, Any]] = []

    when = aos
    while when <= los:
        sat_pos = propagate(satrec, when)
        if sat_pos is not None:
            az, el, range_km = look_angles(sat_pos, lat, lon, alt_m, when)
            sat_lat_deg, sat_lon_deg, sat_alt_km = eci_to_geodetic(sat_pos, when)
            samples.append(
                {
                    "time": when,
                    "azimuth_deg": az,
                    "elevation_deg": el,
                    "range_km": range_km,
                    "sat_latitude_deg": sat_lat_deg,
                    "sat_longitude_deg": sat_lon_deg,
                    "sat_altitude_km": sat_alt_km,
                }
            )
        when += step

    if not samples or samples[-1]["time"] != los:
        sat_pos = propagate(satrec, los)
        if sat_pos is not None:
            az, el, range_km = look_angles(sat_pos, lat, lon, alt_m, los)
            sat_lat_deg, sat_lon_deg, sat_alt_km = eci_to_geodetic(sat_pos, los)
            samples.append(
                {
                    "time": los,
                    "azimuth_deg": az,
                    "elevation_deg": el,
                    "range_km": range_km,
                    "sat_latitude_deg": sat_lat_deg,
                    "sat_longitude_deg": sat_lon_deg,
                    "sat_altitude_km": sat_alt_km,
                }
            )

    return samples

'''

def replace_required(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"could not find {label}")
    return text.replace(old, new, 1)

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    path = root / "tools" / "update_pass_predictions.py"
    if not path.exists():
        print(f"ERROR: missing {path}")
        return 1

    text = path.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied high-resolution pass samples v1.")
        return 0

    backup = path.with_name(path.name + ".bak-high-resolution-pass-samples-v1")
    shutil.copy2(path, backup)

    next_def = text.find("\ndef predict_satellite_passes(")
    if next_def < 0:
        print("ERROR: could not find predict_satellite_passes insertion point.")
        return 1
    text = text[:next_def] + "\n" + DENSE_FUNCTION + text[next_def:]

    try:
        old_sig = '''def predict_satellite_passes(
    satellite: dict[str, Any],
    observer: dict[str, Any],
    start: dt.datetime,
    hours: float,
    step_seconds: int,
) -> list[dict[str, Any]]:'''
        new_sig = '''def predict_satellite_passes(
    satellite: dict[str, Any],
    observer: dict[str, Any],
    start: dt.datetime,
    hours: float,
    step_seconds: int,
    pass_sample_seconds: int,
) -> list[dict[str, Any]]:'''
        text = replace_required(text, old_sig, new_sig, "predict_satellite_passes signature")

        old_close = '''            if duration_s >= 60:
                passes.append(format_pass(satellite, current, duration_s))
            current = None'''
        new_close = '''            if duration_s >= 60:
                current["samples"] = build_visible_pass_samples(
                    satrec,
                    lat,
                    lon,
                    alt_m,
                    current["aos_utc"],
                    current["los_utc"],
                    pass_sample_seconds,
                )
                passes.append(format_pass(satellite, current, duration_s))
            current = None'''
        text = replace_required(text, old_close, new_close, "pass close format_pass block")

        old_final = '''        if duration_s >= 60:
            passes.append(format_pass(satellite, current, duration_s))'''
        new_final = '''        if duration_s >= 60:
            current["samples"] = build_visible_pass_samples(
                satrec,
                lat,
                lon,
                alt_m,
                current["aos_utc"],
                current["los_utc"],
                pass_sample_seconds,
            )
            passes.append(format_pass(satellite, current, duration_s))'''
        text = replace_required(text, old_final, new_final, "final in-pass format_pass block")

        old_call = '''                args.step_seconds,
            )'''
        new_call = '''                args.step_seconds,
                args.pass_sample_seconds,
            )'''
        text = replace_required(text, old_call, new_call, "predict_satellite_passes call")

        old_meta = '''            "step_seconds": args.step_seconds,'''
        new_meta = '''            "step_seconds": args.step_seconds,
            "pass_sample_seconds": args.pass_sample_seconds,'''
        text = replace_required(text, old_meta, new_meta, "metadata step_seconds")

        old_args = '''    parser.add_argument("--step-seconds", type=int, default=30)
    parser.add_argument("--start-utc", default=None)'''
        new_args = '''    parser.add_argument("--step-seconds", type=int, default=30)
    parser.add_argument("--pass-sample-seconds", type=int, default=5)
    parser.add_argument("--start-utc", default=None)'''
        text = replace_required(text, old_args, new_args, "parse_args step-seconds block")
    except RuntimeError as exc:
        print(f"ERROR: {exc}")
        return 1

    path.write_text(text, encoding="utf-8", newline="\n")

    print("Applied high-resolution pass samples v1.")
    print(f"Updated: {path}")
    print(f"Backup:  {backup}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
