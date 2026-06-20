#!/usr/bin/env python3
"""Capture high-rate Pluto IQ/baseband windows for GMSK Phase 2 work.

This tool intentionally does not claim demodulation or HDLC/AX.25 decode.  It
captures interleaved int16 I/Q samples from the Pluto IIO stream so the real
GMSK clock recovery and slicer can be developed offline instead of from the
24 kHz browser audio stream.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import time
import urllib.error
import urllib.request

MARKER = "GMSK_HIGH_RATE_IQ_CAPTURE_V2_8_11"
DEFAULT_HOST = "192.168.68.104"
DEFAULT_PORT = "8080"
DEFAULT_USER = "root"
DEFAULT_PASSWORD = "analog"
DEFAULT_DEVICE = "cf-ad9361-lpc"
DEFAULT_REMOTE_URI = "ip:localhost"
DEFAULT_CHANNELS = ("voltage0", "voltage1")


def utc_stamp() -> str:
    return _dt.datetime.now(_dt.UTC).strftime("%Y%m%dT%H%M%SZ")


def load_dotenv(path: Path) -> dict[str, str]:
    values: dict[str, str] = {}
    if not path.exists():
        return values
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        value = value.strip().strip('"').strip("'")
        if key:
            values[key] = value
    return values


def env_defaults() -> dict[str, str]:
    values: dict[str, str] = {}
    here = Path.cwd()
    for candidate in (here / ".pluto.env", here.parent / ".pluto.env"):
        values.update(load_dotenv(candidate))
    values.update({k: v for k, v in os.environ.items() if k.startswith("PLUTO_")})
    return values


def read_json_url(url: str, timeout: float = 4.0) -> object:
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            body = response.read().decode("utf-8", errors="replace")
        return json.loads(body)
    except Exception as exc:  # noqa: BLE001 - metadata only
        return {"ok": False, "error": str(exc), "url": url}


def post_url(url: str, timeout: float = 4.0) -> object:
    req = urllib.request.Request(url, method="POST", data=b"")
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            body = response.read().decode("utf-8", errors="replace")
        return json.loads(body)
    except Exception as exc:  # noqa: BLE001 - metadata only
        return {"ok": False, "error": str(exc), "url": url}


def build_ssh_command(user: str, host: str, password: str | None, remote_command: str) -> list[str]:
    target = f"{user}@{host}"
    base: list[str]
    if password:
        base = ["sshpass", "-p", password, "ssh"]
    else:
        base = ["ssh"]
    base += [
        "-o", "StrictHostKeyChecking=no",
        "-o", "UserKnownHostsFile=/dev/null",
        "-o", "LogLevel=ERROR",
        target,
        remote_command,
    ]
    return base


def redact_command(cmd: list[str]) -> list[str]:
    redacted: list[str] = []
    skip_next = False
    for item in cmd:
        if skip_next:
            redacted.append("***")
            skip_next = False
            continue
        redacted.append(item)
        if item == "-p":
            skip_next = True
    return redacted


def analyze_iq_preview(path: Path, assumed_sample_rate: float, max_iq_samples: int = 250_000) -> dict[str, object]:
    data = path.read_bytes()[: max_iq_samples * 4]
    pair_count = len(data) // 4
    if pair_count <= 0:
        return {"ok": False, "error": "no complete int16 IQ pairs captured", "iq_sample_count_preview": 0}

    sum_i = 0.0
    sum_q = 0.0
    sum_i2 = 0.0
    sum_q2 = 0.0
    sum_mag2 = 0.0
    peak_i = 0
    peak_q = 0
    clip = 0
    last_phase = None
    phase_step_sum = 0.0
    phase_step_count = 0

    for i_val, q_val in struct.iter_unpack("<hh", data[: pair_count * 4]):
        fi = float(i_val)
        fq = float(q_val)
        sum_i += fi
        sum_q += fq
        sum_i2 += fi * fi
        sum_q2 += fq * fq
        mag2 = fi * fi + fq * fq
        sum_mag2 += mag2
        peak_i = max(peak_i, abs(i_val))
        peak_q = max(peak_q, abs(q_val))
        if abs(i_val) >= 32760 or abs(q_val) >= 32760:
            clip += 1
        phase = math.atan2(fq, fi)
        if last_phase is not None:
            d = phase - last_phase
            while d > math.pi:
                d -= 2 * math.pi
            while d < -math.pi:
                d += 2 * math.pi
            phase_step_sum += d
            phase_step_count += 1
        last_phase = phase

    mean_i = sum_i / pair_count
    mean_q = sum_q / pair_count
    rms_i = math.sqrt(max(0.0, sum_i2 / pair_count))
    rms_q = math.sqrt(max(0.0, sum_q2 / pair_count))
    rms_mag = math.sqrt(max(0.0, sum_mag2 / pair_count))
    mean_phase_step = phase_step_sum / phase_step_count if phase_step_count else 0.0
    coarse_freq_offset_hz = mean_phase_step * assumed_sample_rate / (2.0 * math.pi)

    return {
        "ok": True,
        "iq_sample_count_preview": pair_count,
        "preview_seconds": pair_count / assumed_sample_rate if assumed_sample_rate > 0 else None,
        "mean_i": mean_i,
        "mean_q": mean_q,
        "rms_i": rms_i,
        "rms_q": rms_q,
        "rms_magnitude": rms_mag,
        "peak_i": peak_i,
        "peak_q": peak_q,
        "clipping_count_preview": clip,
        "clipping_pct_preview": 100.0 * clip / pair_count,
        "coarse_mean_phase_step_rad": mean_phase_step,
        "coarse_frequency_offset_hz": coarse_freq_offset_hz,
    }


def write_text_summary(path: Path, manifest: dict[str, object]) -> None:
    analysis = manifest.get("analysis", {}) if isinstance(manifest.get("analysis"), dict) else {}
    lines = [
        f"GMSK high-rate IQ capture ({MARKER})",
        "",
        f"Created UTC: {manifest.get('created_utc')}",
        f"Host: {manifest.get('host')}:{manifest.get('port')}",
        f"IIO device: {manifest.get('iio_device')}",
        f"Channels: {' '.join(manifest.get('channels', []))}",
        f"Requested seconds: {manifest.get('requested_seconds')}",
        f"Assumed IQ sample rate: {manifest.get('assumed_iq_sample_rate_hz')} Hz",
        f"IQ bytes: {manifest.get('iq_bytes')}",
        f"IQ samples: {manifest.get('iq_sample_count')}",
        f"Approx duration: {manifest.get('approx_duration_seconds'):.3f} sec" if isinstance(manifest.get('approx_duration_seconds'), (float, int)) else "Approx duration: unknown",
        "",
        "Preview analysis:",
        f"  RMS I/Q: {analysis.get('rms_i', 'n/a')} / {analysis.get('rms_q', 'n/a')}",
        f"  Peak I/Q: {analysis.get('peak_i', 'n/a')} / {analysis.get('peak_q', 'n/a')}",
        f"  Mean I/Q: {analysis.get('mean_i', 'n/a')} / {analysis.get('mean_q', 'n/a')}",
        f"  Clipping preview: {analysis.get('clipping_count_preview', 'n/a')} ({analysis.get('clipping_pct_preview', 'n/a')}%)",
        f"  Coarse frequency offset: {analysis.get('coarse_frequency_offset_hz', 'n/a')} Hz",
        "",
        f"IQ path: {manifest.get('iq_path')}",
        f"JSON: {manifest.get('json_path')}",
        f"TXT: {manifest.get('text_summary_path')}",
        "",
        "Decoder claim: none; high-rate IQ capture artifact only, no GMSK/HDLC/AX.25/text decode claimed.",
        "Next step: use this IQ file to develop discriminator, clock recovery, slicer, and HDLC flag counting offline.",
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    env = env_defaults()
    default_host = env.get("PLUTO_HOST") or env.get("PLUTO_IP") or DEFAULT_HOST
    default_user = env.get("PLUTO_USER") or DEFAULT_USER
    default_password = env.get("PLUTO_PASSWORD") or env.get("PLUTO_PASS") or DEFAULT_PASSWORD

    parser = argparse.ArgumentParser(description="Capture high-rate Pluto IQ windows for GMSK Phase 2 diagnostics.")
    parser.add_argument("--host", default=default_host)
    parser.add_argument("--port", default=env.get("PLUTO_PORT", DEFAULT_PORT))
    parser.add_argument("--user", default=default_user)
    parser.add_argument("--password", default=default_password, help="SSH password. Use empty string for key auth.")
    parser.add_argument("--seconds", type=float, default=4.0, help="Capture duration. Keep short; 4 sec at 2.4 MS/s is about 38 MB.")
    parser.add_argument("--assumed-iq-sample-rate", type=float, default=2_400_000.0)
    parser.add_argument("--remote-uri", default=DEFAULT_REMOTE_URI)
    parser.add_argument("--device", default=DEFAULT_DEVICE)
    parser.add_argument("--channels", nargs="+", default=list(DEFAULT_CHANNELS))
    parser.add_argument("--buffer-samples", type=int, default=32768)
    parser.add_argument("--output-dir", default="captures/gmsk_phase2_iq")
    parser.add_argument("--keep-live-audio", action="store_true", help="Do not ask the backend to stop live audio before direct IIO capture.")
    parser.add_argument("--dry-run", action="store_true", help="Validate command construction without SSH capture.")
    args = parser.parse_args(argv)

    if args.seconds <= 0 or args.seconds > 60:
        raise SystemExit("--seconds must be > 0 and <= 60")
    if args.buffer_samples <= 0:
        raise SystemExit("--buffer-samples must be positive")
    if len(args.channels) < 2:
        raise SystemExit("at least two I/Q channels are required")

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    stamp = utc_stamp()
    base = f"gmsk_iq_capture_{stamp}"
    iq_path = output_dir / f"{base}.iq"
    json_path = output_dir / f"{base}.json"
    txt_path = output_dir / f"{base}.txt"

    base_url = f"http://{args.host}:{args.port}"
    status = read_json_url(f"{base_url}/api/status")
    radio_status = read_json_url(f"{base_url}/api/radio/status")
    track_state = read_json_url(f"{base_url}/api/radio/track/state")
    live_stop = None
    if not args.keep_live_audio and not args.dry_run:
        live_stop = post_url(f"{base_url}/api/radio/audio/live/stop")

    remote_parts = [
        "iio_readdev",
        "-u", args.remote_uri,
        "-b", str(args.buffer_samples),
        args.device,
        *args.channels,
    ]
    remote_command = " ".join(shlex.quote(part) for part in remote_parts)
    password = args.password if args.password else None
    ssh_cmd = build_ssh_command(args.user, args.host, password, remote_command)

    if args.dry_run:
        manifest = {
            "ok": True,
            "marker": MARKER,
            "dry_run": True,
            "created_utc": stamp,
            "host": args.host,
            "port": args.port,
            "remote_command": remote_command,
            "ssh_command_redacted": redact_command(ssh_cmd),
            "status": status,
            "radio_status": radio_status,
            "track_state": track_state,
            "decoder_claim": "none; dry run only",
        }
        print(json.dumps(manifest, indent=2, sort_keys=True))
        return 0

    print(f"GMSK high-rate IQ capture ({MARKER})")
    print(f"Source: {args.user}@{args.host} {remote_command}")
    print(f"Requested seconds: {args.seconds}")
    print(f"Writing: {iq_path}")

    start_time = time.time()
    try:
        with iq_path.open("wb") as out:
            proc = subprocess.run(
                ssh_cmd,
                stdout=out,
                stderr=subprocess.PIPE,
                timeout=args.seconds + 8.0,
                check=False,
            )
    except subprocess.TimeoutExpired as exc:
        # Local timeout is expected to terminate the SSH stream after requested duration.
        proc = exc
    elapsed = time.time() - start_time

    stderr = ""
    returncode = None
    if isinstance(proc, subprocess.CompletedProcess):
        returncode = proc.returncode
        stderr = proc.stderr.decode("utf-8", errors="replace") if proc.stderr else ""
    elif isinstance(proc, subprocess.TimeoutExpired):
        returncode = "timeout_after_requested_capture"
        stderr = (proc.stderr or b"").decode("utf-8", errors="replace") if isinstance(proc.stderr, bytes) else str(proc.stderr or "")

    iq_bytes = iq_path.stat().st_size if iq_path.exists() else 0
    iq_sample_count = iq_bytes // 4
    approx_duration = iq_sample_count / args.assumed_iq_sample_rate if args.assumed_iq_sample_rate > 0 else 0.0
    ok = iq_bytes >= 1024 * 1024
    analysis = analyze_iq_preview(iq_path, args.assumed_iq_sample_rate) if iq_bytes >= 4 else {"ok": False, "error": "no IQ bytes captured"}

    manifest = {
        "ok": ok,
        "marker": MARKER,
        "created_utc": stamp,
        "host": args.host,
        "port": args.port,
        "user": args.user,
        "requested_seconds": args.seconds,
        "elapsed_seconds": elapsed,
        "remote_uri": args.remote_uri,
        "iio_device": args.device,
        "channels": args.channels,
        "buffer_samples": args.buffer_samples,
        "assumed_iq_sample_rate_hz": args.assumed_iq_sample_rate,
        "iq_path": str(iq_path),
        "json_path": str(json_path),
        "text_summary_path": str(txt_path),
        "iq_bytes": iq_bytes,
        "iq_sample_count": iq_sample_count,
        "approx_duration_seconds": approx_duration,
        "ssh_returncode": returncode,
        "ssh_stderr_tail": stderr[-2000:],
        "status": status,
        "radio_status": radio_status,
        "track_state": track_state,
        "live_audio_stop_result": live_stop,
        "analysis": analysis,
        "decoder_claim": "none; high-rate IQ capture artifact only, no GMSK/HDLC/AX.25/text decode claimed",
        "next_step": "Use the IQ file for offline FM/discriminator, clock recovery, slicer, and HDLC flag-count experiments.",
    }
    json_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_text_summary(txt_path, manifest)

    print(f"IQ bytes: {iq_bytes}")
    print(f"IQ samples: {iq_sample_count}")
    print(f"Approx duration at {args.assumed_iq_sample_rate:.0f} Hz: {approx_duration:.3f} sec")
    if analysis.get("ok"):
        print(f"Preview RMS I/Q: {analysis.get('rms_i'):.1f} / {analysis.get('rms_q'):.1f}")
        print(f"Preview peak I/Q: {analysis.get('peak_i')} / {analysis.get('peak_q')}")
        print(f"Coarse frequency offset: {analysis.get('coarse_frequency_offset_hz'):.1f} Hz")
    print("Artifacts:")
    print(f"  IQ:   {iq_path}")
    print(f"  JSON: {json_path}")
    print(f"  TXT:  {txt_path}")
    print("Decoder claim: none; capture artifact only.")

    if not ok:
        print("ERROR: captured IQ file is too small; check iio_readdev availability and whether another process owns the IIO buffer.", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
