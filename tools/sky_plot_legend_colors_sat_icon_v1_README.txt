# Sky Plot Legend Colors and Satellite Icon v1

## Changes

- Applies the same legend colors to the azimuth / sky plot:
  - Path = orange
  - Progress = cyan
  - Look line = blue-gray
  - Focus = dark
  - Satellite = cyan
- Replaces the current live satellite dot on the azimuth / sky plot with a small satellite icon.
- Shortens legacy legend wording if present.

## Run

```bash
python tools/apply_sky_plot_legend_colors_sat_icon_v1.py .
./tools/validate_sky_plot_legend_colors_sat_icon_v1.sh .
./tools/deploy_and_reboot.sh
```

After reboot:

```bash
source .pluto.env
python tools/test_sky_plot_legend_colors_sat_icon_v1.py
```
