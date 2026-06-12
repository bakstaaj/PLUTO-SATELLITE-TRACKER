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
`/api/refresh/catalog`, with status stored in `refresh_status.json`. The current
Pluto image must provide `python3` for on-device pass/catalog regeneration; if it
does not, the endpoint records a clear `python3 is not installed` status.

Pass filtering and richer radio operations are the next implementation layers.
