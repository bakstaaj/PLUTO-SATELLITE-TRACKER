#!/usr/bin/env python3
from __future__ import annotations
import json
import os
import time
import urllib.error
import urllib.request
from pathlib import Path


def load_env(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if path.exists():
        for raw in path.read_text(encoding='utf-8', errors='replace').splitlines():
            line = raw.strip()
            if not line or line.startswith('#') or '=' not in line:
                continue
            k, v = line.split('=', 1)
            values[k.strip()] = v.strip().strip('"').strip("'")
    return values


def http_json(base: str, path: str, method: str = 'GET', payload: object | None = None, timeout: float = 20.0) -> dict:
    data = None
    headers = {'Cache-Control': 'no-store'}
    if payload is not None:
        data = json.dumps(payload).encode('utf-8')
        headers['Content-Type'] = 'application/json'
    req = urllib.request.Request(base + path, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.loads(response.read().decode('utf-8'))


def iso_epoch(value: str | None) -> int:
    if not value:
        return 0
    from datetime import datetime, timezone
    try:
        return int(datetime.fromisoformat(value.replace('Z', '+00:00')).astimezone(timezone.utc).timestamp())
    except Exception:
        return 0


def compact(base: str) -> dict:
    passes = http_json(base, '/api/passes', timeout=10)
    status = http_json(base, '/api/refresh/status', timeout=10)
    rows = passes.get('passes') or []
    first = rows[0] if rows else {}
    radio = first.get('radio') or {}
    meta = passes.get('metadata') or {}
    return {
        'state': status.get('state'),
        'message': status.get('message'),
        'updated_utc': status.get('updated_utc'),
        'generated_utc': passes.get('generated_utc'),
        'start_utc': passes.get('start_utc'),
        'hours': passes.get('hours'),
        'pass_count': meta.get('pass_count', len(rows)),
        'satellite_count': meta.get('satellite_count'),
        'step_seconds': meta.get('step_seconds'),
        'first_pass': {
            'name': first.get('name'),
            'aos_utc': first.get('aos_utc'),
            'downlink_hz': radio.get('downlink_hz') or ((first.get('downlinks_hz') or [None])[0]),
        } if first else None,
    }


def main() -> int:
    env = load_env(Path('.pluto.env'))
    ip = os.environ.get('PLUTO_IP') or env.get('PLUTO_IP') or '192.168.2.1'
    base = f'http://{ip}:8080'
    print(f'Pluto HTTP: {base}')

    epoch = int(time.time())
    print(f'Simulating browser time sync epoch={epoch} ...')
    print(json.dumps(http_json(base, f'/api/time/sync?epoch={epoch}', timeout=10), indent=2, sort_keys=True))

    print('Triggering browser-owned /api/refresh/passes ...')
    t0 = time.time()
    endpoint = http_json(base, '/api/refresh/passes', method='POST', payload={}, timeout=30)
    elapsed = time.time() - t0
    print(f'Refresh endpoint returned in {elapsed:.1f}s')
    print(json.dumps(endpoint, indent=2, sort_keys=True))
    if elapsed > 10:
        raise SystemExit('FAIL: refresh endpoint blocked too long for quick preview')

    quick = compact(base)
    print('--- quick/current summary ---')
    print(json.dumps(quick, indent=2, sort_keys=True))
    now = int(time.time())
    start = iso_epoch(quick.get('start_utc'))
    if not start or abs(now - start) > 20 * 60:
        raise SystemExit('FAIL: quick/current pass window did not use current browser-synced UTC')

    print('Waiting for background full refresh to complete ...')
    deadline = time.time() + 720
    last = quick
    while time.time() < deadline:
        time.sleep(8)
        last = compact(base)
        print(f"  state={last.get('state')} count={last.get('pass_count')} hours={last.get('hours')} step={last.get('step_seconds')} start={last.get('start_utc')}")
        start = iso_epoch(last.get('start_utc'))
        if (last.get('state') == 'ok'
                and float(last.get('hours') or 0) >= 23
                and int(last.get('pass_count') or 0) >= 20
                and int(last.get('step_seconds') or 0) == 60
                and start and abs(int(time.time()) - start) < 20 * 60):
            print('--- full refresh summary ---')
            print(json.dumps(last, indent=2, sort_keys=True))
            print('PASS: HTTP/browser-time quick preview and background full refresh completed with current UTC')
            return 0
    print('--- last summary ---')
    print(json.dumps(last, indent=2, sort_keys=True))
    raise SystemExit('FAIL: timed out waiting for full current browser-time refresh')


if __name__ == '__main__':
    raise SystemExit(main())
