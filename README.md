# Pluto Satellite Tracker

Local Pluto Plus application for satellite-based amateur radio beacon and repeater tracking.

This repo starts from the proven Pluto firmware and ADS-B Tracker workflow:

- Pluto Plus firmware baseline: ADI PlutoSDR firmware v0.39 with SD card and Ethernet fallback support.
- Development host: Windows + MSYS2 UCRT64 + Docker Desktop.
- Cross-build environment: Pluto v0.39 ARM sysroot image, reused from the ADS-B Tracker project by default.
- Runtime model: a Pluto-local HTTP server with runtime-served web assets.
- Persistent storage: small app/config files in `/mnt/jffs2`, larger satellite catalogs and generated pass data on the Micro SD card.

## Initial Workflow

From MSYS2 UCRT64:

```bash
cp .pluto.env.example .pluto.env
# edit .pluto.env and set PLUTO_PASS

./tools/build_pluto_v0_39.sh
./tools/deploy_to_pluto.sh
./tools/run_on_pluto.sh
```

Then open:

```text
http://192.168.3.1:8080/SatelliteTracker/
```

Use `192.168.2.1` instead if you are connected over Pluto USB Ethernet.

## Enable Startup on Pluto Boot

After deploying the runtime, install the boot-start hook:

```bash
./tools/install_autostart.sh
```

The installer writes `/mnt/jffs2/autorun.sh`, which persists on the Pluto JFFS2
partition and launches `/mnt/jffs2/pluto_sat_tracker/run_tracker.sh -- --net`.
If an unrelated autorun script already exists, it is backed up as
`/mnt/jffs2/autorun.sh.before-pluto-sat-tracker`.

## Current Scope

The first repo milestone provides:

- A buildable Pluto-local C web server.
- JSON status/config/catalog endpoints.
- A simple browser UI shell.
- SD-card data repository layout.
- Cross-build, deploy, web-only deploy, run, and stop scripts.

## Operator Guide

For a practical UI walkthrough, see [docs/USER_WORKFLOW.md](docs/USER_WORKFLOW.md).

## Rotator Control Quick Start

<!-- ROTATOR_WORKFLOW_DOCS_V2_4_7 -->

The rotator workflow is designed to be safe by default. Use this order when
testing a new antenna rotator or switching between active satellite passes:

1. Open the web UI and confirm `Backend online`, `Time synced`, and a current pass list.
2. Select the pass you want to follow from `Next Passes`. If multiple passes are active, the rotator follows the selected pass, not the first active pass in the list.
3. In `Rotator Control`, confirm the live target azimuth/elevation changes and check the `Rotator source` label:
   - `selected pass` means the selected pass Doppler plan is driving the target.
   - `radio state` means the Doppler tracking state file is driving the target.
   - `pass-list fallback` means the backend is using the nearest current point from `passes.json`.
   - `manual/test` means a manual test move or simulation command last updated the state.
4. Choose the rotator `Type`:
   - `Simulation` is safe for UI and workflow testing.
   - `Hamlib rotctld TCP` sends `P <az> <el>` commands to a Hamlib rotator daemon.
   - `EasyComm II` sends `AZ... EL...` style commands.
   - `Yaesu GS-232` sends `Waaa eee` style commands.
   - `SATRAN MK2/MK3` remains intentionally pending until the exact firmware protocol is confirmed.
5. Use `Preview Command` before moving hardware. This shows the exact protocol command and does not move the rotator.
6. Use `Test Move` only after verifying type, host, port, offsets, elevation limits, and command preview.
7. Use `Start Rotator Tracking` to let Pluto periodically send the selected pass target.
8. Use `Stop Tracking`, `Stop`, or `Park` before changing hardware, cabling, offsets, or rotator type.

## Update Satellite Catalog

From MSYS2 UCRT64 or another shell with Python 3:

```bash
python3 tools/update_satellite_catalog.py
python3 tools/update_pass_predictions.py
./tools/deploy_web_only.sh
```

The catalog updater builds `data/satellites.json` from CelesTrak amateur TLE data and matching SatNOGS transmitter metadata. The pass updater builds `data/passes.json` from the local catalog and observer configuration.

The deployed Pluto runtime also includes a local refresh runner on the SD card:

```text
/media/mmcblk0p1/pluto_sat_tracker/tools/pluto_refresh_data.sh passes
/media/mmcblk0p1/pluto_sat_tracker/tools/pluto_refresh_data.sh catalog
```

The web UI calls the same workflow through `/api/refresh/passes` and
`/api/refresh/catalog`, with status stored in `refresh_status.json`. Successful
refreshes now include a summary payload so the UI can show pass counts, catalog
counts, and the latest CelesTrak/SatNOGS source timestamps.

For on-device refresh without changing firmware, stage an ARM hard-float Python
runtime at:

```text
runtime/python-pluto-armhf.tar.gz
```

The archive contents must be rooted at `bin/python3`, `lib/`, and any other files
needed by that interpreter. During deploy it is extracted to:

```text
/media/mmcblk0p1/pluto_sat_tracker/python-runtime/
```

The refresh runner prefers
`/media/mmcblk0p1/pluto_sat_tracker/python-runtime/bin/python3`, then falls back
to system `python3` if present. If neither exists, the endpoint records a clear
runtime-missing status.

Pass filtering and richer radio operations are the next implementation layers.
