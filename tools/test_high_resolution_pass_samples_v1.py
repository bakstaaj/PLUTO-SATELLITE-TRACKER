#!/usr/bin/env python3
# Local high-resolution pass generation test.

from __future__ import annotations

import json
import subprocess
import sys
import tempfile
from pathlib import Path


def main() -> int:
    root = Path.cwd()
    script = root / "tools" / "update_pass_predictions.py"
    catalog = root / "data" / "satellites.json"
    observer_candidates = [
        root / "config" / "observer.json",
        root / "config" / "observer.example.json",
    ]
    observer = next((path for path in observer_candidates if path.exists()), None)

    if not script.exists():
        print(f"FAIL: missing {script}")
        return 1
    if not catalog.exists():
        print(f"FAIL: missing {catalog}")
        return 1
    if not observer:
        print("FAIL: missing observer config")
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        output = Path(tmp) / "passes.json"
        cmd = [
            sys.executable,
            str(script),
            "--catalog",
            str(catalog),
            "--observer",
            str(observer),
            "--output",
            str(output),
            "--hours",
            "24",
            "--limit",
            "20",
            "--step-seconds",
            "30",
            "--pass-sample-seconds",
            "5",
        ]
        print("Running:", " ".join(cmd))
        result = subprocess.run(cmd, text=True, capture_output=True, timeout=120)
        if result.stdout:
            print(result.stdout.strip())
        if result.stderr:
            print(result.stderr.strip())
        if result.returncode != 0:
            print(f"FAIL: generator returned {result.returncode}")
            return 1

        payload = json.loads(output.read_text(encoding="utf-8"))
        passes = payload.get("passes") or []
        print(f"Generated passes: {len(passes)}")
        print(f"Metadata pass_sample_seconds: {payload.get('metadata', {}).get('pass_sample_seconds')}")
        if payload.get("metadata", {}).get("pass_sample_seconds") != 5:
            print("FAIL: metadata pass_sample_seconds is not 5")
            return 1
        if not passes:
            print("FAIL: no passes generated")
            return 1

        best = max(passes, key=lambda row: len(row.get("ground_track") or []))
        ground_count = len(best.get("ground_track") or [])
        doppler_count = len(((best.get("doppler_plan") or {}).get("points")) or [])
        print(f"Best pass: {best.get('name')} ground_track={ground_count} doppler={doppler_count}")

        if ground_count <= 4:
            print("FAIL: ground_track still has 4 or fewer points")
            return 1
        if doppler_count <= 4:
            print("FAIL: doppler_plan still has 4 or fewer points")
            return 1

        print("PASS: generated high-resolution ground_track and Doppler plan")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
