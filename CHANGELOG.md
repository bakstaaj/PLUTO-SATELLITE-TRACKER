# Changelog

## v2.3.0 - 2026-06-16

### Added

- Doppler-aware satellite Listen support.
- Audio-owned Doppler tracking for live satellite audio.
- Active `radio_track_state.json` fallback for Listen starts when the regular auto tracker is already active.
- `doppler_state_fallback` API response field.

### Changed

- Backend Doppler LO writes now use the known-good `iio_attr` path first.
- Pluto backend deploy workflow now uses a real forced reboot instead of manual backend process restart.
- Satellite Listen can now start from the current active Doppler `rx_hz`.

### Verified

- Regular auto-Doppler tracking writes LO through `iio_attr`.
- Explicit Doppler Listen starts successfully with `doppler_track=true`.
- State fallback Listen starts successfully with `doppler_state_fallback=true`.
- NOAA Weather Radio diagnostic audio remains fixed-frequency and does not use Doppler.
- `pluto_fm_receiver` helper was preserved during backend-only deployment.

