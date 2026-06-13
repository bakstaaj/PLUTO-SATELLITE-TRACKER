#!/usr/bin/env python3
"""HTTP-only test for non-blocking Pluto pass refresh.

This simulates the browser-owned flow: sync time over HTTP, trigger pass refresh,
verify that the HTTP endpoint returns quickly, then poll until the quick preview
and full 24-hour refresh appear.
"""
from __future__ import annotations

import json
import os
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


def load_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, value = line.split("=", 1)
            values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def http_json(url: str, method: str = "GET", payload: Any | None = None, timeout: float = 15.0) -> Any:
    data = None
    headers = {"Cache-Control": "no-store"}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as response:
        text = response.read().decode("utf-8", errors="replace")
    return json.loads(text)


def summarize(base: str) -> dict[str, Any]:
    status = http_json(f"{base}/api/refresh/status", timeout=8)
    passes = http_json(f"{base}/api/passes", timeout=8)
    rows = passes.get("passes") or []
    meta = passes.get("metadata") or {}
    first = rows[0] if rows else {}
    radio = first.get("radio") or {}
    return {
        "state": status.get("state"),
        "message": status.get("message"),
        "updated_utc": status.get("updated_utc"),
        "generated_utc": passes.get("generated_utc"),
        "start_utc": passes.get("start_utc"),
        "hours": passes.get("hours"),
        "pass_count": meta.get("pass_count", len(rows)),
        "satellite_count": meta.get("satellite_count"),
        "step_seconds": meta.get("step_seconds"),
        "first_pass": {
            "name": first.get("name"),
            "aos_utc": first.get("aos_utc"),
            "downlink_hz": radio.get("downlink_hz") or (first.get("downlinks_hz") or [None])[0],
        } if first else None,
    }


def iso_epoch(value: str | None) -> int:
    if not value:
        return 0
    try:
        from datetime import datetime, timezone
        return int(datetime.fromisoformat(value.replace("Z", "+00:00")).astimezone(timezone.utc).timestamp())
    except Exception:
        return 0


def wait_for(base: str, kind: str, start_epoch: int, timeout_s: int) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_s
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        try:
            last = summarize(base)
            print(f"  state={last.get('state')} count={last.get('pass_count')} hours={last.get('hours')} step={last.get('step_seconds')} start={last.get('start_utc')}")
            start_ok = abs(iso_epoch(last.get("start_utc")) - start_epoch) <= 10 * 60
            if kind == "quick":
                if start_ok and last.get("hours") == 4.0 and last.get("pass_count", 0) >= 1 and last.get("step_seconds") == 120:
                    return last
            else:
                if start_ok and float(last.get("hours") or 0) >= 23 and int(last.get("pass_count") or 0) >= 20 and last.get("step_seconds") == 60:
                    return last
        except Exception as exc:
            print(f"  poll warning: {exc}")
        time.sleep(4)
    raise SystemExit(f"FAIL: timed out waiting for {kind} refresh. Last summary:\n{json.dumps(last, indent=2, sort_keys=True)}")


def main() -> int:
    env = load_env(Path(".pluto.env"))
    ip = os.environ.get("PLUTO_IP") or env.get("PLUTO_IP") or "192.168.2.1"
    base = f"http://{ip}:8080"
    print(f"Pluto HTTP: {base}")

    epoch = int(time.time())
    print(f"Simulating browser time sync epoch={epoch} ...")
    print(json.dumps(http_json(f"{base}/api/time/sync?epoch={epoch}", timeout=10), indent=2, sort_keys=True))

    print("Triggering non-blocking /api/refresh/passes ...")
    started = time.monotonic()
    result = http_json(f"{base}/api/refresh/passes", method="POST", payload={}, timeout=10)
    elapsed = time.monotonic() - started
    print(f"Refresh endpoint returned in {elapsed:.1f}s")
    print(json.dumps(result, indent=2, sort_keys=True))
    if elapsed > 5.0:
        raise SystemExit("FAIL: refresh endpoint still blocked too long; expected background queue return under 5s")

    print("Waiting for quick preview to publish ...")
    quick = wait_for(base, "quick", epoch, timeout_s=90)
    print("--- quick preview summary ---")
    print(json.dumps(quick, indent=2, sort_keys=True))

    print("Waiting for full 24-hour refresh to publish ...")
    full = wait_for(base, "full", epoch, timeout_s=180)
    print("--- full refresh summary ---")
    print(json.dumps(full, indent=2, sort_keys=True))

    print("PASS: non-blocking browser-time quick/full refresh completed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
