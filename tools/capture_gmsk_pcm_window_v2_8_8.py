#!/usr/bin/env python3
"""
GMSK Phase 2 capture harness for Pluto Satellite Tracker.

This tool captures the current backend live-audio stream into repeatable artifacts
for offline symbol-clock and slicer development. It intentionally does not claim
HDLC, AX.25, or text decode.

GMSK_PHASE2_CAPTURE_HARNESS_V2_8_8
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import os
import struct
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Tuple

MARKER = "GMSK_PHASE2_CAPTURE_HARNESS_V2_8_8"
DEFAULT_SYMBOL_RATE = 9600.0
DEFAULT_SAMPLE_RATE = 24000


def fetch_bytes(url: str, timeout: float) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": f"pluto-gmsk-capture/{MARKER}"})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return response.read()


def fetch_json(url: str, timeout: float = 5.0) -> Dict[str, Any]:
    try:
        data = fetch_bytes(url, timeout=timeout)
        return json.loads(data.decode("utf-8", errors="replace"))
    except Exception as exc:  # noqa: BLE001 - diagnostics should preserve failures
        return {"ok": False, "error": str(exc), "url": url}


def extract_wav_pcm(blob: bytes) -> Tuple[int, int, bytes, str]:
    """Return sample_rate, bits_per_sample, pcm_bytes, format."""
    if len(blob) >= 44 and blob[:4] == b"RIFF" and blob[8:12] == b"WAVE":
        sample_rate = struct.unpack_from("<I", blob, 24)[0]
        bits_per_sample = struct.unpack_from("<H", blob, 34)[0]
        offset = 12
        while offset + 8 <= len(blob):
            chunk_id = blob[offset:offset + 4]
            chunk_size = struct.unpack_from("<I", blob, offset + 4)[0]
            data_start = offset + 8
            data_end = min(data_start + chunk_size, len(blob))
            if chunk_id == b"data":
                return sample_rate, bits_per_sample, blob[data_start:data_end], "wav"
            offset = data_start + chunk_size + (chunk_size & 1)
        return sample_rate, bits_per_sample, b"", "wav_no_data_chunk"
    return DEFAULT_SAMPLE_RATE, 16, blob, "raw_s16le_assumed"


def pcm_s16le_to_samples(pcm: bytes) -> List[int]:
    if len(pcm) < 2:
        return []
    usable = len(pcm) - (len(pcm) % 2)
    return list(struct.unpack("<" + "h" * (usable // 2), pcm[:usable]))


def goertzel_power(samples: List[float], sample_rate: int, freq_hz: float) -> float:
    if not samples or sample_rate <= 0 or freq_hz <= 0:
        return 0.0
    n = len(samples)
    k = int(0.5 + (n * freq_hz / sample_rate))
    omega = (2.0 * math.pi * k) / n
    coeff = 2.0 * math.cos(omega)
    q0 = q1 = q2 = 0.0
    for sample in samples:
        q0 = coeff * q1 - q2 + sample
        q2 = q1
        q1 = q0
    return q1 * q1 + q2 * q2 - q1 * q2 * coeff


def analyze(samples_i16: List[int], sample_rate: int, symbol_rate: float) -> Dict[str, Any]:
    count = len(samples_i16)
    if count == 0:
        return {"ok": False, "error": "no samples"}

    mean = sum(samples_i16) / count
    centered = [float(x) - mean for x in samples_i16]
    rms = math.sqrt(sum(x * x for x in centered) / count)
    peak = max(abs(x) for x in samples_i16)
    duration = count / float(sample_rate) if sample_rate > 0 else 0.0
    sps = sample_rate / symbol_rate if symbol_rate > 0 else 0.0

    zero_crossings = 0
    prev = centered[0]
    for value in centered[1:]:
        if (prev < 0.0 and value >= 0.0) or (prev >= 0.0 and value < 0.0):
            zero_crossings += 1
        prev = value
    zero_crossing_hz = zero_crossings / (2.0 * duration) if duration > 0 else 0.0

    # A very simple transition proxy: sign changes after DC removal normalized to symbol count.
    symbol_count = duration * symbol_rate if duration > 0 else 0.0
    transition_ratio = zero_crossings / symbol_count if symbol_count > 0 else 0.0

    clipping_count = sum(1 for x in samples_i16 if abs(x) >= 32700)
    clipping_pct = 100.0 * clipping_count / count

    # Coarse FSK/GMSK energy bins. These are not a decoder; they help compare windows.
    centered_for_power = centered[: min(len(centered), sample_rate)]
    powers = {
        "1200": goertzel_power(centered_for_power, sample_rate, 1200.0),
        "2400": goertzel_power(centered_for_power, sample_rate, 2400.0),
        "3000": goertzel_power(centered_for_power, sample_rate, 3000.0),
        "4800": goertzel_power(centered_for_power, sample_rate, 4800.0),
        "9600": goertzel_power(centered_for_power, sample_rate, 9600.0),
    }

    if rms < 250.0:
        level = "low"
    elif peak > 30000 or clipping_pct > 0.05:
        level = "too_hot_or_clipping"
    elif rms > 700.0 and peak < 20000:
        level = "healthy"
    else:
        level = "usable"

    if sps >= 4.0:
        clock_note = "good_for_clock_experiments"
    elif sps >= 2.0:
        clock_note = "tight_but_workable_for_diagnostics"
    else:
        clock_note = "too_low_for_reliable_clock_recovery"

    if transition_ratio < 0.15:
        transition_note = "low_transition_activity_or_weak_signal"
    elif transition_ratio <= 1.50:
        transition_note = "candidate_transition_activity_present"
    else:
        transition_note = "high_transition_noise_or_wrong_rate"

    return {
        "ok": True,
        "sample_count": count,
        "sample_rate_hz": sample_rate,
        "duration_seconds": duration,
        "assumed_symbol_rate": symbol_rate,
        "samples_per_symbol": sps,
        "dc_offset": mean,
        "rms": rms,
        "peak": peak,
        "level_status": level,
        "clipping_count": clipping_count,
        "clipping_pct": clipping_pct,
        "zero_crossings": zero_crossings,
        "zero_crossing_estimate_hz": zero_crossing_hz,
        "transition_ratio_vs_symbol_rate": transition_ratio,
        "transition_status": transition_note,
        "clock_status": clock_note,
        "coarse_energy_bins": powers,
        "decoder_claim": "none; capture artifact only, no HDLC/AX.25/text decode claimed",
    }


def write_text_summary(path: Path, metadata: Dict[str, Any]) -> None:
    analysis = metadata.get("analysis", {})
    lines = [
        f"GMSK Phase 2 capture harness ({MARKER})",
        "",
        f"Created UTC: {metadata.get('created_utc')}",
        f"Source URL: {metadata.get('source_url')}",
        f"Stream format: {metadata.get('stream_format')}",
        f"Sample rate: {analysis.get('sample_rate_hz')} Hz",
        f"Samples: {analysis.get('sample_count')}",
        f"Duration: {analysis.get('duration_seconds', 0):.2f} sec",
        f"Assumed symbol rate: {analysis.get('assumed_symbol_rate')} symbols/sec",
        f"Samples per symbol: {analysis.get('samples_per_symbol', 0):.2f}",
        f"RMS: {analysis.get('rms', 0):.1f}",
        f"Peak: {analysis.get('peak')}",
        f"DC offset: {analysis.get('dc_offset', 0):.2f}",
        f"Level status: {analysis.get('level_status')}",
        f"Clock status: {analysis.get('clock_status')}",
        f"Transition status: {analysis.get('transition_status')}",
        f"Transition ratio vs symbol rate: {analysis.get('transition_ratio_vs_symbol_rate', 0):.2f}",
        f"Zero-crossing estimate: {analysis.get('zero_crossing_estimate_hz', 0):.1f} Hz",
        f"Decoder claim: {analysis.get('decoder_claim')}",
        "",
        "Artifacts:",
        f"  WAV/raw stream: {metadata.get('wav_path')}",
        f"  PCM s16le:       {metadata.get('pcm_path')}",
        f"  JSON metadata:   {metadata.get('json_path')}",
        "",
        "Next use: compare multiple captures from the same GMSK satellite pass, then use the PCM artifact to tune symbol timing and slicer thresholds offline.",
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture and analyze a GMSK diagnostic PCM window from Pluto live audio.")
    parser.add_argument("--host", default=os.environ.get("PLUTO_HOST", "192.168.68.104"), help="Pluto host/IP")
    parser.add_argument("--port", default=os.environ.get("PLUTO_PORT", "8080"), help="Pluto HTTP port")
    parser.add_argument("--seconds", type=int, default=8, help="Capture seconds, default 8")
    parser.add_argument("--symbol-rate", type=float, default=DEFAULT_SYMBOL_RATE, help="Assumed GMSK symbol rate, default 9600")
    parser.add_argument("--out-dir", default="captures/gmsk_phase2", help="Output directory")
    parser.add_argument("--no-stream", action="store_true", help="Only query status endpoints; do not capture live.wav")
    args = parser.parse_args()

    if args.seconds < 1 or args.seconds > 120:
        print("seconds must be between 1 and 120", file=sys.stderr)
        return 2

    base = f"http://{args.host}:{args.port}"
    created = _dt.datetime.now(_dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    prefix = out_dir / f"gmsk_capture_{created}"

    status = fetch_json(f"{base}/api/status")
    radio_status = fetch_json(f"{base}/api/radio/status")
    track_state = fetch_json(f"{base}/api/radio/track/state")

    if args.no_stream:
        metadata = {
            "marker": MARKER,
            "created_utc": created,
            "host": args.host,
            "port": args.port,
            "status": status,
            "radio_status": radio_status,
            "track_state": track_state,
            "note": "no stream captured because --no-stream was used",
        }
        json_path = prefix.with_suffix(".json")
        json_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"Wrote metadata only: {json_path}")
        return 0

    source_url = f"{base}/api/radio/audio/live.wav?stream=1&seconds={args.seconds}"
    try:
        blob = fetch_bytes(source_url, timeout=max(10.0, args.seconds + 20.0))
    except urllib.error.HTTPError as exc:
        print(f"live stream request failed: HTTP {exc.code} {exc.reason}", file=sys.stderr)
        print("Start Receive/Listen in the UI during an active pass, then rerun this capture tool.", file=sys.stderr)
        return 3
    except Exception as exc:  # noqa: BLE001
        print(f"live stream request failed: {exc}", file=sys.stderr)
        return 3

    sample_rate, bits_per_sample, pcm, stream_format = extract_wav_pcm(blob)
    samples = pcm_s16le_to_samples(pcm)
    analysis = analyze(samples, sample_rate, args.symbol_rate)

    wav_path = prefix.with_suffix(".wav" if stream_format.startswith("wav") else ".stream")
    pcm_path = prefix.with_suffix(".pcm")
    json_path = prefix.with_suffix(".json")
    txt_path = prefix.with_suffix(".txt")

    wav_path.write_bytes(blob)
    pcm_path.write_bytes(pcm)

    metadata: Dict[str, Any] = {
        "marker": MARKER,
        "created_utc": created,
        "host": args.host,
        "port": args.port,
        "source_url": source_url,
        "requested_seconds": args.seconds,
        "stream_format": stream_format,
        "bits_per_sample": bits_per_sample,
        "wav_path": str(wav_path),
        "pcm_path": str(pcm_path),
        "json_path": str(json_path),
        "text_summary_path": str(txt_path),
        "status": status,
        "radio_status": radio_status,
        "track_state": track_state,
        "analysis": analysis,
    }
    json_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_text_summary(txt_path, metadata)

    print(txt_path.read_text(encoding="utf-8"))
    return 0 if analysis.get("ok") else 4


if __name__ == "__main__":
    raise SystemExit(main())
