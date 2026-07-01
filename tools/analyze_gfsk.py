#!/usr/bin/env python3
"""
analyze_gfsk.py  --  offline GFSK decode pipeline inspector

Runs the exact same pipeline as pluto_digital_decoder.c and prints
a detailed report of what each stage produces.

Usage (synthetic signal — no hardware needed):
    python3 tools/analyze_gfsk.py --synthetic

Usage (real IQ capture from Pluto):
    # 1. On Pluto (while test_gen is running in another SSH):
    #    iio_readdev -b 96000 -s 480000 cf-ad9361-lpc voltage0 voltage1 > /tmp/cap.raw
    # 2. Copy to host:
    #    sshpass -p analog scp -O root@192.168.68.104:/tmp/cap.raw /tmp/loopback.raw
    # 3. Run analysis:
    python3 tools/analyze_gfsk.py --iq /tmp/loopback.raw
"""

import sys
import math
import struct
import argparse

# ── Parameters — must match pluto_digital_decoder.c ───────────────────────────
IQ_RATE      = 2_400_000
GFSK_DECIM   = 25
AUDIO_RATE   = IQ_RATE // GFSK_DECIM   # 96 000
BAUD         = 9600
SPS          = AUDIO_RATE // BAUD       # 10
DEVIATION_HZ = BAUD * 0.25             # 2 400 Hz
DC_ALPHA     = 0.950
GATE_LO      = 15_000
GATE_HI      = 100_000_000
TED_GAIN     = 0.10

# ── FM demodulate + decimate  (matches fm_process() exactly) ──────────────────
def fm_demod(i_arr, q_arr):
    pcm = []
    acc = 0; acc_n = 0
    prev_i = prev_q = 0.0; have_prev = False
    for i_raw, q_raw in zip(i_arr, q_arr):
        i = float(i_raw); q = float(q_raw)
        if have_prev:
            cross = prev_i * q - prev_q * i
            dot   = prev_i * i + prev_q * q
            demod = math.atan2(cross, dot)
        else:
            demod = 0.0
        have_prev = True; prev_i = i; prev_q = q
        acc  += round(demod * 8192.0)
        acc_n += 1
        if acc_n >= GFSK_DECIM:
            v = acc // GFSK_DECIM
            pcm.append(max(-32768, min(32767, v)))
            acc = 0; acc_n = 0
    return pcm

# ── Gated DC block + MA filter + Gardner TED  (matches run_gfsk() exactly) ────
def full_pipeline(i_arr, q_arr, pcm_arr):
    dc_avg   = 0.0
    ma_buf   = [0.0] * SPS
    ma_sum   = 0.0
    ma_idx   = 0
    phi      = 0.0
    filt_prev = 0.0; filt_mid = 0.0; mid_taken = False

    iq_block = 0.0
    pcm_idx  = 0

    # per-window stats
    bits = []; gate_events = []
    pcm_vals = []; y_vals = []; filt_vals = []; dc_vals = []
    gate_open_pcm = []; gate_closed_n = 0; sat_n = 0

    for raw_idx in range(len(i_arr)):
        iq_block += float(i_arr[raw_idx])**2 + float(q_arr[raw_idx])**2

        if (raw_idx + 1) % GFSK_DECIM != 0:
            continue
        if pcm_idx >= len(pcm_arr):
            break
        pcm = pcm_arr[pcm_idx]; pcm_idx += 1

        # DC gate
        if iq_block > GATE_LO and iq_block < GATE_HI:
            dc_avg = DC_ALPHA * dc_avg + (1.0 - DC_ALPHA) * pcm
            gate_open_pcm.append(pcm)
            gate_events.append(True)
        else:
            gate_events.append(False)
            if iq_block >= GATE_HI:
                sat_n += 1
            else:
                gate_closed_n += 1

        y = pcm - dc_avg
        pcm_vals.append(pcm); y_vals.append(y); dc_vals.append(dc_avg)

        # MA filter
        ma_sum -= ma_buf[ma_idx]
        ma_buf[ma_idx] = y
        ma_sum += y
        ma_idx = (ma_idx + 1) % SPS
        filtered = ma_sum
        filt_vals.append(filtered)

        # Gardner TED + bit decision
        phi += 1.0
        if not mid_taken and phi >= SPS * 0.5:
            filt_mid = filtered; mid_taken = True

        if phi >= SPS:
            phi -= SPS; mid_taken = False
            bit = 1 if filtered >= 0.0 else 0
            bits.append(bit)

            sign_now  =  1.0 if filtered  >= 0.0 else -1.0
            sign_prev =  1.0 if filt_prev >= 0.0 else -1.0
            amp = abs(filtered) + abs(filt_prev) + 1e-10
            ted_err = (filt_mid / amp) * (sign_now - sign_prev)
            phi -= TED_GAIN * ted_err
            if phi < 0:     phi += SPS
            if phi >= SPS:  phi -= SPS
            filt_prev = filtered

        iq_block = 0.0

    return dict(
        pcm=pcm_vals, y=y_vals, filtered=filt_vals, dc=dc_vals,
        gate=gate_events, bits=bits,
        gate_open_pcm=gate_open_pcm,
        gate_closed_n=gate_closed_n, sat_n=sat_n
    )

