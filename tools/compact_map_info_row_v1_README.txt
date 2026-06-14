# Compact Map Info Row v1

CSS-only UI cleanup.

## Change

Compresses the live map-info row so these values stay on a single compact row:

- Sample
- Ground Point
- Look Angles
- Altitude
- Live RX

The labels are displayed inline with smaller text. Values use no-wrap plus ellipsis rather than wrapping.

## Run

```bash
python tools/apply_compact_map_info_row_v1.py .
./tools/validate_compact_map_info_row_v1.sh .
```

Then deploy:

```bash
./tools/deploy_and_reboot.sh
```
