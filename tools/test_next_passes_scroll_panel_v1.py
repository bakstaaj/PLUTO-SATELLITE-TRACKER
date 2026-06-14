#!/usr/bin/env python3
# Browser sanity check for Next Passes scroll panel.

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
        ("patch marker served", "NEXT_PASSES_SCROLL_PANEL_V1" in html),
        ("max-height served", "max-height: calc(100vh - 155px);" in html),
        ("scroll enabled served", "overflow-y: auto;" in html),
        ("sticky filter served", "#passes.pass-list .filter-row" in html and "position: sticky;" in html),
    ]
    failed = False
    for name, ok in checks:
        print(("PASS: " if ok else "FAIL: ") + name)
        failed = failed or not ok
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
