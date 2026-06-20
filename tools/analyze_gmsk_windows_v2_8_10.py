#!/usr/bin/env python3
"""Windowed GMSK timing diagnostics for Pluto Satellite Tracker captures.

This is an offline diagnostic tool only. It reads signed 16-bit little-endian PCM
captured from the existing live audio path and ranks short windows by candidate
symbol-timing/slicer quality. It does not claim HDLC, AX.25, packet, or text
recovery.
"""
from __future__ import annotations

import argparse
import array
import datetime as _dt
import json
import math
import os
from pathlib import Path
import statistics
import sys
from typing import Dict, Iterable, List, Sequence, Tuple

MARKER = "GMSK_WINDOWED_TIMING_ANALYZER_V2_8_10"
DEFAULT_SAMPLE_RATE = 24000.0
DEFAULT_SYMBOL_RATE = 9600.0


def utc_stamp() -> str:
    return _dt.datetime.now(_dt.UTC).strftime("%Y%m%dT%H%M%SZ")


def load_pcm_s16le(path: Path) -> array.array:
    data = path.read_bytes()
    if len(data) < 2:
        raise ValueError(f"PCM file is empty or too short: {path}")
    if len(data) % 2:
        data = data[:-1]
    samples = array.array("h")
    samples.frombytes(data)
    if sys.byteorder != "little":
        samples.byteswap()
    return samples


def percentile(sorted_values: Sequence[float], pct: float) -> float:
    if not sorted_values:
        return 0.0
    if len(sorted_values) == 1:
        return float(sorted_values[0])
    pos = (len(sorted_values) - 1) * (pct / 100.0)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return float(sorted_values[lo])
    frac = pos - lo
    return float(sorted_values[lo] * (1.0 - frac) + sorted_values[hi] * frac)


def zero_crossings(samples: Sequence[int]) -> int:
    prev = 0
    count = 0
    for value in samples:
        sign = 1 if value > 0 else -1 if value < 0 else 0
        if sign and prev and sign != prev:
            count += 1
        if sign:
            prev = sign
    return count


def level_metrics(samples: Sequence[int], sample_rate: float) -> Dict[str, float | int | str]:
    n = len(samples)
    if n == 0:
        return {
            "sample_count": 0,
            "duration_seconds": 0.0,
            "rms": 0.0,
            "peak": 0,
            "dc_offset": 0.0,
            "clipping_count": 0,
            "clipping_pct": 0.0,
            "zero_crossings": 0,
            "zero_crossing_estimate_hz": 0.0,
            "level_status": "empty",
        }
    total = 0.0
    total_sq = 0.0
    peak = 0
    clipping = 0
    for sample in samples:
        total += sample
        total_sq += float(sample) * float(sample)
        av = abs(sample)
        if av > peak:
            peak = av
        if av >= 32760:
            clipping += 1
    rms = math.sqrt(total_sq / n)
    dc = total / n
    zc = zero_crossings(samples)
    if clipping:
        level_status = "clipping_present"
    elif rms < 150:
        level_status = "low"
    elif rms > 9000 or peak > 28000:
        level_status = "hot"
    else:
        level_status = "healthy"
    return {
        "sample_count": n,
        "duration_seconds": n / sample_rate,
        "rms": rms,
        "peak": peak,
        "dc_offset": dc,
        "clipping_count": clipping,
        "clipping_pct": (clipping * 100.0 / n),
        "zero_crossings": zc,
        "zero_crossing_estimate_hz": zc * sample_rate / (2.0 * n),
        "level_status": level_status,
    }


def sign_transition_ratio(values: Sequence[float]) -> float:
    prev = 0
    transitions = 0
    usable = 0
    for value in values:
        sign = 1 if value > 0 else -1 if value < 0 else 0
        if sign:
            if prev and sign != prev:
                transitions += 1
            prev = sign
            usable += 1
    if usable <= 1:
        return 0.0
    return transitions / float(usable - 1)


