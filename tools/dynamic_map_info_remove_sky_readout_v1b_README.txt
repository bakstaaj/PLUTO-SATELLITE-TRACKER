# Dynamic Map Info / Remove Sky Readout v1b

Corrective version of v1.

## Why

v1 expected the map-info block to be in an older position before the legend. Your current layout has already been rearranged by compact UI patches.

v1b finds the actual map-info block and changes the displayed values from `focusPoint` to `livePoint`.

## Changes

- Map info box now uses `livePoint`.
- Sample date/time updates dynamically.
- Ground Point updates dynamically.
- Look Angles update dynamically.
- Altitude updates dynamically.
- Redundant `Current look angle` block under the azimuth plot is removed.
- Listen control stays under the azimuth plot.

## Run

```bash
python tools/apply_dynamic_map_info_remove_sky_readout_v1b.py .
./tools/validate_dynamic_map_info_remove_sky_readout_v1b.sh .
```

If needed:

```bash
./tools/diagnose_dynamic_map_info_remove_sky_readout_v1b.sh .
```
