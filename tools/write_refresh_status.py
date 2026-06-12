#!/usr/bin/env python3
"""Write a compact refresh status summary for the Pluto web UI."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


def load_json(path: str) -> Any:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def mapping_or_empty(value: Any) -> dict[str, Any]:
    return value if isinstance(value, dict) else {}


def build_passes_summary(payload: dict[str, Any]) -> dict[str, Any]:
    meta = payload.get("metadata") or {}
    return {
        "generated_utc": payload.get("generated_utc"),
        "start_utc": payload.get("start_utc"),
        "prediction_hours": payload.get("hours"),
        "pass_count": meta.get("pass_count"),
        "satellite_count": meta.get("satellite_count"),
        "minimum_elevation_deg": meta.get("minimum_elevation_deg"),
    }


def build_catalog_summary(payload: dict[str, Any]) -> dict[str, Any]:
    meta = payload.get("metadata") or {}
    sources = payload.get("sources") or {}
    celestrak = mapping_or_empty(sources.get("celestrak_amateur_tle"))
    satnogs = mapping_or_empty(sources.get("satnogs_transmitters"))
    return {
        "catalog_updated_utc": payload.get("updated_utc"),
        "satellite_count": meta.get("satellite_count"),
        "satellites_with_transmitters": meta.get("satellites_with_transmitters"),
        "transmitter_count": meta.get("transmitter_count"),
        "celestrak_retrieved_utc": celestrak.get("retrieved_utc"),
        "satnogs_status": satnogs.get("status"),
        "satnogs_retrieved_utc": satnogs.get("retrieved_utc"),
        "satnogs_warning": satnogs.get("warning"),
    }


def build_status(target: str, payload: dict[str, Any]) -> dict[str, Any]:
    if target == "passes":
        summary = build_passes_summary(payload)
        updated_utc = summary.get("generated_utc")
        message = "Pass predictions regenerated on Pluto"
    elif target == "catalog":
        summary = build_catalog_summary(payload)
        updated_utc = summary.get("catalog_updated_utc")
        message = "Catalog and TLE data refreshed on Pluto"
        if summary.get("satnogs_status") == "warning" and summary.get("satnogs_warning"):
            message = f"{message} (SatNOGS warning: {summary['satnogs_warning']})"
    else:
        raise ValueError(f"unsupported refresh target: {target}")

    return {
        "ok": True,
        "state": "ok",
        "target": target,
        "updated_utc": updated_utc,
        "message": message,
        "summary": summary,
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", required=True, choices=["passes", "catalog"])
    parser.add_argument("--input", required=True, help="Generated JSON file to summarize.")
    parser.add_argument("--status-file", required=True, help="Refresh status JSON path.")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    payload = load_json(args.input)
    status = build_status(args.target, payload)
    output = Path(args.status_file)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(status, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
