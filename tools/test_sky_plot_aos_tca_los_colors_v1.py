#!/usr/bin/env python3
# Browser sanity check for sky plot AOS/TCA/LOS colors.

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
        ("patch marker served", "SKY_PLOT_AOS_TCA_LOS_COLORS_V1" in html),
        ("AOS/TCA/LOS classes served", "sky-aos-dot" in html and "sky-tca-dot" in html and "sky-los-dot" in html),
        ("helper served", "skySpecialMarkerClassForPoint" in html),
        ("legend colors served", "background:#1a7f37" in html and "background:#a15c00" in html and "background:#7d3ad3" in html),
    ]
    failed = False
    for name, ok in checks:
        print(("PASS: " if ok else "FAIL: ") + name)
        failed = failed or not ok
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
