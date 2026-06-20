#!/usr/bin/env python3
"""Offline GMSK/FSK PCM analyzer for Pluto Satellite Tracker.

Marker: GMSK_PHASE2_OFFLINE_ANALYZER_V2_8_9

This is a diagnostic-only tool. It reads signed 16-bit little-endian PCM
captured from the current live audio path and estimates whether the sample
window is useful for later clock/slicer work. It does not claim HDLC, AX.25,
packet, or text decode.
"""

from __future__ import annotations

import argparse
import array
import json
import math
import os
import statistics
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, Iterable, List, Sequence

MARKER = "GMSK_PHASE2_OFFLINE_ANALYZER_V2_8_9"
DEFAULT_SAMPLE_RATE = 24000.0
DEFAULT_SYMBOL_RATE = 9600.0


def load_pcm_s16le(path: Path) -> List[int]:
    raw = path.read_bytes()
    if len(raw) < 2:
        raise ValueError(f"{path} is too small to contain s16le PCM")
    if len(raw) % 2:
        raw = raw[:-1]
    data = array.array("h")
    data.frombytes(raw)
    if sys.byteorder != "little":
        data.byteswap()
    return list(data)


def percentile(sorted_values: Sequence[float], pct: float) -> float:
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return float(sorted_values[0])
    pos = (len(sorted_values) - 1) * pct / 100.0
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return float(sorted_values[lo])
    frac = pos - lo
    return float(sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac)


def interpolate(samples: Sequence[float], index: float) -> float:
    i0 = int(math.floor(index))
    if i0 < 0:
        return samples[0]
    if i0 >= len(samples) - 1:
        return samples[-1]
    frac = index - i0
    return samples[i0] * (1.0 - frac) + samples[i0 + 1] * frac


def zero_crossings(values: Sequence[float]) -> int:
    if len(values) < 2:
        return 0
    count = 0
    prev = values[0] >= 0.0
    for value in values[1:]:
        cur = value >= 0.0
        if cur != prev:
            count += 1
        prev = cur
    return count


def sample_symbols(
    centered: Sequence[float],
    rms: float,
    sample_rate: float,
    symbol_rate: float,
    phase_samples: float,
    max_symbols: int,
) -> List[float]:
    sps = sample_rate / symbol_rate
    limit = min(max_symbols, max(0, int((len(centered) - 2 - phase_samples) / sps)))
    scale = rms if rms > 0.0 else 1.0
    return [interpolate(centered, phase_samples + k * sps) / scale for k in range(limit)]


def phase_metrics(symbols: Sequence[float], phase_samples: float) -> Dict[str, float]:
    if not symbols:
        return {
            "phase_samples": phase_samples,
            "score": 0.0,
            "symbol_count": 0,
        }
    abs_values = sorted(abs(v) for v in symbols)
    mean_abs = sum(abs_values) / len(abs_values)
    p10 = percentile(abs_values, 10.0)
    p25 = percentile(abs_values, 25.0)
    near_zero_pct = 100.0 * sum(1 for v in abs_values if v < 0.25) / len(abs_values)
    positives = sum(1 for v in symbols if v > 0.0)
    positive_pct = 100.0 * positives / len(symbols)
    transitions = 0
    if len(symbols) > 1:
        prev = symbols[0] > 0.0
        for value in symbols[1:]:
            cur = value > 0.0
            if cur != prev:
                transitions += 1
            prev = cur
        transition_per_symbol = transitions / (len(symbols) - 1)
    else:
        transition_per_symbol = 0.0
    balance = max(0.0, 1.0 - abs(positive_pct - 50.0) / 50.0)
    near_factor = max(0.0, 1.0 - near_zero_pct / 100.0)
    score = p25 * near_factor * (0.5 + 0.5 * balance)
    return {
        "phase_samples": phase_samples,
        "score": score,
        "symbol_count": len(symbols),
        "mean_abs_norm": mean_abs,
        "p10_abs_norm": p10,
        "p25_abs_norm": p25,
        "near_zero_pct": near_zero_pct,
        "positive_pct": positive_pct,
        "transition_per_symbol": transition_per_symbol,
    }


