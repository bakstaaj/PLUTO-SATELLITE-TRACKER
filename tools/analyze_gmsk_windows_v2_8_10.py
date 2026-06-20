#!/usr/bin/env python3
"""Fast windowed GMSK timing diagnostics for Pluto Satellite Tracker v2.8.10c.

This is an offline diagnostic tool. It does not claim HDLC/AX.25/text decode.
It scans a PCM s16le capture in short windows, ranks the most promising timing
segments, and exports the best PCM window for later slicer/HDLC experiments.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import struct
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Sequence, Tuple

MARKER = "GMSK_WINDOWED_TIMING_ANALYZER_V2_8_10C_FAST"
DEFAULT_SAMPLE_RATE = 24000.0
DEFAULT_SYMBOL_RATE = 9600.0


def utc_stamp() -> str:
    return _dt.datetime.utcnow().strftime("%Y%m%dT%H%M%SZ")


def read_pcm_s16le(path: Path) -> List[int]:
    data = path.read_bytes()
    if len(data) < 2:
        raise ValueError(f"PCM file is empty or too small: {path}")
    if len(data) % 2:
        data = data[:-1]
    count = len(data) // 2
    return list(struct.unpack("<" + "h" * count, data))


def write_pcm_s16le(path: Path, samples: Sequence[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as f:
        for value in samples:
            if value < -32768:
                value = -32768
            elif value > 32767:
                value = 32767
            f.write(struct.pack("<h", int(value)))


def percentile(sorted_values: Sequence[float], pct: float) -> float:
    if not sorted_values:
        return 0.0
    if pct <= 0:
        return float(sorted_values[0])
    if pct >= 100:
        return float(sorted_values[-1])
    pos = (len(sorted_values) - 1) * (pct / 100.0)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return float(sorted_values[lo])
    frac = pos - lo
    return float(sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac)


def basic_stats(samples: Sequence[int]) -> Dict[str, float]:
    n = len(samples)
    if n <= 0:
        return {
            "sample_count": 0,
            "rms": 0.0,
            "peak": 0,
            "dc_offset": 0.0,
            "clipping_count": 0,
            "clipping_pct": 0.0,
            "zero_crossings": 0,
            "zero_crossing_estimate_hz": 0.0,
        }
    total = 0.0
    total_sq = 0.0
    peak = 0
    clipping = 0
    zero_crossings = 0
    prev_sign = 0
    for x in samples:
        total += x
        total_sq += float(x) * float(x)
        ax = abs(int(x))
        if ax > peak:
            peak = ax
        if ax >= 32760:
            clipping += 1
        sign = 1 if x > 0 else (-1 if x < 0 else prev_sign)
        if prev_sign and sign and sign != prev_sign:
            zero_crossings += 1
        if sign:
            prev_sign = sign
    rms = math.sqrt(total_sq / n)
    dc = total / n
    return {
        "sample_count": n,
        "rms": rms,
        "peak": peak,
        "dc_offset": dc,
        "clipping_count": clipping,
        "clipping_pct": (100.0 * clipping / n) if n else 0.0,
        "zero_crossings": zero_crossings,
    }


def estimate_zero_crossing_hz(samples: Sequence[int], sample_rate: float) -> float:
    st = basic_stats(samples)
    dur = len(samples) / sample_rate if sample_rate > 0 else 0.0
    if dur <= 0:
        return 0.0
    return (float(st["zero_crossings"]) / 2.0) / dur


def phase_values_for_window(
    window: Sequence[int],
    sample_rate: float,
    symbol_rate: float,
    phase_samples: float,
    max_symbols: int,
) -> List[int]:
    sps = sample_rate / symbol_rate
    if sps <= 0:
        return []
    total_symbols = int((len(window) - 1 - phase_samples) / sps)
    if total_symbols <= 8:
        return []
    step = max(1, total_symbols // max_symbols)
    vals: List[int] = []
    k = 0
    while k < total_symbols and len(vals) < max_symbols:
        idx = int(round(phase_samples + k * sps))
        if 0 <= idx < len(window):
            vals.append(int(window[idx]))
        k += step
    return vals


def score_phase(vals: Sequence[int], peak: float) -> Dict[str, float]:
    n = len(vals)
    if n <= 4 or peak <= 0:
        return {
            "symbol_count": n,
            "mean_abs_norm": 0.0,
            "p10_abs_norm": 0.0,
            "p25_abs_norm": 0.0,
            "near_zero_pct": 100.0,
            "positive_pct": 0.0,
            "transition_per_symbol": 0.0,
            "score": 0.0,
        }
    abs_norm = sorted(abs(v) / peak for v in vals)
    mean_abs = sum(abs_norm) / n
    p10 = percentile(abs_norm, 10)
    p25 = percentile(abs_norm, 25)
    near_zero = sum(1 for v in abs_norm if v < 0.12) * 100.0 / n
    positive = sum(1 for v in vals if v >= 0) * 100.0 / n
    transitions = 0
    prev = 1 if vals[0] >= 0 else -1
    for v in vals[1:]:
        sign = 1 if v >= 0 else -1
        if sign != prev:
            transitions += 1
        prev = sign
    transition_per_symbol = transitions / max(1, n - 1)
    balance_penalty = max(0.0, 1.0 - abs(positive - 50.0) / 50.0)
    zero_penalty = max(0.0, 1.0 - near_zero / 100.0)
    transition_bonus = min(1.0, max(0.0, transition_per_symbol / 0.55))
    score = (0.42 * mean_abs + 0.36 * p25 + 0.12 * p10) * zero_penalty
    score *= (0.70 + 0.20 * balance_penalty + 0.10 * transition_bonus)
    return {
        "symbol_count": n,
        "mean_abs_norm": mean_abs,
        "p10_abs_norm": p10,
        "p25_abs_norm": p25,
        "near_zero_pct": near_zero,
        "positive_pct": positive,
        "transition_per_symbol": transition_per_symbol,
        "score": score,
    }


def phase_candidates_for_window(
    window: Sequence[int],
    sample_rate: float,
    symbol_rate: float,
    max_symbols: int,
) -> List[Dict[str, float]]:
    sps = sample_rate / symbol_rate
    if sps <= 0:
        return []
    peak = max(1, max(abs(int(x)) for x in window))
    # For 2.5 samples/symbol, 0.25-sample offsets are enough to see if any
    # phase stands out, without an expensive all-symbol/all-phase scan.
    phase_step = 0.25
    phase_count = max(2, int(math.ceil(sps / phase_step)))
    rows = []
    for i in range(phase_count):
        phase = round(i * phase_step, 3)
        vals = phase_values_for_window(window, sample_rate, symbol_rate, phase, max_symbols)
        row = score_phase(vals, float(peak))
        row["phase_samples"] = phase
        rows.append(row)
    rows.sort(key=lambda r: r["score"], reverse=True)
    return rows


def analyze_window(
    samples: Sequence[int],
    sample_rate: float,
    symbol_rate: float,
    start: int,
    end: int,
    max_symbols: int,
) -> Dict[str, object]:
    window = samples[start:end]
    st = basic_stats(window)
    candidates = phase_candidates_for_window(window, sample_rate, symbol_rate, max_symbols)
    best = candidates[0] if candidates else {}
    scores = [float(c.get("score", 0.0)) for c in candidates[:5]]
    score_spread = 0.0
    if scores:
        top = scores[0]
        bottom = scores[-1]
        if top > 0:
            score_spread = 100.0 * (top - bottom) / top
    start_sec = start / sample_rate
    duration_sec = (end - start) / sample_rate
    # Window confidence is intentionally conservative: it tells us whether to try
    # HDLC flag probing on this window, not whether decode is solved.
    confidence = "weak"
    if score_spread >= 18.0 and float(best.get("near_zero_pct", 100.0)) < 22.0:
        confidence = "candidate"
    if score_spread >= 28.0 and float(best.get("near_zero_pct", 100.0)) < 18.0:
        confidence = "strong_candidate"
    return {
        "start_sample": start,
        "end_sample": end,
        "start_seconds": start_sec,
        "duration_seconds": duration_sec,
        "rms": st["rms"],
        "peak": st["peak"],
        "dc_offset": st["dc_offset"],
        "zero_crossing_estimate_hz": estimate_zero_crossing_hz(window, sample_rate),
        "best_phase": best,
        "phase_score_spread_pct": score_spread,
        "window_confidence": confidence,
        "timing_phase_candidates": candidates[:10],
    }


def analyze_capture(
    samples: Sequence[int],
    sample_rate: float,
    symbol_rate: float,
    window_seconds: float,
    hop_seconds: float,
    max_symbols: int,
    max_windows: int,
) -> Dict[str, object]:
    n = len(samples)
    win_len = max(256, int(round(window_seconds * sample_rate)))
    hop_len = max(128, int(round(hop_seconds * sample_rate)))
    starts: List[int] = []
    start = 0
    while start + win_len <= n:
        starts.append(start)
        start += hop_len
    if not starts and n > 0:
        starts = [0]
        win_len = n
    if max_windows > 0 and len(starts) > max_windows:
        # Evenly sample windows across the capture when requested.
        idxs = sorted(set(int(round(i * (len(starts) - 1) / (max_windows - 1))) for i in range(max_windows)))
        starts = [starts[i] for i in idxs]

    rows = []
    for start in starts:
        rows.append(analyze_window(samples, sample_rate, symbol_rate, start, min(n, start + win_len), max_symbols))

    rows.sort(key=lambda r: (float(r.get("phase_score_spread_pct", 0.0)), float((r.get("best_phase") or {}).get("score", 0.0))), reverse=True)
    full = basic_stats(samples)
    full["zero_crossing_estimate_hz"] = estimate_zero_crossing_hz(samples, sample_rate)
    return {
        "marker": MARKER,
        "ok": True,
        "created_utc": utc_stamp(),
        "sample_rate_hz": sample_rate,
        "assumed_symbol_rate": symbol_rate,
        "samples_per_symbol": sample_rate / symbol_rate if symbol_rate else 0.0,
        "sample_count": n,
        "duration_seconds": n / sample_rate if sample_rate else 0.0,
        "window_seconds": window_seconds,
        "hop_seconds": hop_seconds,
        "analyzed_window_count": len(rows),
        "full_capture": full,
        "best_window": rows[0] if rows else None,
        "top_windows": rows[:20],
        "clock_status": "windowed_candidates_available" if rows else "no_windows_analyzed",
        "decoder_claim": "none; windowed offline diagnostics only, no HDLC/AX.25/text decode claimed",
        "next_step": "If a window reaches candidate/strong_candidate confidence, run HDLC flag probing on that exported PCM window; otherwise capture higher-rate discriminator/baseband samples.",
    }


def safe_stem(path: Path) -> str:
    return path.stem.replace(" ", "_").replace("/", "_")


def write_reports(result: Dict[str, object], pcm_path: Path, output_dir: Path, samples: Sequence[int], sample_rate: float) -> Tuple[Path, Path, Path]:
    stem = safe_stem(pcm_path)
    output_dir.mkdir(parents=True, exist_ok=True)
    windows_dir = output_dir / "windows"
    json_path = output_dir / f"{stem}_windowed_timing_v2_8_10.json"
    txt_path = output_dir / f"{stem}_windowed_timing_v2_8_10.txt"
    best_pcm_path = windows_dir / f"{stem}_best_window_v2_8_10.pcm"

    best = result.get("best_window") or {}
    if best:
        start = int(best.get("start_sample", 0))
        end = int(best.get("end_sample", start))
        write_pcm_s16le(best_pcm_path, samples[start:end])
    result["pcm_path"] = str(pcm_path)
    result["json_path"] = str(json_path)
    result["text_summary_path"] = str(txt_path)
    result["best_window_pcm_path"] = str(best_pcm_path)

    json_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    full = result.get("full_capture") or {}
    best_phase = (best.get("best_phase") or {}) if isinstance(best, dict) else {}
    lines = [
        f"GMSK windowed timing analyzer ({MARKER})",
        "",
        f"PCM: {pcm_path}",
        f"Sample rate: {sample_rate:.0f} Hz",
        f"Assumed symbol rate: {result.get('assumed_symbol_rate', 0):.1f} symbols/sec",
        f"Samples: {result.get('sample_count', 0)}",
        f"Duration: {result.get('duration_seconds', 0):.2f} sec",
        f"Samples per symbol: {result.get('samples_per_symbol', 0):.2f}",
        f"Full RMS: {float(full.get('rms', 0.0)):.1f}",
        f"Full peak: {int(full.get('peak', 0))}",
        f"Full clipping: {int(full.get('clipping_count', 0))} ({float(full.get('clipping_pct', 0.0)):.4f}%)",
        f"Analyzed windows: {result.get('analyzed_window_count', 0)}",
        "",
        "Best window:",
        f"  start: {float(best.get('start_seconds', 0.0)):.2f} sec",
        f"  duration: {float(best.get('duration_seconds', 0.0)):.2f} sec",
        f"  confidence: {best.get('window_confidence', 'unknown')}",
        f"  RMS: {float(best.get('rms', 0.0)):.1f}",
        f"  peak: {int(best.get('peak', 0))}",
        f"  zero-crossing estimate: {float(best.get('zero_crossing_estimate_hz', 0.0)):.1f} Hz",
        f"  phase: {float(best_phase.get('phase_samples', 0.0)):.2f} samples",
        f"  phase score: {float(best_phase.get('score', 0.0)):.6f}",
        f"  phase score spread: {float(best.get('phase_score_spread_pct', 0.0)):.2f}%",
        f"  p25 abs normalized: {float(best_phase.get('p25_abs_norm', 0.0)):.4f}",
        f"  near-zero: {float(best_phase.get('near_zero_pct', 0.0)):.1f}%",
        f"  positive balance: {float(best_phase.get('positive_pct', 0.0)):.1f}%",
        f"  transitions/symbol: {float(best_phase.get('transition_per_symbol', 0.0)):.3f}",
        "",
        f"Best window PCM: {best_pcm_path}",
        f"Clock status: {result.get('clock_status')}",
        f"Decoder claim: {result.get('decoder_claim')}",
        f"Next step: {result.get('next_step')}",
        "",
        f"JSON: {json_path}",
        f"TXT:  {txt_path}",
    ]
    txt_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return json_path, txt_path, best_pcm_path


def self_test() -> int:
    # A tiny deterministic synthetic signal for smoke testing runtime only.
    tmp = Path("/tmp/gmsk_windowed_v2_8_10c_selftest.pcm")
    samples = []
    sr = 24000.0
    for i in range(int(sr * 2.0)):
        t = i / sr
        v = int(3000.0 * math.sin(2.0 * math.pi * 3200.0 * t) + 800.0 * math.sin(2.0 * math.pi * 900.0 * t))
        samples.append(v)
    write_pcm_s16le(tmp, samples)
    result = analyze_capture(samples, sr, 9600.0, 1.0, 0.5, 1200, 20)
    if not result.get("best_window") or int(result.get("analyzed_window_count", 0)) < 1:
        print("SELFTEST FAIL")
        return 1
    print("SELFTEST PASS")
    return 0


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Fast windowed GMSK timing diagnostics for PCM s16le captures.")
    parser.add_argument("pcm", nargs="?", help="PCM s16le file to analyze")
    parser.add_argument("--sample-rate", type=float, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--symbol-rate", type=float, default=DEFAULT_SYMBOL_RATE)
    parser.add_argument("--window-seconds", type=float, default=1.0)
    parser.add_argument("--hop-seconds", type=float, default=0.5)
    parser.add_argument("--max-symbols", type=int, default=2400, help="maximum sampled symbols per phase/window")
    parser.add_argument("--max-windows", type=int, default=0, help="0 means analyze all windows")
    parser.add_argument("--output-dir", default="analysis/gmsk_phase2")
    parser.add_argument("--self-test", action="store_true")
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.self_test:
        return self_test()
    if not args.pcm:
        print("ERROR: PCM file is required unless --self-test is used", file=sys.stderr)
        return 2
    pcm_path = Path(args.pcm)
    samples = read_pcm_s16le(pcm_path)
    result = analyze_capture(
        samples,
        args.sample_rate,
        args.symbol_rate,
        args.window_seconds,
        args.hop_seconds,
        max(200, args.max_symbols),
        max(0, args.max_windows),
    )
    json_path, txt_path, best_pcm = write_reports(result, pcm_path, Path(args.output_dir), samples, args.sample_rate)
    full = result["full_capture"]
    best = result["best_window"] or {}
    best_phase = best.get("best_phase") or {}
    print(f"GMSK windowed timing analyzer ({MARKER})")
    print()
    print(f"PCM: {pcm_path}")
    print(f"Sample rate: {args.sample_rate:.0f} Hz")
    print(f"Assumed symbol rate: {args.symbol_rate:.1f} symbols/sec")
    print(f"Samples: {len(samples)}")
    print(f"Duration: {len(samples) / args.sample_rate:.2f} sec")
    print(f"Samples per symbol: {args.sample_rate / args.symbol_rate:.2f}")
    print(f"Full RMS: {float(full.get('rms', 0.0)):.1f}")
    print(f"Full peak: {int(full.get('peak', 0))}")
    print(f"Analyzed windows: {result.get('analyzed_window_count', 0)}")
    print()
    print("Best window:")
    print(f"  start: {float(best.get('start_seconds', 0.0)):.2f} sec")
    print(f"  duration: {float(best.get('duration_seconds', 0.0)):.2f} sec")
    print(f"  confidence: {best.get('window_confidence', 'unknown')}")
    print(f"  phase: {float(best_phase.get('phase_samples', 0.0)):.2f} samples")
    print(f"  phase score: {float(best_phase.get('score', 0.0)):.6f}")
    print(f"  phase score spread: {float(best.get('phase_score_spread_pct', 0.0)):.2f}%")
    print(f"  near-zero: {float(best_phase.get('near_zero_pct', 0.0)):.1f}%")
    print(f"  transitions/symbol: {float(best_phase.get('transition_per_symbol', 0.0)):.3f}")
    print()
    print("Artifacts:")
    print(f"  JSON: {json_path}")
    print(f"  TXT:  {txt_path}")
    print(f"  Best window PCM: {best_pcm}")
    print()
    print("Decoder claim: none; windowed offline diagnostics only, no HDLC/AX.25/text decode claimed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
