# Pluto Satellite Tracker Handoff

## Project State

This repository now builds and deploys a Pluto-local satellite tracking application for amateur radio satellite passes. The runtime is hosted directly on the Pluto, serves a browser UI, stores persistent operational data on the SD card, and supports Pluto-side refresh of pass predictions and catalog data.

Primary runtime URL after deploy:

```text
http://192.168.3.1:8080/
```

Use `http://192.168.2.1:8080/` when connected over Pluto USB Ethernet.

## Completed Functional Implementation

### Backend application

- Native C Pluto application `pluto_sat_tracker`
- Built-in HTTP server for UI and API endpoints
- Runtime-served static web UI from `/mnt/jffs2/pluto_sat_tracker/web`
- Persistent observer config stored on Pluto JFFS2
- Persistent catalog, passes, logs, cache, and Python runtime stored on SD card

### Satellite and pass data management

- Local satellite catalog repository using public source data
- Public source workflow based on:
  - CelesTrak amateur TLE data
  - SatNOGS transmitter metadata
- Pluto-local refresh workflow for:
  - catalog/TLE refresh
  - pass regeneration
- Refresh status tracking with timestamps, summaries, and UI-visible state
- Background pass refresh loop on Pluto to keep `passes.json` current
- Startup pass regeneration from saved observer config when the app launches

### Observer location and time handling

- Observer location editor in the UI
- Saved location name, latitude, longitude, altitude, and minimum elevation
- Map click to prefill observer coordinates
- Observer save persists on-device
- Pluto time sync support surfaced in the UI
- Tracking guardrails that require synced time before tracked operation

### Mapping and visualization

- Real OpenStreetMap basemap in the web UI
- Ground track rendering for selected passes
- Sky plot / azimuth-elevation visualization
- Focused sample workflow shared between map and sky plot
- AOS, TCA, LOS, and focused/current point display
- Map view reset and live/focused sample controls

### Radio and tracking workflow

- Radio target planning for selected passes
- Manual receiver tuning workflow
- Doppler track plan generation and storage
- Manual step tracking
- Focused-sample tune and step actions
- Automatic Doppler tracking loop
- Track state persistence and status reporting
- Guardrails to reject stale passes
- Seconds-to-AOS / seconds-to-LOS status reporting
- LO write result recorded and shown in the UI

### Pluto hardware awareness

- Radio hardware/profile reporting in the UI
- Rev.B / AD9364-oriented tunability handling
- VHF/UHF tunability surfaced from backend capability checks
- Current runtime profile shown to help explain what is tunable

### Startup and persistence

- Pluto runtime launcher `run_tracker.sh`
- Persistent boot start via `/mnt/jffs2/autorun.sh`
- Installer script that places the autorun hook on JFFS2
- Runtime startup sequence that:
  - restores or establishes time
  - refreshes passes from saved config
  - starts background pass refresh
  - launches the app in network mode

### Analog audio work

- Analog FM audio control and browser playback path has been scaffolded end to end
- Dedicated helper binary `pluto_fm_receiver` added for Pluto-side live capture
- Live audio endpoints and UI controls are present
- This portion should still be treated as experimental compared with the core tracking workflow

## Final Runtime Layout

Small persistent runtime files on JFFS2:

```text
/mnt/jffs2/pluto_sat_tracker/
  pluto_sat_tracker
  pluto_fm_receiver
  run_tracker.sh
  autorun.sh
  web/index.html
  config/observer.json
```

Persistent SD-card application storage:

```text
/media/mmcblk0p1/pluto_sat_tracker/
  data/
    repositories.json
    satellites.json
    passes.json
    refresh_status.json
    radio_track.json
    radio_track_state.json
  tools/
    pluto_refresh_data.sh
    pluto_pass_refresh_loop.sh
    update_satellite_catalog.py
    update_pass_predictions.py
    write_refresh_status.py
  python/
    sgp4/
  python-runtime/
  cache/
  logs/
```

## Development Environment

Host environment:

- Windows
- MSYS2 UCRT64 shell for day-to-day work
- Docker Desktop in Linux container mode

Recommended MSYS2 packages:

