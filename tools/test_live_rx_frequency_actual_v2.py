#!/usr/bin/env python3
from __future__ import annotations

import os
import urllib.request
from pathlib import Path

def load_env_file(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    if not path.exists(): return out
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line: continue
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
        ("patch marker served", "LIVE_RX_FREQUENCY_ACTUAL_V2" in html),
        ("Live RX row served", "<strong>Live RX</strong>" in html),
        ("live RX helper served", "liveRxLabelForMapInfoActualV2" in html),
        ("track-state RX source served", "lastTrackState && lastTrackState.rx_hz" in html),
    ]
    failed = False
    for name, ok in checks:
        print(("PASS: " if ok else "FAIL: ") + name)
        failed = failed or not ok
    return 1 if failed else 0

if __name__ == "__main__":
    raise SystemExit(main())
