# SD Install Runtime Hardening v1

Adds the field-tested recovery steps directly to the SD installer and autorun scripts.

## Fixes added

- Validate SD card is mounted read-write before install/start.
- Fail with a clear `chkdsk X: /f` instruction if SD is read-only.
- Reset execute permissions on:
  - app binaries
  - shell scripts
  - bundled Python runtime binaries
- Create `observer.json` in both:
  - SD runtime config
  - `/mnt/jffs2` deploy config
- Clear stale pass refresh locks and stale `refresh_status.json` errors during install.
- Stop stale tracker/refresh processes before starting backend.
- Leave refresh itself to the browser time-sync flow, which is now confirmed working.

## Apply

```bash
cd ~/sdrdev/PLUTO-SATELLITE-TRACKER

cp /c/Users/jim/Downloads/install_from_sd_pluto_sat_tracker_v1_hardened.sh tools/install_from_sd_pluto_sat_tracker_v1.sh
cp /c/Users/jim/Downloads/autorun_pluto_sat_tracker_v1_hardened.sh tools/autorun_pluto_sat_tracker_v1.sh
cp /c/Users/jim/Downloads/validate_sd_install_runtime_hardening_v1.sh tools/
cp /c/Users/jim/Downloads/sd_install_runtime_hardening_v1_README.txt tools/

chmod +x tools/install_from_sd_pluto_sat_tracker_v1.sh
chmod +x tools/autorun_pluto_sat_tracker_v1.sh
chmod +x tools/validate_sd_install_runtime_hardening_v1.sh

./tools/validate_sd_install_runtime_hardening_v1.sh .

./tools/build_sd_card_installer_folder_v2.sh
./tools/validate_sd_card_installer_folder_v2.sh dist/sd_card/PlutoSatelliteTrackerInstall
```