```bash
pacman -S --needed git make python sshpass
```

Build environment:

- Cross-build Docker image from `docker/Dockerfile.cross`
- Default image tag: `pluto-adsb-tracker-cross:v0.39`
- Cross compiler: `arm-linux-gnueabihf-gcc`
- Pluto sysroot staged in the Docker image

Pluto connectivity assumptions:

- Pluto reachable over Ethernet fallback or USB Ethernet
- Default deploy IP in scripts: `192.168.3.1`
- Alternative USB Ethernet UI address: `192.168.2.1`
- SD card installed and mounted at `/media/mmcblk0p1`

Local environment file:

```text
.pluto.env
```

Expected variables include:

```text
PLUTO_IP=192.168.3.1
PLUTO_USER=root
PLUTO_PASS=...
PLUTO_DEPLOY_DIR=/mnt/jffs2/pluto_sat_tracker
PLUTO_SD_ROOT=/media/mmcblk0p1/pluto_sat_tracker
```

Python runtime approach:

- Python-dependent refresh utilities are intended to run from the SD card
- Optional runtime tarball path:

```text
runtime/python-pluto-armhf.tar.gz
```

- Deploy extracts that runtime to:

```text
/media/mmcblk0p1/pluto_sat_tracker/python-runtime/
```

## Build and Deploy Scripts

### Build

Cross-build the Pluto binaries:

```bash
./tools/build_pluto_v0_39.sh
```

Clean rebuild:

```bash
./tools/build_pluto_v0_39.sh --clean
```

Rebuild the Docker image first:

```bash
./tools/build_pluto_v0_39.sh --rebuild-image
```

Build only the cross image:

```bash
./tools/build_cross_image.sh
```

### Deploy

Full runtime deploy to Pluto:

```bash
./tools/deploy_to_pluto.sh
```

This deploys:

- `dist/pluto_sat_tracker`
- `dist/pluto_fm_receiver`
- runtime launcher
- web UI
- default observer config when missing
- SD-card tools and Python support files
- optional staged Python runtime tarball

Web-only deploy:

```bash
./tools/deploy_web_only.sh
```

### Run and restart

Run interactively:

```bash
./tools/run_on_pluto.sh
```

Stop the deployed services:

```bash
./tools/stop_on_pluto.sh
```

Restart the deployed runtime and tail recent log output:

```bash
./tools/codex_restart_pluto.sh
```

### Startup persistence

Install boot startup:

```bash
./tools/install_autostart.sh
```

This writes a persistent `/mnt/jffs2/autorun.sh` that launches the app on reboot.

## Operational Notes

- The core workflow is now centered on:
  1. open UI
  2. save observer location
  3. sync time
  4. refresh catalog and passes as needed
  5. select a pass
  6. tune or track the pass
- The UI now includes a simplified main view with setup/help/system items moved into the menu structure.
- Map-based pass review and focused-sample actions are part of the normal operator workflow.
- Background pass refresh is intended to keep the pass list populated after startup and through normal operation.

## Key Repo Files

- `src/pluto_sat_tracker.c` - main Pluto application and HTTP API
- `src/pluto_fm_receiver.c` - dedicated Pluto FM helper
- `web/index.html` - operator UI
- `tools/build_pluto_v0_39.sh` - cross-build wrapper
- `tools/deploy_to_pluto.sh` - full Pluto deploy
- `tools/pluto_runtime.sh` - runtime launcher on Pluto
- `tools/pluto_refresh_data.sh` - Pluto-local refresh entry point
- `tools/pluto_pass_refresh_loop.sh` - background pass refresh loop
- `tools/install_autostart.sh` - JFFS2 autorun installer
- `docs/USER_WORKFLOW.md` - operator usage guide
- `docs/DEVELOPMENT.md` - developer workflow

## Current Deliverable

The repository now contains a working Pluto-local satellite tracker with:

- persistent on-device configuration
- Pluto-local satellite catalog and pass management
- map and pass visualization
- manual and automatic Doppler tuning workflows
- persistent startup behavior on reboot
- reusable cross-build and deploy tooling for continued development
