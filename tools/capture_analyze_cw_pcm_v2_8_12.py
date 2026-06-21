#!/usr/bin/env python3
"""CW tone/envelope capture and diagnostic analyzer for Pluto Satellite Tracker v2.8.12.

This tool captures the existing backend live WAV stream or analyzes an existing PCM/WAV file.
It is diagnostic-only: it reports tone/envelope/timing candidates and only marks text
recovery as tentative when the timing looks plausible. It does not change Pluto backend C.
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import os
from pathlib import Path
import statistics
import struct
import sys
import tempfile
import urllib.request
import wave

MARKER = "CW_TONE_ENVELOPE_ANALYZER_V2_8_12"
DEFAULT_HOST = "192.168.68.104"
DEFAULT_PORT = 8080
DEFAULT_SAMPLE_RATE = 24000
MORSE_TABLE = {
    ".-": "A", "-...": "B", "-.-.": "C", "-..": "D", ".": "E", "..-.": "F",
    "--.": "G", "....": "H", "..": "I", ".---": "J", "-.-": "K", ".-..": "L",
    "--": "M", "-.": "N", "---": "O", ".--.": "P", "--.-": "Q", ".-.": "R",
    "...": "S", "-": "T", "..-": "U", "...-": "V", ".--": "W", "-..-": "X",
    "-.--": "Y", "--..": "Z", ".----": "1", "..---": "2", "...--": "3", "....-": "4",
    ".....": "5", "-....": "6", "--...": "7", "---..": "8", "----.": "9", "-----": "0",
    ".-.-.-": ".", "--..--": ",", "..--..": "?", "-..-.": "/", "-....-": "-", "-.--.": "(", "-.--.-": ")",
}


def utc_stamp() -> str:
    return _dt.datetime.now(_dt.UTC).strftime("%Y%m%dT%H%M%SZ")


def read_wav_or_pcm(path: Path, sample_rate: int) -> tuple[list[int], int, str]:
    suffix = path.suffix.lower()
    if suffix == ".wav":
        with wave.open(str(path), "rb") as wf:
            channels = wf.getnchannels()
            width = wf.getsampwidth()
            rate = wf.getframerate()
            frames = wf.readframes(wf.getnframes())
        if width != 2:
            raise SystemExit(f"only 16-bit WAV is supported, got {width * 8}-bit")
        raw = struct.unpack("<" + "h" * (len(frames) // 2), frames)
        if channels == 1:
            return list(raw), rate, "wav"
        return [raw[i] for i in range(0, len(raw), channels)], rate, "wav"
    data = path.read_bytes()
    return list(struct.unpack("<" + "h" * (len(data) // 2), data[: len(data) - (len(data) % 2)])), sample_rate, "pcm"


def write_pcm(path: Path, samples: list[int]) -> None:
    path.write_bytes(struct.pack("<" + "h" * len(samples), *samples))


def wav_payload_to_samples(payload: bytes, fallback_rate: int) -> tuple[list[int], int, str]:
    if payload[:4] == b"RIFF" and b"WAVE" in payload[:16]:
        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
            tmp.write(payload)
            tmp_path = Path(tmp.name)
        try:
            return read_wav_or_pcm(tmp_path, fallback_rate)
        finally:
            tmp_path.unlink(missing_ok=True)
    return list(struct.unpack("<" + "h" * (len(payload) // 2), payload[: len(payload) - (len(payload) % 2)])), fallback_rate, "pcm"


def rms(samples: list[int]) -> float:
    if not samples:
        return 0.0
    return math.sqrt(sum(float(x) * float(x) for x in samples) / len(samples))


def zero_cross_tone(samples: list[int], sample_rate: int) -> float:
    if len(samples) < 2:
        return 0.0
    crossings = 0
    prev = samples[0]
    for cur in samples[1:]:
        if (prev < 0 <= cur) or (prev > 0 >= cur):
            crossings += 1
        prev = cur
    duration = len(samples) / float(sample_rate)
    return (crossings / 2.0) / duration if duration > 0 else 0.0


def goertzel_power(samples: list[int], sample_rate: int, freq: float, max_samples: int = 48000) -> float:
    if not samples:
        return 0.0
    if len(samples) > max_samples:
        step = max(1, len(samples) // max_samples)
        work = samples[::step][:max_samples]
        effective_rate = sample_rate / step
    else:
        work = samples
        effective_rate = sample_rate
    omega = 2.0 * math.pi * freq / effective_rate
    coeff = 2.0 * math.cos(omega)
    q0 = q1 = q2 = 0.0
    for s in work:
        q0 = coeff * q1 - q2 + float(s)
        q2 = q1
        q1 = q0
    return q1 * q1 + q2 * q2 - coeff * q1 * q2


def estimate_cw_band_tone(samples: list[int], sample_rate: int) -> dict:
    # CW side-tone should normally be audio-ish, so search a practical passband.
    freqs = list(range(300, 1601, 25))
    powers = [(freq, goertzel_power(samples, sample_rate, float(freq))) for freq in freqs]
    powers.sort(key=lambda item: item[1], reverse=True)
    best_freq, best_power = powers[0]
    median_power = statistics.median([p for _, p in powers]) if powers else 0.0
    ratio = best_power / median_power if median_power > 0 else 0.0
    return {
        "best_tone_hz": float(best_freq),
        "best_power": best_power,
        "median_power": median_power,
        "tone_to_median_ratio": ratio,
        "top_candidates": [{"hz": f, "power": p} for f, p in powers[:8]],
    }


def moving_average_abs(samples: list[int], window: int) -> list[float]:
    window = max(1, window)
    out: list[float] = []
    acc = 0.0
    q: list[float] = []
    for s in samples:
        v = abs(float(s))
        q.append(v)
        acc += v
        if len(q) > window:
            acc -= q.pop(0)
        out.append(acc / len(q))
    return out


def run_lengths(bits: list[int]) -> list[tuple[int, int]]:
    if not bits:
        return []
    out: list[tuple[int, int]] = []
    state = bits[0]
    count = 1
    for bit in bits[1:]:
        if bit == state:
            count += 1
        else:
            out.append((state, count))
            state = bit
            count = 1
    out.append((state, count))
    return out


def decode_runs_to_morse(runs: list[tuple[int, int]], unit_samples: float) -> tuple[str, str, dict]:
    if unit_samples <= 0:
        return "", "", {"decode_reliable": False, "reason": "invalid unit"}
    symbols: list[str] = []
    current = ""
    on_lengths = []
    off_lengths = []
    for state, length in runs:
        units = length / unit_samples
        if state:
            on_lengths.append(units)
            current += "-" if units >= 2.2 else "."
        else:
            off_lengths.append(units)
            if units >= 6.0:
                if current:
                    symbols.append(current)
                    current = ""
                symbols.append("/")
            elif units >= 2.2:
                if current:
                    symbols.append(current)
                    current = ""
    if current:
        symbols.append(current)
    morse = " ".join(symbols).replace(" / ", " / ")
    chars = []
    unknown = 0
    known = 0
    for token in symbols:
        if token == "/":
            chars.append(" ")
            continue
        ch = MORSE_TABLE.get(token)
        if ch:
            chars.append(ch)
            known += 1
        else:
            chars.append("?")
            unknown += 1
    text = "".join(chars).strip()
    total = known + unknown
    reliable = total >= 3 and known >= max(2, unknown * 2)
    return morse, text, {
        "known_symbols": known,
        "unknown_symbols": unknown,
        "decode_reliable": reliable,
        "on_units_median": statistics.median(on_lengths) if on_lengths else 0.0,
        "off_units_median": statistics.median(off_lengths) if off_lengths else 0.0,
    }


def analyze_samples(samples: list[int], sample_rate: int) -> dict:
    if not samples:
        return {"ok": False, "error": "no samples"}
    sample_count = len(samples)
    duration = sample_count / float(sample_rate)
    mean = sum(samples) / float(sample_count)
    centered = [int(max(-32768, min(32767, round(x - mean)))) for x in samples]
    r = rms(centered)
    peak = max(abs(x) for x in centered)
    clipping = sum(1 for x in samples if x <= -32768 or x >= 32767)
    broad_tone = zero_cross_tone(centered, sample_rate)
    cw_tone = estimate_cw_band_tone(centered, sample_rate)

    env_window = max(8, int(sample_rate * 0.006))
    env = moving_average_abs(centered, env_window)
    sorted_env = sorted(env)
    p20 = sorted_env[int(0.20 * (len(sorted_env) - 1))]
    p80 = sorted_env[int(0.80 * (len(sorted_env) - 1))]
    threshold = p20 + 0.55 * (p80 - p20)
    raw_bits = [1 if v >= threshold else 0 for v in env]
    # Light debouncing at roughly 8 ms.
    debounce = max(1, int(sample_rate * 0.008))
    runs = run_lengths(raw_bits)
    debounced: list[int] = []
    for idx, (state, length) in enumerate(runs):
        if length < debounce and debounced:
            debounced.extend([debounced[-1]] * length)
        else:
            debounced.extend([state] * length)
    runs2 = run_lengths(debounced)
    key_duty = sum(length for state, length in runs2 if state) / max(1, len(debounced))
    on_lengths = [length for state, length in runs2 if state and length >= debounce]
    if on_lengths:
        # The shortest third of on-runs usually approximates dits.
        short = sorted(on_lengths)[: max(1, len(on_lengths) // 3)]
        unit_samples = max(float(debounce), statistics.median(short))
    else:
        unit_samples = sample_rate * 0.06
    unit_ms = 1000.0 * unit_samples / sample_rate
    morse, text, decode_meta = decode_runs_to_morse(runs2, unit_samples)

    tone_plausible = 300.0 <= cw_tone["best_tone_hz"] <= 1600.0 and cw_tone["tone_to_median_ratio"] >= 2.0
    duty_plausible = 0.05 <= key_duty <= 0.75
    timing_plausible = 15.0 <= unit_ms <= 250.0
    reliability = "candidate" if tone_plausible and duty_plausible and timing_plausible else "diagnostic_only"

    return {
        "ok": True,
        "marker": MARKER,
        "sample_rate_hz": sample_rate,
        "sample_count": sample_count,
        "duration_seconds": duration,
        "rms": r,
        "peak": peak,
        "dc_offset": mean,
        "clipping_count": clipping,
        "clipping_pct": 100.0 * clipping / sample_count,
        "broad_zero_crossing_tone_hz": broad_tone,
        "cw_band_tone": cw_tone,
        "envelope": {
            "window_samples": env_window,
            "threshold": threshold,
            "p20": p20,
            "p80": p80,
            "key_duty_pct": key_duty * 100.0,
            "debounce_samples": debounce,
            "unit_samples": unit_samples,
            "unit_ms": unit_ms,
            "run_count": len(runs2),
        },
        "morse_symbols": morse,
        "tentative_text": text,
        "decode_meta": decode_meta,
        "reliability": reliability,
        "decoder_claim": "tentative CW only" if reliability == "candidate" and decode_meta.get("decode_reliable") else "none; diagnostic-only, no reliable CW text decode claimed",
    }


def write_outputs(base: Path, samples: list[int], sample_rate: int, analysis: dict, out_dir: Path) -> dict:
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = base.stem
    pcm_path = out_dir / f"{stem}_cw_v2_8_12.pcm"
    json_path = out_dir / f"{stem}_cw_analysis_v2_8_12.json"
    txt_path = out_dir / f"{stem}_cw_analysis_v2_8_12.txt"
    write_pcm(pcm_path, samples)
    analysis = dict(analysis)
    analysis.update({"pcm_path": str(pcm_path), "json_path": str(json_path), "text_summary_path": str(txt_path)})
    json_path.write_text(json.dumps(analysis, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    txt = [
        f"CW tone/envelope analyzer ({MARKER})",
        "",
        f"Samples: {analysis['sample_count']}",
        f"Duration: {analysis['duration_seconds']:.2f} sec",
        f"Sample rate: {analysis['sample_rate_hz']} Hz",
        f"RMS: {analysis['rms']:.1f}",
        f"Peak: {analysis['peak']}",
        f"Broad zero-crossing tone: {analysis['broad_zero_crossing_tone_hz']:.1f} Hz",
        f"CW-band tone candidate: {analysis['cw_band_tone']['best_tone_hz']:.1f} Hz",
        f"Tone/median ratio: {analysis['cw_band_tone']['tone_to_median_ratio']:.2f}",
        f"Key duty: {analysis['envelope']['key_duty_pct']:.1f}%",
        f"Envelope threshold: {analysis['envelope']['threshold']:.1f}",
        f"Estimated unit: {analysis['envelope']['unit_ms']:.1f} ms",
        f"Reliability: {analysis['reliability']}",
        f"Morse symbols: {analysis['morse_symbols']}",
        f"Tentative text: {analysis['tentative_text']}",
        f"Decoder claim: {analysis['decoder_claim']}",
        "",
        f"JSON: {json_path}",
        f"PCM:  {pcm_path}",
    ]
    txt_path.write_text("\n".join(txt) + "\n", encoding="utf-8")
    return analysis


def self_test() -> int:
    sr = DEFAULT_SAMPLE_RATE
    # Synthetic 700 Hz keyed tone: CQ CQ using simple timing.
    unit = int(sr * 0.06)
    message = "-.-. --.- / -.-. --.-"
    samples: list[int] = []
    phase = 0.0
    for token in message.split(" "):
        if token == "/":
            samples.extend([0] * (unit * 7))
            continue
        for mark in token:
            on_units = 3 if mark == "-" else 1
            for _ in range(on_units * unit):
                samples.append(int(4000 * math.sin(phase)))
                phase += 2.0 * math.pi * 700.0 / sr
            samples.extend([0] * unit)
        samples.extend([0] * (unit * 2))
    analysis = analyze_samples(samples, sr)
    if not analysis.get("ok"):
        print(json.dumps(analysis))
        return 1
    print(json.dumps(analysis, sort_keys=True))
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Capture/analyze CW PCM from Pluto live audio stream.")
    parser.add_argument("input", nargs="?", help="Optional existing .pcm or .wav file to analyze")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", default=str(DEFAULT_PORT))
    parser.add_argument("--seconds", type=int, default=10)
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--output-dir", default="captures/cw_v2_8_12")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args(argv)
    if args.self_test:
        return self_test()

    out_dir = Path(args.output_dir)
    if args.input:
        in_path = Path(args.input)
        samples, sample_rate, fmt = read_wav_or_pcm(in_path, args.sample_rate)
        base = in_path
        source = str(in_path)
    else:
        stamp = utc_stamp()
        url = f"http://{args.host}:{args.port}/api/radio/audio/live.wav?stream=1&seconds={args.seconds}"
        payload = urllib.request.urlopen(url, timeout=max(20, args.seconds + 10)).read()
        samples, sample_rate, fmt = wav_payload_to_samples(payload, args.sample_rate)
        out_dir.mkdir(parents=True, exist_ok=True)
        raw_path = out_dir / f"cw_capture_{stamp}.{fmt if fmt in ('wav','pcm') else 'raw'}"
        raw_path.write_bytes(payload)
        base = raw_path
        source = url

    analysis = analyze_samples(samples, sample_rate)
    analysis.update({
        "created_utc": utc_stamp(),
        "source": source,
        "host": args.host,
        "port": args.port,
        "requested_seconds": args.seconds,
    })
    final = write_outputs(base, samples, sample_rate, analysis, out_dir)

    print(f"CW tone/envelope analyzer ({MARKER})")
    print("")
    print(f"Source: {source}")
    print(f"Sample rate: {sample_rate} Hz")
    print(f"Samples: {final['sample_count']}")
    print(f"Duration: {final['duration_seconds']:.2f} sec")
    print(f"RMS: {final['rms']:.1f}")
    print(f"Peak: {final['peak']}")
    print(f"Broad zero-crossing tone: {final['broad_zero_crossing_tone_hz']:.1f} Hz")
    print(f"CW-band tone candidate: {final['cw_band_tone']['best_tone_hz']:.1f} Hz")
    print(f"Key duty: {final['envelope']['key_duty_pct']:.1f}%")
    print(f"Estimated unit: {final['envelope']['unit_ms']:.1f} ms")
    print(f"Reliability: {final['reliability']}")
    print(f"Morse symbols: {final['morse_symbols']}")
    print(f"Tentative text: {final['tentative_text']}")
    print(f"Decoder claim: {final['decoder_claim']}")
    print("")
    print("Artifacts:")
    print(f"  PCM:  {final['pcm_path']}")
    print(f"  JSON: {final['json_path']}")
    print(f"  TXT:  {final['text_summary_path']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
