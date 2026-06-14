#!/usr/bin/env python3
"""
HTTP smoke test for UI_STREAMING_PCM_RECONNECT_V3.

This reads several backend stream segments in the same way the UI reconnect loop
will do, confirming the stream can be reopened repeatedly.
"""

from __future__ import annotations

import json
import os
import time
import urllib.parse
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
        k, v = line.split("=", 1)
        out[k.strip()] = v.strip().strip('"').strip("'")
    return out

env = load_env_file(Path(".pluto.env"))
ip = os.environ.get("PLUTO_IP") or env.get("PLUTO_IP") or "192.168.2.1"
base = f"http://{ip}:8080"
hz = int(os.environ.get("PLUTO_AUDIO_TEST_HZ", "435352000"))
name = os.environ.get("PLUTO_AUDIO_TEST_NAME", "MOZHAETS 4 (RS22)")
mode = os.environ.get("PLUTO_AUDIO_TEST_MODE", "CW")

def get_json(url: str, method: str = "GET", timeout: int = 20):
    data = b"" if method == "POST" else None
    req = urllib.request.Request(url, data=data, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        body = r.read().decode("utf-8", "replace")
        print(body.strip())
        return json.loads(body)

print(f"Pluto HTTP: {base}")
print(f"Target: {name} {hz} Hz {mode}")

params = urllib.parse.urlencode({
    "name": name,
    "norad": "0",
    "aos": "manual",
    "downlink": str(hz),
    "mode": mode,
    "description": "ui streaming pcm reconnect smoke test",
})
print("Planning target...")
get_json(f"{base}/api/radio/plan?{params}")

print("Starting backend DSP...")
get_json(f"{base}/api/radio/audio/live/start?downlink_hz={hz}", method="POST")

try:
    ok_segments = 0
    total_bytes = 0
    for reconnect in range(3):
        time.sleep(0.6)
        url = f"{base}/api/radio/audio/live.wav?stream=1&downlink_hz={hz}&request={int(time.time()*1000)}&reconnect={reconnect}"
        print(f"Opening segment {reconnect}: {url}")
        with urllib.request.urlopen(url, timeout=20) as r:
            data = r.read(4096)
            total_bytes += len(data)
            print(f"HTTP {r.status} {r.reason}")
            print("Content-Type:", r.headers.get("Content-Type"))
            print("Bytes read:", len(data))
            print("First bytes:", data[:32])
            if r.status == 200 and data.startswith(b"RIFF") and b"WAVE" in data[:32] and len(data) >= 44:
                ok_segments += 1
    if ok_segments >= 2:
        print(f"PASS: backend stream can be reopened repeatedly ({ok_segments}/3 segments, {total_bytes} bytes)")
        raise SystemExit(0)
    print(f"FAIL: only {ok_segments}/3 stream segments opened correctly")
    raise SystemExit(1)
finally:
    print("Stopping backend DSP...")
    try:
        get_json(f"{base}/api/radio/audio/live/stop", method="POST")
    except Exception as exc:
        print(f"WARN: stop failed: {exc}")
