# Remove Stale Current Look Angle v1c

Corrective patch after `DYNAMIC_MAP_INFO_REMOVE_SKY_READOUT_V1B`.

## Why

v1b successfully changed the map info box to live/current values, but validation still found residual `Current look angle` text somewhere else in `web/index.html`.

## Run

```bash
python tools/apply_remove_stale_current_look_angle_v1c.py .
./tools/validate_remove_stale_current_look_angle_v1c.sh .
./tools/validate_dynamic_map_info_remove_sky_readout_v1b.sh .
```

If needed:

```bash
./tools/diagnose_remove_stale_current_look_angle_v1c.sh .
```