def phase_candidates_for_window(samples: Sequence[int], sample_rate: float, symbol_rate: float) -> List[Dict[str, float | int]]:
    if not samples:
        return []
    samples_per_symbol = sample_rate / symbol_rate
    max_phase_steps = max(1, int(round(samples_per_symbol * 10.0)))
    phases = [step / 10.0 for step in range(max_phase_steps)]
    max_abs = max(abs(v) for v in samples) or 1
    candidates: List[Dict[str, float | int]] = []

    for phase in phases:
        symbol_values: List[float] = []
        k = 0
        while True:
            idx = int(round(phase + k * samples_per_symbol))
            if idx >= len(samples):
                break
            symbol_values.append(float(samples[idx]))
            k += 1
        if len(symbol_values) < 32:
            continue
        norm_abs = sorted(abs(v) / max_abs for v in symbol_values)
        mean_abs_norm = sum(norm_abs) / len(norm_abs)
        p10 = percentile(norm_abs, 10.0)
        p25 = percentile(norm_abs, 25.0)
        near_zero_pct = sum(1 for v in norm_abs if v < 0.10) * 100.0 / len(norm_abs)
        positive_pct = sum(1 for v in symbol_values if v > 0.0) * 100.0 / len(symbol_values)
        transition_per_symbol = sign_transition_ratio(symbol_values)
        balance_factor = max(0.0, 1.0 - abs(50.0 - positive_pct) / 50.0)
        zero_factor = max(0.0, 1.0 - near_zero_pct / 100.0)
        # Reward a clean eye opening (p25), balanced slicer polarity, and plausible transitions.
        transition_factor = max(0.2, 1.0 - abs(0.50 - transition_per_symbol))
        score = p25 * zero_factor * balance_factor * transition_factor
        candidates.append({
            "phase_samples": round(phase, 3),
            "score": score,
            "mean_abs_norm": mean_abs_norm,
            "p10_abs_norm": p10,
            "p25_abs_norm": p25,
            "near_zero_pct": near_zero_pct,
            "positive_pct": positive_pct,
            "transition_per_symbol": transition_per_symbol,
            "symbol_count": len(symbol_values),
        })
    candidates.sort(key=lambda row: float(row["score"]), reverse=True)
    return candidates


def phase_spread_pct(candidates: Sequence[Dict[str, float | int]]) -> float:
    if len(candidates) < 2:
        return 0.0
    best = float(candidates[0]["score"])
    second = float(candidates[1]["score"])
    if best <= 0.0:
        return 0.0
    return max(0.0, (best - second) * 100.0 / best)


def clock_status(spread: float, best: Dict[str, float | int] | None) -> str:
    if not best:
        return "no_candidate"
    near_zero = float(best["near_zero_pct"])
    p25 = float(best["p25_abs_norm"])
    if spread >= 25.0 and near_zero <= 20.0 and p25 >= 0.25:
        return "strong_candidate_window"
    if spread >= 12.0 and near_zero <= 24.0 and p25 >= 0.22:
        return "moderate_candidate_window"
    if spread >= 6.0:
        return "weak_candidate_window"
    return "no_dominant_clock_phase"


def analyze_window(samples: Sequence[int], sample_rate: float, symbol_rate: float, start_sample: int, end_sample: int) -> Dict[str, object]:
    window = samples[start_sample:end_sample]
    metrics = level_metrics(window, sample_rate)
    candidates = phase_candidates_for_window(window, sample_rate, symbol_rate)
    spread = phase_spread_pct(candidates)
    best = candidates[0] if candidates else None
    timing_score = 0.0
    if best:
        timing_score = float(best["score"]) * (1.0 + spread / 100.0)
        if metrics["level_status"] != "healthy":
            timing_score *= 0.75
    return {
        "start_sample": start_sample,
        "end_sample": end_sample,
        "start_seconds": start_sample / sample_rate,
        "end_seconds": end_sample / sample_rate,
        "duration_seconds": (end_sample - start_sample) / sample_rate,
        "metrics": metrics,
        "best_phase": best,
        "phase_score_spread_pct": spread,
        "clock_status": clock_status(spread, best),
        "timing_score": timing_score,
        "timing_phase_candidates": candidates[:10],
    }


def parse_window_seconds(text: str) -> List[float]:
    out: List[float] = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        value = float(part)
        if value <= 0:
            raise ValueError("window sizes must be positive")
        out.append(value)
    if not out:
        raise ValueError("at least one window size is required")
    return out


def safe_slug(path: Path) -> str:
    return path.stem.replace(" ", "_")