def classify_level(rms: float, peak: int, clipping_count: int) -> str:
    if clipping_count > 0:
        return "clipping_detected"
    if rms < 250.0 or peak < 1200:
        return "weak"
    if rms > 8000.0 or peak > 25000:
        return "hot"
    return "healthy"


def classify_clock(samples_per_symbol: float, best: Dict[str, float], spread_pct: float) -> str:
    near_zero = float(best.get("near_zero_pct", 100.0))
    if samples_per_symbol < 2.0:
        return "too_few_samples_per_symbol"
    if samples_per_symbol < 3.0:
        if near_zero < 30.0:
            return "tight_but_workable_for_offline_diagnostics"
        return "tight_and_weak_eye"
    if spread_pct < 2.0:
        return "weak_phase_peak"
    return "candidate_symbol_timing"


def classify_slicer(best: Dict[str, float]) -> str:
    near_zero = float(best.get("near_zero_pct", 100.0))
    positive = float(best.get("positive_pct", 0.0))
    if near_zero <= 30.0 and 35.0 <= positive <= 65.0:
        return "candidate_threshold_available"
    if near_zero <= 40.0:
        return "possible_threshold_but_no_lock"
    return "weak_or_no_threshold"


def analyze_pcm(path: Path, sample_rate: float, symbol_rate: float, max_symbols: int, phase_step: float) -> Dict[str, object]:
    raw = load_pcm_s16le(path)
    if not raw:
        raise ValueError(f"{path} contains no samples")
    sample_count = len(raw)
    duration = sample_count / sample_rate
    dc = sum(raw) / sample_count
    centered = [float(v) - dc for v in raw]
    rms = math.sqrt(sum(v * v for v in centered) / sample_count)
    peak = max(abs(v) for v in raw)
    clipping_count = sum(1 for v in raw if abs(v) >= 32760)
    sps = sample_rate / symbol_rate
    zc = zero_crossings(centered)
    zc_hz = zc / (2.0 * duration) if duration > 0.0 else 0.0

    phase_results: List[Dict[str, float]] = []
    phase = 0.0
    while phase < sps:
        symbols = sample_symbols(centered, rms, sample_rate, symbol_rate, phase, max_symbols)
        phase_results.append(phase_metrics(symbols, round(phase, 4)))
        phase += phase_step
    phase_results.sort(key=lambda row: float(row.get("score", 0.0)), reverse=True)
    best = phase_results[0] if phase_results else {}
    worst_score = float(phase_results[-1].get("score", 0.0)) if phase_results else 0.0
    best_score = float(best.get("score", 0.0)) if best else 0.0
    score_spread_pct = 0.0 if best_score <= 0.0 else 100.0 * (best_score - worst_score) / best_score

    analysis = {
        "ok": True,
        "marker": MARKER,
        "created_utc": datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"),
        "pcm_path": str(path),
        "sample_rate_hz": sample_rate,
        "assumed_symbol_rate": symbol_rate,
        "samples_per_symbol": sps,
        "sample_count": sample_count,
        "duration_seconds": duration,
        "rms": rms,
        "peak": peak,
        "dc_offset": dc,
        "clipping_count": clipping_count,
        "clipping_pct": 100.0 * clipping_count / sample_count,
        "level_status": classify_level(rms, peak, clipping_count),
        "zero_crossings": zc,
        "zero_crossing_estimate_hz": zc_hz,
        "timing_phase_candidates": phase_results[:10],
        "best_phase": best,
        "phase_score_spread_pct": score_spread_pct,
        "clock_status": classify_clock(sps, best, score_spread_pct),
        "slicer_status": classify_slicer(best),
        "decoder_claim": "none; offline diagnostics only, no HDLC/AX.25/text decode claimed",
        "next_step": "Use higher-rate discriminator/baseband samples for real clock recovery, then add slicer and HDLC flag counting.",
    }
    return analysis


