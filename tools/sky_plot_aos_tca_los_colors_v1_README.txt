# Sky Plot AOS/TCA/LOS Colors v1

## Changes

Applies the same legend colors to the AOS/TCA/LOS markers on the azimuth / sky plot:

- AOS = green `#1a7f37`
- TCA = orange/brown `#a15c00`
- LOS = purple `#7d3ad3`

The helper also handles the case where the exact TCA timestamp is not one of the high-resolution sample points by coloring the sample nearest TCA.

## Run

```bash
python tools/apply_sky_plot_aos_tca_los_colors_v1.py .
./tools/validate_sky_plot_aos_tca_los_colors_v1.sh .
./tools/deploy_and_reboot.sh
```

After reboot:

```bash
source .pluto.env
python tools/test_sky_plot_aos_tca_los_colors_v1.py
```