def write_pcm_window(source_samples: Sequence[int], start: int, end: int, out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    window = array.array("h", source_samples[start:end])
    if sys.byteorder != "little":
        window.byteswap()
    out_path.write_bytes(window.tobytes())


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Rank GMSK PCM windows by offline timing/slicer diagnostics.")
    parser.add_argument("pcm_path", help="signed 16-bit little-endian PCM capture")
    parser.add_argument("--sample-rate", type=float, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--symbol-rate", type=float, default=DEFAULT_SYMBOL_RATE)
    parser.add_argument("--windows", default="1,2,5", help="comma-separated window sizes in seconds")
    parser.add_argument("--stride", type=float, default=0.5, help="window stride in seconds")
    parser.add_argument("--max-windows", type=int, default=2000)
    parser.add_argument("--output-dir", default="analysis/gmsk_phase2")
    args = parser.parse_args(argv)

    pcm_path = Path(args.pcm_path)
    if args.sample_rate <= 0 or args.symbol_rate <= 0 or args.stride <= 0:
        raise SystemExit("sample rate, symbol rate, and stride must be positive")
    window_seconds = parse_window_seconds(args.windows)
    samples = load_pcm_s16le(pcm_path)
    sample_rate = float(args.sample_rate)
    symbol_rate = float(args.symbol_rate)
    base_metrics = level_metrics(samples, sample_rate)

    rows: List[Dict[str, object]] = []
    for win_sec in window_seconds:
        win_len = int(round(win_sec * sample_rate))
        stride_len = int(round(args.stride * sample_rate))
        if win_len <= 0 or stride_len <= 0 or win_len > len(samples):
            continue
        start = 0
        count = 0
        while start + win_len <= len(samples):
            rows.append(analyze_window(samples, sample_rate, symbol_rate, start, start + win_len))
            count += 1
            if count >= args.max_windows:
                break
            start += stride_len
    rows.sort(key=lambda row: float(row["timing_score"]), reverse=True)

    best = rows[0] if rows else None
    slug = safe_slug(pcm_path)
    out_dir = Path(args.output_dir)
    windows_dir = out_dir / "windows"
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / f"{slug}_windowed_timing_v2_8_10.json"
    txt_path = out_dir / f"{slug}_windowed_timing_v2_8_10.txt"
    best_pcm_path = windows_dir / f"{slug}_best_window_v2_8_10.pcm"

    if best:
        write_pcm_window(samples, int(best["start_sample"]), int(best["end_sample"]), best_pcm_path)

    samples_per_symbol = sample_rate / symbol_rate
    result = {
        "ok": True,
        "marker": MARKER,
        "created_utc": utc_stamp(),
        "pcm_path": str(pcm_path),
        "sample_rate_hz": sample_rate,
        "assumed_symbol_rate": symbol_rate,
        "samples_per_symbol": samples_per_symbol,
        "source": base_metrics,
        "window_seconds": window_seconds,
        "stride_seconds": args.stride,
        "window_count": len(rows),
        "best_window": best,
        "top_windows": rows[:20],
        "best_window_pcm_path": str(best_pcm_path) if best else None,
        "decoder_claim": "none; windowed offline diagnostics only, no HDLC/AX.25/text decode claimed",
        "next_step": "If a strong or moderate candidate window appears, run HDLC flag probing on the exported best-window PCM; otherwise capture higher-rate discriminator/baseband.",
    }
    json_path.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    lines = [
        f"GMSK windowed timing analyzer ({MARKER})",
        "",
        f"PCM: {pcm_path}",
        f"Sample rate: {sample_rate:.0f} Hz",
        f"Assumed symbol rate: {symbol_rate:.1f} symbols/sec",
        f"Samples: {len(samples)}",
        f"Duration: {len(samples) / sample_rate:.2f} sec",
        f"Samples per symbol: {samples_per_symbol:.2f}",
        f"Source RMS: {float(base_metrics['rms']):.1f}",
        f"Source peak: {int(base_metrics['peak'])}",
        f"Source clipping: {int(base_metrics['clipping_count'])} ({float(base_metrics['clipping_pct']):.4f}%)",
        f"Windows analyzed: {len(rows)}",
        "",
    ]
    if best:
        bp = best.get("best_phase") or {}
        bm = best.get("metrics") or {}
        lines.extend([
            "Best window:",
            f"  start: {float(best['start_seconds']):.2f} sec",
            f"  end: {float(best['end_seconds']):.2f} sec",
            f"  duration: {float(best['duration_seconds']):.2f} sec",
            f"  timing score: {float(best['timing_score']):.6f}",
            f"  clock status: {best['clock_status']}",
            f"  phase: {float(bp.get('phase_samples', 0.0)):.2f} samples",
            f"  phase score: {float(bp.get('score', 0.0)):.6f}",
            f"  phase score spread: {float(best['phase_score_spread_pct']):.2f}%",
            f"  p25 abs normalized: {float(bp.get('p25_abs_norm', 0.0)):.4f}",
            f"  near-zero: {float(bp.get('near_zero_pct', 0.0)):.1f}%",
            f"  positive balance: {float(bp.get('positive_pct', 0.0)):.1f}%",
            f"  transitions/symbol: {float(bp.get('transition_per_symbol', 0.0)):.3f}",
            f"  RMS: {float(bm.get('rms', 0.0)):.1f}",
            f"  peak: {int(bm.get('peak', 0))}",
            f"  exported PCM: {best_pcm_path}",
            "",
        ])
    lines.extend([
        "Top windows:",
    ])
    for idx, row in enumerate(rows[:10], 1):
        bp = row.get("best_phase") or {}
        lines.append(
            f"  {idx:02d}. {float(row['start_seconds']):6.2f}-{float(row['end_seconds']):6.2f}s "
            f"score={float(row['timing_score']):.6f} "
            f"spread={float(row['phase_score_spread_pct']):5.2f}% "
            f"phase={float(bp.get('phase_samples', 0.0)):4.2f} "
            f"clock={row['clock_status']}"
        )
    lines.extend([
        "",
        "Decoder claim: none; windowed offline diagnostics only, no HDLC/AX.25/text decode claimed",
        "Next step: If a strong or moderate candidate window appears, run HDLC flag probing on the exported best-window PCM; otherwise capture higher-rate discriminator/baseband.",
        "",
        f"JSON: {json_path}",
        f"TXT:  {txt_path}",
    ])
    txt_path.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
