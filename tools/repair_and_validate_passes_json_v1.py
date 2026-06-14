#!/usr/bin/env python3
"""
Repair and validate Pluto /api/passes after atomic pass JSON patch.

This script:
  - syncs Pluto time
  - queues pass refresh
  - waits for refresh status to become ok
  - repeatedly requests /api/passes
  - validates JSON parse and basic pass count

Run from repo root after deploy/reboot:
  source .pluto.env
  python tools/repair_and_validate_passes_json_v1.py
"""

from __future__ import annotations

import json
import os
import time
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


def request_json(path: str, method: str = "GET", timeout: int = 30) -> dict:
    req = urllib.request.Request(base + path, data=(b"" if method == "POST" else None), method=method)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        text = resp.read().decode("utf-8", "replace")
        return json.loads(text)


def main() -> int:
    print(f"Pluto HTTP: {base}")

    epoch = int(time.time())
    print(f"Syncing time: {epoch}")
    print(request_json(f"/api/time/sync?epoch={epoch}"))

    print("Queuing pass refresh...")
    print(request_json("/api/refresh/passes", method="POST"))

    last = {}
    for i in range(90):
        time.sleep(2)
        try:
            last = request_json("/api/refresh/status")
        except Exception as exc:
            print(f"  refresh status read failed: {exc}")
            continue
        print(f"  refresh state={last.get('state')} target={last.get('target')} message={last.get('message')}")
        if last.get("state") == "ok" and last.get("target") == "passes":
            break
        if last.get("state") == "failed":
            print("FAIL: pass refresh failed")
            return 1
    else:
        print("FAIL: pass refresh did not complete")
        return 1

    print("Validating /api/passes JSON repeatedly...")
    good = 0
    last_count = -1
    for i in range(8):
        try:
            payload = request_json("/api/passes", timeout=30)
        except json.JSONDecodeError as exc:
            print(f"FAIL: /api/passes invalid JSON on attempt {i + 1}: {exc}")
            return 1
        except urllib.error.HTTPError as exc:
            print(f"FAIL: /api/passes HTTP {exc.code}")
            return 1
        passes = payload.get("passes", [])
        last_count = len(passes)
        print(f"  attempt {i + 1}: valid JSON, pass_count={last_count}, generated={payload.get('generated_utc')}")
        good += 1
        time.sleep(0.5)

    if good >= 8:
        print(f"PASS: /api/passes valid JSON across {good} reads; pass_count={last_count}")
        return 0

    print("FAIL: insufficient successful reads")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