# ── Synthetic GFSK signal generator ───────────────────────────────────────────
def make_synthetic_iq(carrier_offset_hz=0, n_flags=24, n_data_bytes=10,
                      amplitude=500, noise_amp=15):
    """
    Generate a synthetic GFSK IQ burst:
      n_flags HDLC flag bytes (0x7E) + n_data_bytes random data + 4 flag bytes
    followed by silence.  The carrier is at carrier_offset_hz from the LO.
    """
    import random
    random.seed(42)

    # Build bit stream: preamble flags + frame + tail flags
    def byte_bits(b):
        return [(b >> i) & 1 for i in range(8)]

    flag = 0x7E
    bits = []
    for _ in range(n_flags):
        bits.extend(byte_bits(flag))
    for _ in range(n_data_bytes):
        bits.extend(byte_bits(random.randint(0, 255)))
    for _ in range(4):
        bits.extend(byte_bits(flag))

    # GFSK: FM modulate at IQ_RATE
    dev = 2.0 * math.pi * DEVIATION_HZ / IQ_RATE
    carrier = 2.0 * math.pi * carrier_offset_hz / IQ_RATE

    samples_per_bit = IQ_RATE // BAUD   # 250
    phase = 0.0
    i_arr = []; q_arr = []

    for bit in bits:
        freq = carrier + (dev if bit else -dev)
        for _ in range(samples_per_bit):
            phase += freq
            ni = round(amplitude * math.cos(phase) + random.gauss(0, noise_amp))
            nq = round(amplitude * math.sin(phase) + random.gauss(0, noise_amp))
            i_arr.append(max(-32767, min(32767, ni)))
            q_arr.append(max(-32767, min(32767, nq)))

    # 200ms silence
    silence = IQ_RATE // 5
    i_arr.extend([0] * silence)
    q_arr.extend([0] * silence)

    return i_arr, q_arr, bits

# ── HDLC flag finder ──────────────────────────────────────────────────────────
def find_hdlc_flags(bits):
    FLAG = [0,1,1,1,1,1,1,0]   # 0x7E LSB-first
    count = 0
    for i in range(len(bits) - 8):
        if bits[i:i+8] == FLAG:
            count += 1
    return count

# ── Simple stats helper ────────────────────────────────────────────────────────
def stats(arr):
    if not arr: return "  (empty)"
    n = len(arr)
    mn = min(arr); mx = max(arr)
    mean = sum(arr) / n
    variance = sum((x - mean)**2 for x in arr) / n
    std = math.sqrt(variance)
    return f"  n={n}  min={mn:.0f}  max={mx:.0f}  mean={mean:.1f}  std={std:.1f}"

# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--synthetic', action='store_true',
                    help='Use synthetic GFSK signal (no hardware needed)')
    ap.add_argument('--iq', metavar='FILE',
                    help='Raw IQ capture file (16-bit LE interleaved I/Q)')
    ap.add_argument('--offset', type=int, default=0,
                    help='Carrier offset in Hz for synthetic signal (default 0)')
    ap.add_argument('--amplitude', type=int, default=500,
                    help='Signal amplitude for synthetic (default 500; 2048=full scale)')
    ap.add_argument('--noise', type=int, default=15,
                    help='Noise amplitude for synthetic (default 15)')
    args = ap.parse_args()

    if not args.synthetic and not args.iq:
        ap.print_help()
        sys.exit(1)

    # ── Load IQ data ──────────────────────────────────────────────────────────
    if args.synthetic:
        print(f"Generating synthetic GFSK: offset={args.offset} Hz  "
              f"amplitude={args.amplitude}  noise={args.noise}")
        i_arr, q_arr, tx_bits = make_synthetic_iq(
            carrier_offset_hz=args.offset,
            amplitude=args.amplitude,
            noise_amp=args.noise)
        print(f"TX: {len(tx_bits)} bits  "
              f"{find_hdlc_flags(tx_bits)} HDLC flags in {len(i_arr)} IQ samples "
              f"({len(i_arr)/IQ_RATE*1000:.1f} ms)")
    else:
        print(f"Reading IQ from {args.iq}")
        with open(args.iq, 'rb') as f:
            raw = f.read()
        n_samples = len(raw) // 4
        print(f"  {n_samples} IQ samples  ({n_samples/IQ_RATE*1000:.1f} ms)")
        i_arr = []; q_arr = []
        for k in range(n_samples):
            off = k * 4
            iv = struct.unpack_from('<h', raw, off)[0]
            qv = struct.unpack_from('<h', raw, off+2)[0]
            i_arr.append(iv); q_arr.append(qv)
        tx_bits = None

    # ── Stage 1: IQ power ─────────────────────────────────────────────────────
    print("\n=== Stage 1: IQ Power ===")
    n_blk = len(i_arr) // GFSK_DECIM
    blk_powers = []
    for k in range(n_blk):
        pwr = sum(float(i_arr[k*GFSK_DECIM+j])**2 + float(q_arr[k*GFSK_DECIM+j])**2
                  for j in range(GFSK_DECIM))
        blk_powers.append(pwr)
    above_lo  = sum(1 for p in blk_powers if p > GATE_LO)
    above_hi  = sum(1 for p in blk_powers if p >= GATE_HI)
    in_window = sum(1 for p in blk_powers if GATE_LO < p < GATE_HI)
    print(f"  blocks: {n_blk}  (gate_lo={GATE_LO}  gate_hi={GATE_HI})")
    print(f"  block power:" + stats(blk_powers))
    print(f"  > gate_lo  : {above_lo} ({100*above_lo/n_blk:.1f}%)")
    print(f"  >= gate_hi (saturation): {above_hi} ({100*above_hi/n_blk:.1f}%)")
    print(f"  in window (lo..hi): {in_window} ({100*in_window/n_blk:.1f}%)")
    if in_window > 0:
        window_pwr = [p for p in blk_powers if GATE_LO < p < GATE_HI]
        avg_a = math.sqrt(sum(window_pwr)/len(window_pwr)/GFSK_DECIM)
        print(f"  avg IQ amplitude in window: {avg_a:.1f} counts")
        expected_dev_pcm = round(2*math.pi*DEVIATION_HZ/IQ_RATE * 8192)
        print(f"  expected GFSK deviation PCM: ±{expected_dev_pcm}")
        print(f"  expected SNR_IQ (A/noise_floor): "
              f"≈{avg_a:.1f}/{math.sqrt(GATE_LO/GFSK_DECIM):.1f} = "
              f"{avg_a/math.sqrt(GATE_LO/GFSK_DECIM):.2f}")

    # ── Stage 2: FM demodulation ──────────────────────────────────────────────
    print("\n=== Stage 2: FM Demodulation (fm_process) ===")
    pcm = fm_demod(i_arr, q_arr)
    print(f"  audio samples: {len(pcm)}")
    print("  pcm values  :" + stats(pcm))
    expected_pcm = round(2*math.pi*DEVIATION_HZ/IQ_RATE * 8192)
    print(f"  expected deviation PCM: ±{expected_pcm}  (for {DEVIATION_HZ:.0f} Hz dev)")
    print(f"  expected carrier DC  : pcm_dc = f_offset × {8192*2*math.pi/IQ_RATE:.4f}")
    # Histogram
    buckets = {}
    for v in pcm:
        b = (v // 500) * 500
        buckets[b] = buckets.get(b, 0) + 1
    print("  histogram (bucket=500):")
    for k in sorted(buckets):
        bar = '#' * (buckets[k] * 40 // max(buckets.values()))
        print(f"    {k:+6d}: {bar} ({buckets[k]})")

    # ── Stage 3: DC block + MA filter ─────────────────────────────────────────
    print("\n=== Stage 3: DC Block + MA Filter + TED ===")
    res = full_pipeline(i_arr, q_arr, pcm)

    n_gate_open = sum(1 for g in res['gate'] if g)
    n_total     = len(res['gate'])
    print(f"  gate-open : {n_gate_open}/{n_total} ({100*n_gate_open/n_total:.1f}%)")
    print(f"  saturated (above gate_hi): {res['sat_n']}")
    print(f"  held (below gate_lo)     : {res['gate_closed_n']}")

    if res['gate_open_pcm']:
        mean_pcm = sum(res['gate_open_pcm']) / len(res['gate_open_pcm'])
        print(f"\n  pcm_mean during gate-open: {mean_pcm:.1f}")
        offset_hz = mean_pcm * IQ_RATE / (8192 * 2 * math.pi)
        print(f"  → implied carrier offset: {offset_hz:.0f} Hz")
    else:
        print("\n  WARNING: gate NEVER opened — check gate thresholds and signal level")

    print("\n  dc_avg at end of run: " + f"{res['dc'][-1]:.1f}" if res['dc'] else "  (no data)")
    print("  y (after DC sub)   :" + stats(res['y']))
    print("  filtered (MA out)  :" + stats(res['filtered']))

    expected_filtered_pk = SPS * expected_dev_pcm
    print(f"\n  expected filtered peak (sustained bit): ±{expected_filtered_pk}")
    if res['filtered']:
        actual_pk = max(abs(v) for v in res['filtered'])
        ratio = actual_pk / expected_filtered_pk if expected_filtered_pk else 0
        print(f"  actual   filtered peak              : {actual_pk:.0f}  ({ratio:.1f}× expected)")
        if ratio > 3:
            dc_contribution = abs(res['dc'][-1]) * SPS if res['dc'] else 0
            print(f"  → excess likely from residual DC: dc_avg={res['dc'][-1]:.1f} → "
                  f"DC contribution to filtered ≈ {dc_contribution:.0f}")

    # ── Stage 4: Bits ─────────────────────────────────────────────────────────
    print("\n=== Stage 4: Bit Stream ===")
    bits = res['bits']
    print(f"  decoded bits : {len(bits)}")
    if bits:
        ones_pct = 100 * sum(bits) / len(bits)
        print(f"  ones        : {ones_pct:.1f}%  (50% = random/noise, <50% may be inverted)")
        print(f"  first 80 bits: {''.join(str(b) for b in bits[:80])}")
        n_flags = find_hdlc_flags(bits)
        print(f"  HDLC 0x7E flags found: {n_flags}")
        if tx_bits:
            matches = sum(a==b for a,b in zip(bits, tx_bits[:len(bits)]))
            ber = 1 - matches/len(bits)
            print(f"  BER vs TX   : {ber:.3f}  ({100*ber:.1f}%)")

    # ── Summary ───────────────────────────────────────────────────────────────
    print("\n=== Summary ===")
    if not res['gate_open_pcm']:
        print("  FAIL: gate never opened. Signal absent or below GATE_LO threshold.")
        print(f"        max IQ block power = {max(blk_powers):.0f}  threshold = {GATE_LO}")
    elif res['sat_n'] > n_total * 0.05:
        print(f"  WARN: {res['sat_n']} saturation events ({100*res['sat_n']/n_total:.1f}%)")
        print("        AGC not settled before update window. Try --atten 60 in test_gen.")
    else:
        mean_pcm = sum(res['gate_open_pcm']) / len(res['gate_open_pcm'])
        offset_hz = mean_pcm * IQ_RATE / (8192 * 2 * math.pi)
        if abs(offset_hz) > 5000:
            print(f"  WARN: carrier offset {offset_hz:.0f} Hz — dc_avg will converge to {mean_pcm:.0f}")
            print(f"        run decoder at --freq-hz {437000000 + int(offset_hz)}")
        else:
            print(f"  OK: carrier offset {offset_hz:.0f} Hz (small, no LO adjustment needed)")

        if res['filtered']:
            actual_pk = max(abs(v) for v in res['filtered'])
            if actual_pk > expected_filtered_pk * 5:
                print(f"  FAIL: filtered_peak={actual_pk:.0f} >> expected {expected_filtered_pk}")
                print("        DC not removed — check dc_avg convergence above")
            else:
                print(f"  OK: filtered_peak={actual_pk:.0f} ≈ expected {expected_filtered_pk}")

        if bits:
            n_flags = find_hdlc_flags(bits)
            if n_flags == 0:
                print("  FAIL: no HDLC 0x7E flags found in bit stream")
            else:
                print(f"  OK: {n_flags} HDLC flags found")

if __name__ == '__main__':
    main()
