# Live RX Frequency Actual v2

Fixes the v1 generated Python syntax error.

## Change

Adds a `Live RX` row immediately after the existing live map-info `Altitude` row.

Value source:

1. `lastTrackState.rx_hz`
2. fallback to `livePoint.rx_hz`
3. `-` if no RX frequency is available

## Run

```bash
python tools/apply_live_rx_frequency_actual_v2.py .
./tools/validate_live_rx_frequency_actual_v2.sh .
```

If needed:

```bash
./tools/diagnose_live_rx_frequency_actual_v2.sh .
```
