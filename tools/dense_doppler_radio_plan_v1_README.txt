# Dense Doppler Radio Plan v1

Yes: the radio track should use a finer Doppler plan, not only a smoother map display.

## What this patch does

Before the browser posts `/api/radio/track/plan`, it converts the existing pass `doppler_plan.points` into a 5-second dense plan.

It interpolates:

- `rx_hz`
- `tx_hz`
- `rx_offset_hz`
- `tx_offset_hz`
- `range_rate_m_s`
- `range_km`
- `azimuth_deg`
- `elevation_deg`

The backend still owns actual LO writes. The UI only gives the backend a finer plan.

## Why this is the right next step

A display-only smooth line can hide that the receiver LO is still stepping coarsely. This patch makes the radio tracking plan finer without needing a new backend endpoint yet.

## Run

```bash
python tools/apply_dense_doppler_radio_plan_v1.py .
./tools/validate_dense_doppler_radio_plan_v1.sh .
./tools/deploy_and_reboot.sh
```

After reboot:

```bash
source .pluto.env
python tools/test_dense_doppler_radio_plan_v1.py
```
