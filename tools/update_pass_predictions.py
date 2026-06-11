#!/usr/bin/env python3
"""Generate upcoming satellite pass predictions from the local TLE catalog."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import sys
from pathlib import Path
from typing import Any

try:
    from sgp4.api import Satrec
except ImportError as exc:  # pragma: no cover - user-facing dependency guard
    raise SystemExit(
        "Missing dependency: sgp4. Install with `python -m pip install -r requirements-dev.txt`."
    ) from exc


EARTH_RADIUS_KM = 6378.137
EARTH_FLATTENING = 1.0 / 298.257223563
SECONDS_PER_DAY = 86400.0


def utc_now() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0)


def parse_utc(value: str) -> dt.datetime:
    parsed = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=dt.timezone.utc)
    return parsed.astimezone(dt.timezone.utc).replace(microsecond=0)


def iso_utc(value: dt.datetime) -> str:
    return value.astimezone(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def julian_date(value: dt.datetime) -> tuple[float, float]:
    unix_days = value.timestamp() / SECONDS_PER_DAY
    jd = unix_days + 2440587.5
    jd_int = math.floor(jd)
    return jd_int, jd - jd_int


def gmst_rad(value: dt.datetime) -> float:
    jd, fr = julian_date(value)
    full_jd = jd + fr
    t = (full_jd - 2451545.0) / 36525.0
    gmst_deg = (
        280.46061837
        + 360.98564736629 * (full_jd - 2451545.0)
        + 0.000387933 * t * t
        - (t * t * t) / 38710000.0
    )
    return math.radians(gmst_deg % 360.0)


def observer_eci(lat_deg: float, lon_deg: float, altitude_m: float, when: dt.datetime) -> tuple[float, float, float]:
    lat = math.radians(lat_deg)
    lon = math.radians(lon_deg)
    alt_km = altitude_m / 1000.0
    e2 = EARTH_FLATTENING * (2.0 - EARTH_FLATTENING)
    sin_lat = math.sin(lat)
    cos_lat = math.cos(lat)
    n = EARTH_RADIUS_KM / math.sqrt(1.0 - e2 * sin_lat * sin_lat)

    x_ecef = (n + alt_km) * cos_lat * math.cos(lon)
    y_ecef = (n + alt_km) * cos_lat * math.sin(lon)
    z_ecef = (n * (1.0 - e2) + alt_km) * sin_lat

    theta = gmst_rad(when)
    cos_t = math.cos(theta)
    sin_t = math.sin(theta)
    return (
        cos_t * x_ecef - sin_t * y_ecef,
        sin_t * x_ecef + cos_t * y_ecef,
        z_ecef,
    )


def look_angles(
    sat_eci: tuple[float, float, float],
    lat_deg: float,
    lon_deg: float,
    altitude_m: float,
    when: dt.datetime,
) -> tuple[float, float, float]:
    obs = observer_eci(lat_deg, lon_deg, altitude_m, when)
    rx = sat_eci[0] - obs[0]
    ry = sat_eci[1] - obs[1]
    rz = sat_eci[2] - obs[2]

    theta = gmst_rad(when)
    cos_t = math.cos(theta)
    sin_t = math.sin(theta)
    x = cos_t * rx + sin_t * ry
    y = -sin_t * rx + cos_t * ry
    z = rz

    lat = math.radians(lat_deg)
    lon = math.radians(lon_deg)
    sin_lat = math.sin(lat)
    cos_lat = math.cos(lat)
    sin_lon = math.sin(lon)
    cos_lon = math.cos(lon)

    east = -sin_lon * x + cos_lon * y
    north = -sin_lat * cos_lon * x - sin_lat * sin_lon * y + cos_lat * z
    up = cos_lat * cos_lon * x + cos_lat * sin_lon * y + sin_lat * z

    range_km = math.sqrt(east * east + north * north + up * up)
    elevation_deg = math.degrees(math.asin(up / range_km))
    azimuth_deg = (math.degrees(math.atan2(east, north)) + 360.0) % 360.0
    return azimuth_deg, elevation_deg, range_km


def propagate(satrec: Satrec, when: dt.datetime) -> tuple[float, float, float] | None:
    jd, fr = julian_date(when)
    error, position, _velocity = satrec.sgp4(jd, fr)
    if error != 0:
        return None
    return float(position[0]), float(position[1]), float(position[2])


def unique_downlinks(satellite: dict[str, Any]) -> list[int]:
    values: list[int] = []
    seen: set[int] = set()
    for tx in satellite.get("transmitters", []):
        hz = tx.get("downlink_hz")
        if not isinstance(hz, int) or hz in seen:
            continue
        seen.add(hz)
        values.append(hz)
    return values


def refine_crossing(
    satrec: Satrec,
    lat: float,
    lon: float,
    alt_m: float,
    threshold: float,
    low: dt.datetime,
    high: dt.datetime,
) -> dt.datetime:
    for _ in range(10):
        mid = low + (high - low) / 2
        sat_pos = propagate(satrec, mid)
        if sat_pos is None:
            break
        _az, el, _range_km = look_angles(sat_pos, lat, lon, alt_m, mid)
        if el >= threshold:
            high = mid
        else:
            low = mid
    return high


def predict_satellite_passes(
    satellite: dict[str, Any],
    observer: dict[str, Any],
    start: dt.datetime,
    hours: float,
    step_seconds: int,
) -> list[dict[str, Any]]:
    tle = satellite.get("tle") or {}
    line1 = tle.get("line1")
    line2 = tle.get("line2")
    if not line1 or not line2:
        return []

    satrec = Satrec.twoline2rv(line1, line2)
    lat = float(observer["latitude_deg"])
    lon = float(observer["longitude_deg"])
    alt_m = float(observer.get("altitude_m", 0))
    min_el = float(observer.get("minimum_elevation_deg", 10))

    end = start + dt.timedelta(hours=hours)
    step = dt.timedelta(seconds=step_seconds)
    passes: list[dict[str, Any]] = []
    in_pass = False
    current: dict[str, Any] | None = None
    last_time: dt.datetime | None = None
    last_el: float | None = None
    when = start

    while when <= end:
        sat_pos = propagate(satrec, when)
        if sat_pos is None:
            when += step
            continue

        az, el, range_km = look_angles(sat_pos, lat, lon, alt_m, when)
        visible = el >= min_el

        if visible and not in_pass:
            aos = when
            if last_time is not None and last_el is not None and last_el < min_el:
                aos = refine_crossing(satrec, lat, lon, alt_m, min_el, last_time, when)
            current = {
                "aos_utc": aos,
                "los_utc": when,
                "tca_utc": when,
                "max_elevation_deg": el,
                "aos_azimuth_deg": az,
                "los_azimuth_deg": az,
                "range_at_tca_km": range_km,
            }
            in_pass = True

        if visible and in_pass and current is not None:
            current["los_utc"] = when
            current["los_azimuth_deg"] = az
            if el > float(current["max_elevation_deg"]):
                current["max_elevation_deg"] = el
                current["tca_utc"] = when
                current["range_at_tca_km"] = range_km

        if not visible and in_pass and current is not None:
            los = when
            if last_time is not None and last_el is not None and last_el >= min_el:
                los = refine_crossing(satrec, lat, lon, alt_m, min_el, when, last_time)
            current["los_utc"] = los
            duration_s = max(0, int((current["los_utc"] - current["aos_utc"]).total_seconds()))
            if duration_s >= 60:
                passes.append(format_pass(satellite, current, duration_s))
            current = None
            in_pass = False

        last_time = when
        last_el = el
        when += step

    if in_pass and current is not None:
        duration_s = max(0, int((current["los_utc"] - current["aos_utc"]).total_seconds()))
        if duration_s >= 60:
            passes.append(format_pass(satellite, current, duration_s))

    return passes


def format_pass(satellite: dict[str, Any], row: dict[str, Any], duration_s: int) -> dict[str, Any]:
    return {
        "norad_id": satellite.get("norad_id"),
        "name": satellite.get("name"),
        "aos_utc": iso_utc(row["aos_utc"]),
        "tca_utc": iso_utc(row["tca_utc"]),
        "los_utc": iso_utc(row["los_utc"]),
        "duration_s": duration_s,
        "max_elevation_deg": round(float(row["max_elevation_deg"]), 1),
        "aos_azimuth_deg": round(float(row["aos_azimuth_deg"]), 1),
        "los_azimuth_deg": round(float(row["los_azimuth_deg"]), 1),
        "range_at_tca_km": round(float(row["range_at_tca_km"]), 1),
        "downlinks_hz": unique_downlinks(satellite)[:6],
        "modes": satellite.get("modes", [])[:8],
    }


def load_json(path: str) -> Any:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def build_predictions(args: argparse.Namespace) -> dict[str, Any]:
    observer = load_json(args.observer)
    catalog = load_json(args.catalog)
    start = parse_utc(args.start_utc) if args.start_utc else utc_now()

    passes: list[dict[str, Any]] = []
    for satellite in catalog.get("satellites", []):
        passes.extend(
            predict_satellite_passes(
                satellite,
                observer,
                start,
                args.hours,
                args.step_seconds,
            )
        )

    passes.sort(key=lambda row: row["aos_utc"])
    if args.limit > 0:
        passes = passes[: args.limit]

    return {
        "version": 1,
        "generated_utc": iso_utc(utc_now()),
        "start_utc": iso_utc(start),
        "hours": args.hours,
        "observer": observer,
        "metadata": {
            "minimum_elevation_deg": observer.get("minimum_elevation_deg", 10),
            "satellite_count": len(catalog.get("satellites", [])),
            "pass_count": len(passes),
            "step_seconds": args.step_seconds,
        },
        "passes": passes,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", default="data/satellites.json")
    parser.add_argument("--observer", default="config/observer.example.json")
    parser.add_argument("--output", default="data/passes.json")
    parser.add_argument("--hours", type=float, default=24.0)
    parser.add_argument("--limit", type=int, default=80)
    parser.add_argument("--step-seconds", type=int, default=30)
    parser.add_argument("--start-utc", default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    predictions = build_predictions(args)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(predictions, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    meta = predictions["metadata"]
    print(f"wrote {output}")
    print(f"passes: {meta['pass_count']}")
    print(f"window: {predictions['start_utc']} + {predictions['hours']}h")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
