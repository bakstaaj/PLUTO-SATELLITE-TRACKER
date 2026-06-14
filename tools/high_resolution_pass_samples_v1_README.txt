# High-Resolution Pass Samples v1

Fixes the root cause of 4-point maps and 4-point Doppler plans.

## Problem

The pass generator used the coarse pass-detection samples directly for:

- `ground_track`
- `doppler_plan.points`

So if a pass detection had only 4 visible samples, both the map and radio plan only had 4 points.

## Fix

`tools/update_pass_predictions.py` now keeps the coarse `--step-seconds` pass scan, but after a pass is detected it resamples the actual AOS-to-LOS window at `--pass-sample-seconds`.

Default:

```text
--step-seconds 30
--pass-sample-seconds 5
```

This makes both map and radio data granular in `/api/passes`.

## Run

```bash
python tools/apply_high_resolution_pass_samples_v1.py .
./tools/validate_high_resolution_pass_samples_v1.sh .
python tools/test_high_resolution_pass_samples_v1.py
./tools/deploy_and_reboot.sh
```

After deploy, regenerate passes from the UI or run your existing pass repair/rebuild helper so `/api/passes` is rebuilt with the new sample interval.
