# SD-card Installer Release Packaging v2

Creates a distributable ZIP named:

```text
Pluto_Satellite_Tracker_Installer.zip
```

The ZIP contains:

```text
PlutoSatelliteTrackerInstall/
```

and includes a detailed installer README at:

```text
PlutoSatelliteTrackerInstall/INSTALL_README.txt
```

## Run

```bash
cd ~/sdrdev/PLUTO-SATELLITE-TRACKER

cp /c/Users/jim/Downloads/package_sd_card_installer_release_v2.sh tools/
cp /c/Users/jim/Downloads/package_sd_card_installer_release_v2_README.txt tools/

chmod +x tools/package_sd_card_installer_release_v2.sh

RELEASE_TAG=v0.1.0-sd-installer ./tools/package_sd_card_installer_release_v2.sh
```

## Output

```text
dist/releases/pluto-satellite-tracker-v0.1.0-sd-installer/
  Pluto_Satellite_Tracker_Installer.zip
  RELEASE_MANIFEST.txt
  SHA256SUMS.txt
```

Copy the contents of `Pluto_Satellite_Tracker_Installer.zip` to the SD card root. The SD card should contain:

```text
/PlutoSatelliteTrackerInstall/install_from_sd_pluto_sat_tracker_v1.sh
/PlutoSatelliteTrackerInstall/INSTALL_README.txt
```

The Pluto-side install command is:

```sh
mkdir -p /media/mmcblk0p1
mount /dev/mmcblk0p1 /media/mmcblk0p1 2>/dev/null || true
cd /media/mmcblk0p1/PlutoSatelliteTrackerInstall
./install_from_sd_pluto_sat_tracker_v1.sh --start
```
