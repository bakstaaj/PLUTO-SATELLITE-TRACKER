#!/usr/bin/env python3
"""Generate upcoming satellite pass predictions from the local TLE catalog."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import math
import os
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
PLUTO_MIN_HZ = 70_000_000
PLUTO_MAX_HZ = 6_000_000_000
SPEED_OF_LIGHT_KM_S = 299_792.458


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


def eci_to_geodetic(
    sat_eci: tuple[float, float, float],
    when: dt.datetime,
) -> tuple[float, float, float]:
    theta = gmst_rad(when)
    cos_t = math.cos(theta)
    sin_t = math.sin(theta)
    x = cos_t * sat_eci[0] + sin_t * sat_eci[1]
    y = -sin_t * sat_eci[0] + cos_t * sat_eci[1]
    z = sat_eci[2]

    lon = math.atan2(y, x)
    p = math.hypot(x, y)
    e2 = EARTH_FLATTENING * (2.0 - EARTH_FLATTENING)
    lat = math.atan2(z, p * (1.0 - e2))

    for _ in range(6):
        sin_lat = math.sin(lat)
        n = EARTH_RADIUS_KM / math.sqrt(1.0 - e2 * sin_lat * sin_lat)
        alt = p / max(math.cos(lat), 1e-9) - n
        lat = math.atan2(z, p * (1.0 - e2 * (n / max(n + alt, 1e-9))))

    sin_lat = math.sin(lat)
    n = EARTH_RADIUS_KM / math.sqrt(1.0 - e2 * sin_lat * sin_lat)
    alt = p / max(math.cos(lat), 1e-9) - n
    return math.degrees(lat), ((math.degrees(lon) + 540.0) % 360.0) - 180.0, alt


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


def is_pluto_tunable(hz: Any) -> bool:
    return isinstance(hz, int) and PLUTO_MIN_HZ <= hz <= PLUTO_MAX_HZ


def transmitter_score(transmitter: dict[str, Any]) -> tuple[int, int, int, int]:
    downlink = transmitter.get("downlink_hz")
    alive = transmitter.get("alive") is True
    active = str(transmitter.get("status", "")).lower() == "active"
    tunable = is_pluto_tunable(downlink)
    has_uplink = isinstance(transmitter.get("uplink_hz"), int)
    uhf_vhf = isinstance(downlink, int) and 100_000_000 <= downlink <= 500_000_000
    return (
        0 if tunable else 1,
        0 if alive or active else 1,
        0 if has_uplink else 1,
        0 if uhf_vhf else 1,
    )


def primary_radio_target(satellite: dict[str, Any]) -> dict[str, Any] | None:
    transmitters = [
        tx for tx in satellite.get("transmitters", [])
        if isinstance(tx.get("downlink_hz"), int)
    ]
    if not transmitters:
        return None

    tx = sorted(transmitters, key=transmitter_score)[0]
    downlink = tx.get("downlink_hz")
    return {
        "downlink_hz": downlink,
        "uplink_hz": tx.get("uplink_hz"),
        "mode": tx.get("mode"),
        "description": tx.get("description"),
        "type": tx.get("type"),
        "status": tx.get("status"),
        "alive": tx.get("alive"),
        "invert": tx.get("invert"),
        "pluto_tunable": is_pluto_tunable(downlink),
    }


def build_doppler_plan(samples: list[dict[str, Any]], radio: dict[str, Any] | None) -> dict[str, Any] | None:
    if not samples or not radio:
        return None

    downlink = radio.get("downlink_hz")
    uplink = radio.get("uplink_hz")
    if not isinstance(downlink, int):
        return None

    points: list[dict[str, Any]] = []
    for index, sample in enumerate(samples):
        if len(samples) == 1:
            range_rate_km_s = 0.0
        elif index == 0:
            dt_s = (samples[1]["time"] - sample["time"]).total_seconds()
            range_rate_km_s = (samples[1]["range_km"] - sample["range_km"]) / dt_s
        elif index == len(samples) - 1:
            dt_s = (sample["time"] - samples[index - 1]["time"]).total_seconds()
            range_rate_km_s = (sample["range_km"] - samples[index - 1]["range_km"]) / dt_s
        else:
            dt_s = (samples[index + 1]["time"] - samples[index - 1]["time"]).total_seconds()
            range_rate_km_s = (samples[index + 1]["range_km"] - samples[index - 1]["range_km"]) / dt_s

        rx_hz = round(downlink * (1.0 - range_rate_km_s / SPEED_OF_LIGHT_KM_S))
        point: dict[str, Any] = {
            "time_utc": iso_utc(sample["time"]),
            "azimuth_deg": round(float(sample["azimuth_deg"]), 1),
            "elevation_deg": round(float(sample["elevation_deg"]), 1),
            "range_km": round(float(sample["range_km"]), 1),
            "range_rate_km_s": round(range_rate_km_s, 4),
            "rx_hz": int(rx_hz),
            "rx_offset_hz": int(rx_hz - downlink),
        }
        if isinstance(uplink, int):
            tx_hz = round(uplink * (1.0 + range_rate_km_s / SPEED_OF_LIGHT_KM_S))
            point["tx_hz"] = int(tx_hz)
            point["tx_offset_hz"] = int(tx_hz - uplink)
        points.append(point)

    return {
        "model": "range-rate",
        "range_rate_sign": "positive_when_satellite_recedes",
        "downlink_hz": downlink,
        "uplink_hz": uplink if isinstance(uplink, int) else None,
        "points": points,
    }


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


def predict_satellite_passes(
    satellite: dict[str, Any],
    observer: dict[str, Any],
    start: dt.datetime,
    hours: float,
    step_seconds: int,
    pass_sample_seconds: int,
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
        sat_lat_deg, sat_lon_deg, sat_alt_km = eci_to_geodetic(sat_pos, when)
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
                "samples": [],
            }
            in_pass = True

        if visible and in_pass and current is not None:
            current["los_utc"] = when
            current["los_azimuth_deg"] = az
            current["samples"].append(
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
            current = None
            in_pass = False

        last_time = when
        last_el = el
        when += step

    if in_pass and current is not None:
        duration_s = max(0, int((current["los_utc"] - current["aos_utc"]).total_seconds()))
        if duration_s >= 60:
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

    return passes


def format_pass(satellite: dict[str, Any], row: dict[str, Any], duration_s: int) -> dict[str, Any]:
    radio = primary_radio_target(satellite)
    doppler_plan = build_doppler_plan(row.get("samples", []), radio)
    ground_track = [
        {
            "time_utc": iso_utc(sample["time"]),
            "latitude_deg": round(float(sample["sat_latitude_deg"]), 3),
            "longitude_deg": round(float(sample["sat_longitude_deg"]), 3),
            "altitude_km": round(float(sample["sat_altitude_km"]), 1),
            "azimuth_deg": round(float(sample["azimuth_deg"]), 1),
            "elevation_deg": round(float(sample["elevation_deg"]), 1),
        }
        for sample in row.get("samples", [])
    ]
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
        "radio": radio,
        "doppler_plan": doppler_plan,
        "ground_track": ground_track,
    }


def load_json(path: str) -> Any:
    return json.loads(Path(path).read_text(encoding="utf-8"))


# MINIMUM_PASS_COUNT_REFRESH_V2_5_5
def build_predictions_for_window(
    catalog: dict[str, Any],
    observer: dict[str, Any],
    start: dt.datetime,
    hours: float,
    step_seconds: int,
    pass_sample_seconds: int,
) -> list[dict[str, Any]]:
    passes: list[dict[str, Any]] = []
    for satellite in catalog.get("satellites", []):
        passes.extend(
            predict_satellite_passes(
                satellite,
                observer,
                start,
                hours,
                step_seconds,
                pass_sample_seconds,
            )
        )
    passes.sort(key=lambda row: row["aos_utc"])
    return passes


def build_predictions(args: argparse.Namespace) -> dict[str, Any]:
    observer = load_json(args.observer)
    catalog = load_json(args.catalog)
    start = parse_utc(args.start_utc) if args.start_utc else utc_now()

    requested_hours = max(0.1, float(args.hours))
    max_hours = max(requested_hours, float(args.max_hours))
    min_passes = max(0, int(args.min_passes))
    effective_hours = requested_hours
    passes: list[dict[str, Any]] = []

    while True:
        passes = build_predictions_for_window(
            catalog,
            observer,
            start,
            effective_hours,
            args.step_seconds,
            args.pass_sample_seconds,
        )
        if min_passes <= 0 or len(passes) >= min_passes or effective_hours >= max_hours:
            break
        if effective_hours < 1.0:
            effective_hours = min(1.0, max_hours)
        else:
            effective_hours = min(max_hours, effective_hours * 2.0)

    if args.limit > 0:
        passes = passes[: args.limit]

    return {
        "version": 1,
        "generated_utc": iso_utc(utc_now()),
        "start_utc": iso_utc(start),
        "hours": effective_hours,
        "requested_hours": requested_hours,
        "max_hours": max_hours,
        "min_passes": min_passes,
        "observer": observer,
        "metadata": {
            "minimum_elevation_deg": observer.get("minimum_elevation_deg", 10),
            "satellite_count": len(catalog.get("satellites", [])),
            "pass_count": len(passes),
            "step_seconds": args.step_seconds,
            "pass_sample_seconds": args.pass_sample_seconds,
            "requested_hours": requested_hours,
            "effective_hours": effective_hours,
            "max_hours": max_hours,
            "min_passes": min_passes,
        },
        "passes": passes,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", default="data/satellites.json")
    parser.add_argument("--observer", default="config/observer.example.json")
    parser.add_argument("--output", default="data/passes.json")
    parser.add_argument("--hours", type=float, default=24.0)
    parser.add_argument("--max-hours", type=float, default=24.0)
    parser.add_argument("--min-passes", type=int, default=0)
    parser.add_argument("--limit", type=int, default=80)
    parser.add_argument("--step-seconds", type=int, default=30)
    parser.add_argument("--pass-sample-seconds", type=int, default=5)
    parser.add_argument("--start-utc", default=None)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    predictions = build_predictions(args)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)

    # PASS_JSON_ATOMIC_REFRESH_V1:
    # Never expose a partially written passes.json to the HTTP backend.
    # The UI polls /api/passes while refreshes are running, so direct writes can
    # produce transient or persistent invalid JSON. Build and validate the full
    # JSON document first, then atomically replace the final file.
    body = json.dumps(predictions, indent=2, sort_keys=True) + "\n"
    json.loads(body)

    tmp_output = output.with_name(output.name + ".tmp")
    try:
        with tmp_output.open("w", encoding="utf-8") as handle:
            handle.write(body)
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(tmp_output, output)
    except Exception:
        try:
            tmp_output.unlink()
        except FileNotFoundError:
            pass
        raise
    meta = predictions["metadata"]
    print(f"wrote {output}")
    print(f"passes: {meta['pass_count']}")
    print(
        f"window: {predictions['start_utc']} + {predictions['hours']}h "
        f"(requested {predictions.get('requested_hours', predictions['hours'])}h, "
        f"min_passes {predictions.get('min_passes', 0)})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
