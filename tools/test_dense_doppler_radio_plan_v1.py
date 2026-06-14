#!/usr/bin/env python3
# Browser/API sanity check for dense Doppler radio plan patch.

from __future__ import annotations

import json
import os
import urllib.request
from pathlib import Path


def load_env_file(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.exists():
        return out
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, val = line.split("=", 1)
        out[key.strip()] = val.strip().strip('"').strip("'")
    return out


env = load_env_file(Path(".pluto.env"))
ip = os.environ.get("PLUTO_IP") or env.get("PLUTO_IP") or "192.168.2.1"
base = f"http://{ip}:8080"


def get(path: str) -> str:
    with urllib.request.urlopen(base + path, timeout=30) as resp:
        return resp.read().decode("utf-8", "replace")


def main() -> int:
    print(f"Pluto HTTP: {base}")
    html = get("/SatelliteTracker/")
    checks = [
        ("dense Doppler patch present", "DENSE_DOPPLER_RADIO_PLAN_V1" in html),
        ("dense plan helper served", "densifyDopplerPlanForRadioV1" in html),
        ("radio step is 5 seconds", "RADIO_DOPPLER_STEP_SECONDS_V1 = 5" in html),
    ]
    failed = False
    for name, ok in checks:
        print(("PASS: " if ok else "FAIL: ") + name)
        failed = failed or not ok

    payload = json.loads(get("/api/passes"))
    passes = payload.get("passes") or []
    print(f"Pass count: {len(passes)} generated={payload.get('generated_utc')}")
    if not passes:
        print("FAIL: no passes available")
        return 1

    plan = passes[0].get("doppler_plan") or {}
    points = plan.get("points") or []
    if len(points) < 2:
        print("FAIL: first pass has fewer than 2 Doppler points")
        return 1
    print(f"PASS: first pass has {len(points)} source Doppler points for browser densification")

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
