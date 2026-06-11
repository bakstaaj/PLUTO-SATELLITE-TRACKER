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

## Current Scope

The first repo milestone provides:

- A buildable Pluto-local C web server.
- JSON status/config/catalog endpoints.
- A simple browser UI shell.
- SD-card data repository layout.
- Cross-build, deploy, web-only deploy, run, and stop scripts.

Pass prediction, TLE refresh, satellite filtering, Doppler planning, and Pluto radio control are the next implementation layers.