def write_outputs(analysis: Dict[str, object], out_dir: Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    pcm_path = Path(str(analysis["pcm_path"]))
    stem = pcm_path.stem
    json_path = out_dir / f"{stem}_gmsk_analysis_v2_8_9.json"
    txt_path = out_dir / f"{stem}_gmsk_analysis_v2_8_9.txt"
    json_path.write_text(json.dumps(analysis, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    best = analysis.get("best_phase", {})
    lines = [
        f"GMSK Phase 2 offline analyzer ({MARKER})",
        "",
        f"PCM: {analysis['pcm_path']}",
        f"Sample rate: {analysis['sample_rate_hz']:.0f} Hz",
        f"Assumed symbol rate: {analysis['assumed_symbol_rate']:.1f} symbols/sec",
        f"Samples: {analysis['sample_count']}",
        f"Duration: {analysis['duration_seconds']:.2f} sec",
        f"Samples per symbol: {analysis['samples_per_symbol']:.2f}",
        f"RMS: {analysis['rms']:.1f}",
        f"Peak: {analysis['peak']}",
        f"DC offset: {analysis['dc_offset']:.3f}",
        f"Clipping: {analysis['clipping_count']} ({analysis['clipping_pct']:.4f}%)",
        f"Level status: {analysis['level_status']}",
        f"Zero-crossing estimate: {analysis['zero_crossing_estimate_hz']:.1f} Hz",
        "",
        "Best symbol timing candidate:",
        f"  phase: {best.get('phase_samples', 0.0):.2f} samples",
        f"  score: {best.get('score', 0.0):.6f}",
        f"  p25 abs normalized: {best.get('p25_abs_norm', 0.0):.4f}",
        f"  near-zero: {best.get('near_zero_pct', 0.0):.1f}%",
        f"  positive balance: {best.get('positive_pct', 0.0):.1f}%",
        f"  transitions/symbol: {best.get('transition_per_symbol', 0.0):.3f}",
        f"  phase score spread: {analysis['phase_score_spread_pct']:.2f}%",
        "",
        f"Clock status: {analysis['clock_status']}",
        f"Slicer status: {analysis['slicer_status']}",
        f"Decoder claim: {analysis['decoder_claim']}",
        f"Next step: {analysis['next_step']}",
        "",
        f"JSON: {json_path}",
        f"TXT:  {txt_path}",
    ]
    txt_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print("\n".join(lines))


def self_test() -> int:
    tmp = Path("/tmp/gmsk_v2_8_9_selftest.pcm")
    fs = 24000.0
    baud = 9600.0
    # Deterministic synthetic diagnostic waveform: random-ish NRZ at 9600, sampled at 24 kHz.
    values = []
    state = 1.0
    lfsr = 0xACE1
    for n in range(int(fs * 1.25)):
        if n % 3 == 0:
            bit = ((lfsr >> 0) ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1
            lfsr = (lfsr >> 1) | (bit << 15)
            state = 1.0 if (lfsr & 1) else -1.0
        shaped = 1800.0 * state + 200.0 * math.sin(2.0 * math.pi * 1200.0 * n / fs)
        values.append(max(-32767, min(32767, int(shaped))))
    arr = array.array("h", values)
    if sys.byteorder != "little":
        arr.byteswap()
    tmp.write_bytes(arr.tobytes())
    analysis = analyze_pcm(tmp, fs, baud, 50000, 0.25)
    if not analysis.get("ok") or analysis.get("sample_count", 0) <= 0:
        raise RuntimeError("self-test analysis failed")
    if analysis.get("decoder_claim") != "none; offline diagnostics only, no HDLC/AX.25/text decode claimed":
        raise RuntimeError("self-test decoder claim guard failed")
    print(f"SELF-TEST PASS {MARKER}")
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Analyze captured s16le PCM for GMSK/FSK timing diagnostics.")
    parser.add_argument("pcm", nargs="*", help="PCM s16le files to analyze")
    parser.add_argument("--sample-rate", type=float, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--symbol-rate", type=float, default=DEFAULT_SYMBOL_RATE)
    parser.add_argument("--phase-step", type=float, default=0.1, help="Timing phase scan step in samples")
    parser.add_argument("--max-symbols", type=int, default=200000)
    parser.add_argument("--out-dir", default="analysis/gmsk_phase2")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if not args.pcm:
        parser.error("at least one PCM file is required unless --self-test is used")
    for pcm_text in args.pcm:
        pcm_path = Path(pcm_text)
        analysis = analyze_pcm(pcm_path, args.sample_rate, args.symbol_rate, args.max_symbols, args.phase_step)
        write_outputs(analysis, Path(args.out_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
