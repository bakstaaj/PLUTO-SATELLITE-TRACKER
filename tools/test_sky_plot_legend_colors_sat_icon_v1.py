#!/usr/bin/env python3
# Browser sanity check for sky plot legend colors and satellite icon.

from __future__ import annotations

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
        ("patch marker served", "SKY_PLOT_LEGEND_COLORS_SAT_ICON_V1" in html),
        ("sky classes served", "sky-pass-path" in html and "sky-pass-progress" in html and "sky-look-line" in html),
        ("satellite icon helper served", "renderSkySatelliteIcon" in html),
        ("satellite icon classes served", "satellite-body" in html and "satellite-panel" in html),
        ("compact legend served", ">Path</span>" in html and ">Progress</span>" in html and ">Satellite</span>" in html),
    ]
    failed = False
    for name, ok in checks:
        print(("PASS: " if ok else "FAIL: ") + name)
        failed = failed or not ok
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
