# Development Environment

## Host

Use Windows with MSYS2 UCRT64 and Docker Desktop in Linux-container mode.

The scripts assume these tools are available in the MSYS2 shell:

```bash
pacman -S --needed git make python sshpass
```

Docker is provided by Docker Desktop for Windows.

## Pluto Access

Create a local `.pluto.env` from `.pluto.env.example`.

Default targets:

```bash
PLUTO_IP=192.168.3.1
PLUTO_USER=root
PLUTO_DEPLOY_DIR=/mnt/jffs2/pluto_sat_tracker
PLUTO_SD_ROOT=/media/mmcblk0p1/pluto_sat_tracker
```

The firmware handoff confirms the Pluto Plus Ethernet fallback address is `192.168.3.1` and the SD card is mounted at `/media/mmcblk0p1`.

## Build

The build script reuses `PLUTO_CROSS_IMAGE` when it exists. The included Dockerfile builds a Pluto v0.39 sysroot image with Debian's maintained `arm-linux-gnueabihf` cross compiler:

```bash
./tools/build_pluto_v0_39.sh
```

Build the image only when missing or when `docker/Dockerfile.cross` changes:

```bash
./tools/build_cross_image.sh
```

Clean rebuild:

```bash
./tools/build_pluto_v0_39.sh --clean
```

## Deploy

Full deploy:

```bash
./tools/deploy_to_pluto.sh
```

Web/data-only deploy:

```bash
./tools/deploy_web_only.sh
```

Run interactively over SSH:

```bash
./tools/run_on_pluto.sh
```

Stop the Pluto process:

```bash
./tools/stop_on_pluto.sh
```
