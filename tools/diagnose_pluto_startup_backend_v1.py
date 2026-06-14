#!/usr/bin/env python3
"""
Diagnose Pluto backend/UI startup APIs.

Run from repo root:
  source .pluto.env
  python tools/diagnose_pluto_startup_backend_v1.py
"""

from __future__ import annotations

import json
import os
import urllib.error
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


def get_text(path: str, timeout: int = 20) -> tuple[int, str, str]:
    url = base + path
    try:
        with urllib.request.urlopen(url, timeout=timeout) as r:
            return r.status, r.headers.get("Content-Type", ""), r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as exc:
        body = exc.read().decode("utf-8", "replace")
        return exc.code, exc.headers.get("Content-Type", ""), body


def show_json(path: str, timeout: int = 20) -> bool:
    print(f"\n== {path} ==")
    try:
        status, ctype, text = get_text(path, timeout=timeout)
        print(f"HTTP {status} {ctype}")
        if status >= 400:
            print(text[:1000])
            return False
        try:
            data = json.loads(text)
        except json.JSONDecodeError as exc:
            print(f"FAIL: invalid JSON: {exc}")
            print(text[:1200])
            return False
        if isinstance(data, dict):
            print("keys:", ", ".join(sorted(data.keys())[:20]))
            if "passes" in data:
                print("pass_count:", len(data.get("passes") or []))
                print("generated_utc:", data.get("generated_utc"))
            if "state" in data:
                print("state:", data.get("state"), "target:", data.get("target"), "message:", data.get("message"))
        print("PASS: valid JSON")
        return True
    except Exception as exc:
        print(f"FAIL: {exc}")
        return False


def main() -> int:
    print(f"Pluto HTTP: {base}")

    ok = True
    ok = show_json("/api/status") and ok
    ok = show_json("/api/refresh/status") and ok
    ok = show_json("/api/passes", timeout=60) and ok

    print("\n== web UI root ==")
    try:
        status, ctype, text = get_text("/SatelliteTracker/", timeout=20)
        print(f"HTTP {status} {ctype} bytes={len(text)}")
        print("PASS: UI route reachable" if status == 200 else "FAIL: UI route not 200")
        ok = ok and status == 200
    except Exception as exc:
        print(f"FAIL: {exc}")
        ok = False

    print("\n== Result ==")
    if ok:
        print("PASS: backend startup APIs are reachable and /api/passes parses")
        return 0

    print("FAIL: one or more startup APIs failed")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
