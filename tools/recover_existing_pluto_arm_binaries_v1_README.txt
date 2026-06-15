# Recover Existing Pluto ARM Binaries v1

This script avoids new Docker experiments. It searches your local `~/sdrdev` tree for previously built ARM ELF binaries:

```text
pluto_sat_tracker
pluto_fm_receiver
```

If it finds them, it copies them to:

```text
build/pluto_sat_tracker
build/pluto_fm_receiver
```

It also prints likely build scripts and Dockerfiles so we can reuse the exact old build tooling.

## Run

```bash
cd ~/sdrdev/PLUTO-SATELLITE-TRACKER

cp /c/Users/jim/Downloads/recover_existing_pluto_arm_binaries_v1.py tools/
cp /c/Users/jim/Downloads/recover_existing_pluto_arm_binaries_v1_README.txt tools/

chmod +x tools/recover_existing_pluto_arm_binaries_v1.py

python tools/recover_existing_pluto_arm_binaries_v1.py .
```

Then:

```bash
./tools/validate_pluto_arm_binaries_v2.sh .
./tools/build_sd_card_installer_folder_v1.sh
./tools/validate_sd_card_installer_folder_v1.sh dist/sd_card/PlutoSatelliteTrackerInstall
```
