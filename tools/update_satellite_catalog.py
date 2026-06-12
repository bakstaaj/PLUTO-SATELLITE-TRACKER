#!/usr/bin/env python3
"""Build the local satellite catalog from public orbital and transmitter data."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import sys
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


CELESTRAK_AMATEUR_TLE = (
    "https://celestrak.org/NORAD/elements/gp.php?GROUP=amateur&FORMAT=tle"
)
SATNOGS_TRANSMITTERS = "https://db.satnogs.org/api/transmitters/"


def utc_now_iso() -> str:
    return dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def fetch_text(url: str, timeout: int) -> str:
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": "PlutoSatelliteTracker/0.1 (+https://github.com/bakstaaj/PLUTO-SATELLITE-TRACKER)"
        },
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        charset = response.headers.get_content_charset() or "utf-8"
        return response.read().decode(charset, errors="replace")


def fetch_json(url: str, timeout: int) -> Any:
    return json.loads(fetch_text(url, timeout))


def parse_tle_catalog(text: str) -> dict[int, dict[str, Any]]:
    lines = [line.rstrip() for line in text.splitlines() if line.strip()]
    catalog: dict[int, dict[str, Any]] = {}
    idx = 0

    while idx + 2 < len(lines):
        name = lines[idx].strip()
        line1 = lines[idx + 1].strip()
        line2 = lines[idx + 2].strip()
        idx += 3

        if not line1.startswith("1 ") or not line2.startswith("2 "):
            continue

        try:
            norad_id = int(line1[2:7])
        except ValueError:
            continue

        catalog[norad_id] = {
            "norad_id": norad_id,
            "name": name,
            "tle": {
                "line1": line1,
                "line2": line2,
            },
            "transmitters": [],
            "modes": [],
        }

    return catalog


def paged_satnogs_transmitters(timeout: int, max_pages: int) -> tuple[list[dict[str, Any]], int]:
    transmitters: list[dict[str, Any]] = []
    url: str | None = SATNOGS_TRANSMITTERS
    pages = 0

    while url and pages < max_pages:
        pages += 1
        payload = fetch_json(url, timeout)
        if isinstance(payload, dict) and isinstance(payload.get("results"), list):
            transmitters.extend(row for row in payload["results"] if isinstance(row, dict))
            next_url = payload.get("next")
            url = str(next_url) if next_url else None
        elif isinstance(payload, list):
            transmitters.extend(row for row in payload if isinstance(row, dict))
            url = None
        else:
            raise ValueError("Unexpected SatNOGS transmitters response shape")

    return transmitters, pages


def first_present(row: dict[str, Any], names: list[str]) -> Any:
    for name in names:
        value = row.get(name)
        if value not in (None, ""):
            return value
    return None


def normalize_frequency(value: Any) -> float | None:
    if value in (None, ""):
        return None
    try:
        hz = float(value)
    except (TypeError, ValueError):
        return None
    if hz <= 0:
        return None
    return hz


def compact_transmitter(row: dict[str, Any]) -> dict[str, Any] | None:
    downlink_hz = normalize_frequency(
        first_present(row, ["downlink_low", "downlink", "downlink_high"])
    )
    uplink_hz = normalize_frequency(first_present(row, ["uplink_low", "uplink", "uplink_high"]))
    mode = first_present(row, ["mode", "modulation"])
    description = first_present(row, ["description", "citation", "service"])

    if downlink_hz is None and uplink_hz is None:
        return None

    compact: dict[str, Any] = {}
    if description:
        compact["description"] = str(description)
    if mode:
        compact["mode"] = str(mode)
    if downlink_hz is not None:
        compact["downlink_hz"] = int(round(downlink_hz))
    if uplink_hz is not None:
        compact["uplink_hz"] = int(round(uplink_hz))

    for src, dest in [
        ("baud", "baud"),
        ("type", "type"),
        ("status", "status"),
        ("alive", "alive"),
        ("invert", "invert"),
    ]:
        value = row.get(src)
        if value not in (None, ""):
            compact[dest] = value

    return compact


def build_catalog(args: argparse.Namespace) -> dict[str, Any]:
    tle_text = fetch_text(args.celestrak_url, args.timeout)
    celestrak_retrieved_utc = utc_now_iso()
    satellites = parse_tle_catalog(tle_text)
    if not satellites:
        raise RuntimeError("No CelesTrak amateur TLE records were parsed")

    transmitter_count = 0
    satnogs_source: dict[str, Any] | None = None
    if not args.skip_satnogs:
        try:
            rows, pages = paged_satnogs_transmitters(args.timeout, args.max_satnogs_pages)
            satnogs_retrieved_utc = utc_now_iso()
            for row in rows:
                norad_raw = first_present(row, ["norad_cat_id", "norad_id", "norad"])
                try:
                    norad_id = int(norad_raw)
                except (TypeError, ValueError):
                    continue

                satellite = satellites.get(norad_id)
                if not satellite:
                    continue

                transmitter = compact_transmitter(row)
                if not transmitter:
                    continue

                satellite["transmitters"].append(transmitter)
                transmitter_count += 1

                mode = transmitter.get("mode")
                if mode and mode not in satellite["modes"]:
                    satellite["modes"].append(mode)

            satnogs_source = {
                "url": SATNOGS_TRANSMITTERS,
                "status": "ok",
                "retrieved_utc": satnogs_retrieved_utc,
                "page_count": pages,
                "transmitters_seen": len(rows),
                "transmitters_attached": transmitter_count,
            }
        except (urllib.error.URLError, TimeoutError, ValueError, json.JSONDecodeError) as exc:
            print(f"warning: SatNOGS transmitter import skipped: {exc}", file=sys.stderr)
            satnogs_source = {
                "url": SATNOGS_TRANSMITTERS,
                "status": "warning",
                "warning": str(exc),
            }
    else:
        satnogs_source = {
            "url": SATNOGS_TRANSMITTERS,
            "status": "skipped",
        }

    rows = sorted(satellites.values(), key=lambda row: row["name"].upper())
    with_tx = sum(1 for row in rows if row["transmitters"])

    return {
        "version": 1,
        "updated_utc": utc_now_iso(),
        "sources": {
            "celestrak_amateur_tle": {
                "url": args.celestrak_url,
                "status": "ok",
                "retrieved_utc": celestrak_retrieved_utc,
                "satellite_records": len(satellites),
            },
            "satnogs_transmitters": satnogs_source,
        },
        "metadata": {
            "satellite_count": len(rows),
            "satellites_with_transmitters": with_tx,
            "transmitter_count": transmitter_count,
        },
        "satellites": rows,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        default="data/satellites.json",
        help="Output compact catalog JSON path.",
    )
    parser.add_argument(
        "--celestrak-url",
        default=CELESTRAK_AMATEUR_TLE,
        help="CelesTrak amateur TLE URL.",
    )
    parser.add_argument(
        "--skip-satnogs",
        action="store_true",
        help="Only import CelesTrak TLEs.",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=30,
        help="Network timeout in seconds.",
    )
    parser.add_argument(
        "--max-satnogs-pages",
        type=int,
        default=25,
        help="Safety cap for paginated SatNOGS transmitter reads.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    catalog = build_catalog(args)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(catalog, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    meta = catalog["metadata"]
    print(f"wrote {output}")
    print(f"satellites: {meta['satellite_count']}")
    print(f"satellites with transmitters: {meta['satellites_with_transmitters']}")
    print(f"transmitters attached: {meta['transmitter_count']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
