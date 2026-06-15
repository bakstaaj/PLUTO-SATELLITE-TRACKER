#!/usr/bin/env bash
# Package a validated Pluto Satellite Tracker SD-card installer release.
#
# Output ZIP name:
#   Pluto_Satellite_Tracker_Installer.zip
#
# Run from repo root:
#   ./tools/package_sd_card_installer_release_v2.sh
#
# Optional:
#   RELEASE_TAG=v0.1.0-sd-installer ./tools/package_sd_card_installer_release_v2.sh
#
# Output:
#   dist/releases/<release-name>/
#     Pluto_Satellite_Tracker_Installer.zip
#     RELEASE_MANIFEST.txt
#     SHA256SUMS.txt
#
# PACKAGE_SD_CARD_INSTALLER_RELEASE_V2

set -euo pipefail

ROOT="$(pwd)"
INSTALL_FOLDER_NAME="${SD_INSTALL_FOLDER_NAME:-PlutoSatelliteTrackerInstall}"
ZIP_NAME="${ZIP_NAME:-Pluto_Satellite_Tracker_Installer.zip}"
RELEASE_TAG="${RELEASE_TAG:-sd-installer-$(date -u +%Y%m%d_%H%M%S)}"
RELEASE_NAME="pluto-satellite-tracker-${RELEASE_TAG}"
RELEASE_DIR="$ROOT/dist/releases/$RELEASE_NAME"
INSTALL_DIR="$ROOT/dist/sd_card/$INSTALL_FOLDER_NAME"
ZIP_PATH="$RELEASE_DIR/$ZIP_NAME"
MANIFEST="$RELEASE_DIR/RELEASE_MANIFEST.txt"
SHA_FILE="$RELEASE_DIR/SHA256SUMS.txt"
INSTALL_README="$INSTALL_DIR/INSTALL_README.txt"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

need_file() {
  [[ -f "$1" ]] || fail "missing required file: $1"
}

need_exec() {
  [[ -x "$1" ]] || fail "missing or not executable: $1"
}

echo "Packaging Pluto Satellite Tracker SD-card installer release"
echo "Repo:        $ROOT"
echo "Release tag: $RELEASE_TAG"
echo "ZIP name:    $ZIP_NAME"
echo "Release dir: $RELEASE_DIR"
echo

need_exec "$ROOT/tools/build_sd_card_installer_folder_v2.sh"
need_exec "$ROOT/tools/validate_sd_card_installer_folder_v2.sh"
need_exec "$ROOT/tools/validate_pluto_arm_binaries_v3.sh"
need_exec "$ROOT/tools/validate_sd_install_runtime_hardening_v1.sh"

need_file "$ROOT/tools/install_from_sd_pluto_sat_tracker_v1.sh"
need_file "$ROOT/tools/autorun_pluto_sat_tracker_v1.sh"

echo "Step 1: validating ARM binaries"
"$ROOT/tools/validate_pluto_arm_binaries_v3.sh" "$ROOT"

echo
echo "Step 2: validating installer hardening"
"$ROOT/tools/validate_sd_install_runtime_hardening_v1.sh" "$ROOT"

echo
echo "Step 3: rebuilding SD-card installer folder"
"$ROOT/tools/build_sd_card_installer_folder_v2.sh"

echo
echo "Step 4: writing detailed installer README"
cat > "$INSTALL_README" <<'EOF'
Pluto Satellite Tracker SD Card Installer
========================================

This SD card installer installs the Pluto Satellite Tracker web application onto an
Analog Devices PlutoSDR / Pluto Plus running compatible firmware.

The installer is designed for a field install where the SD card is prepared on a
Windows PC, mailed or handed to the Pluto owner, inserted into the Pluto, and then
installed from a single SSH command sequence.

What this installer does
------------------------

The installer:

1. Copies the tracker application binaries to persistent Pluto flash storage:
   /mnt/jffs2/pluto_sat_tracker

2. Copies the web interface to:
   /mnt/jffs2/pluto_sat_tracker/web

3. Creates the SD-card runtime folder:
   /media/mmcblk0p1/pluto_sat_tracker

4. Creates runtime data/config/log folders:
   /media/mmcblk0p1/pluto_sat_tracker/data
   /media/mmcblk0p1/pluto_sat_tracker/config
   /media/mmcblk0p1/pluto_sat_tracker/logs
   /media/mmcblk0p1/pluto_sat_tracker/tmp

5. Installs the Pluto startup script:
   /mnt/jffs2/autorun.sh

6. Writes the tracker environment file:
   /mnt/jffs2/pluto_sat_tracker.env

7. Resets executable permissions for:
   - tracker backend binary
   - FM receiver helper binary
   - shell scripts
   - bundled Python runtime files

8. Verifies that the SD card is writable. If the SD card is read-only, the
   installer stops and tells you to repair the card.

9. Creates the receiver/observer configuration file if it is missing.

10. Clears stale pass-refresh lock files and stale refresh error state.

Important notes
---------------

- The installer does not format the SD card.
- The installer does not erase existing pass data unless the optional
  --reset-data flag is used.
- The web browser provides time synchronization to the Pluto. After install,
  open the web UI and refresh the pass list from the browser.

SD card layout
--------------

After extracting this ZIP to the root of the SD card, the SD card should contain:

  /PlutoSatelliteTrackerInstall/install_from_sd_pluto_sat_tracker_v1.sh
  /PlutoSatelliteTrackerInstall/autorun.sh
  /PlutoSatelliteTrackerInstall/bin/
  /PlutoSatelliteTrackerInstall/web/
  /PlutoSatelliteTrackerInstall/tools/
  /PlutoSatelliteTrackerInstall/INSTALL_README.txt

Do not copy only the shell script. Copy the entire PlutoSatelliteTrackerInstall
folder to the root of the SD card.

Pluto network assumptions
-------------------------

These instructions assume the Pluto is reachable at:

  192.168.2.1

Default SSH login is normally:

  user: root
  password: analog

If your firmware uses a different password, use that password instead.

Windows preparation steps
-------------------------

1. Insert the SD card into the Windows PC.

2. Extract Pluto_Satellite_Tracker_Installer.zip.

3. Copy the entire folder named PlutoSatelliteTrackerInstall to the root of the
   SD card.

   Example final SD card path:

     X:\PlutoSatelliteTrackerInstall\install_from_sd_pluto_sat_tracker_v1.sh

4. Safely eject the SD card.

Pluto install steps
-------------------

1. Power off the Pluto / Pluto Plus.

2. Insert the prepared SD card.

3. Power on the Pluto and wait for boot to complete.

4. SSH into the Pluto from MSYS2, Linux, macOS, or another SSH client.

5. Run these commands on the Pluto:

     mkdir -p /media/mmcblk0p1
     mount /dev/mmcblk0p1 /media/mmcblk0p1 2>/dev/null || true
     cd /media/mmcblk0p1/PlutoSatelliteTrackerInstall
     ./install_from_sd_pluto_sat_tracker_v1.sh --start

   If the mount command reports that the card is already mounted, that is OK.

6. Open the web UI in a browser:

     http://192.168.2.1:8080/SatelliteTracker/

7. Refresh the page once after it opens so the browser can synchronize time with
   the Pluto.

8. Use the UI to refresh catalog/TLE and generate quick passes.

If SSH host key changed
-----------------------

After reflashing Pluto firmware, your PC may show:

  WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED

From MSYS2 or Linux, run:

  ssh-keygen -R 192.168.2.1

Then SSH again and accept the new key.

If the SD card is read-only
---------------------------

If the installer reports that the SD card is read-only, remove the SD card and
repair it on Windows.

Open a Windows command prompt as Administrator and run:

  chkdsk X: /f

Replace X: with the SD card drive letter.

After repair, safely eject the SD card, reinsert it into the Pluto, and rerun
the install command.

Useful Pluto commands
---------------------

View startup log:

  cat /media/mmcblk0p1/pluto_sat_tracker/logs/autorun.log

View install log:

  cat /tmp/pluto_sat_tracker_sd_install.log

Restart application:

  /mnt/jffs2/autorun.sh

Stop application:

  killall pluto_sat_tracker 2>/dev/null || true

Verify app is listening:

  wget -qO- http://127.0.0.1:8080/api/passes | head

Expected result
---------------

After a successful install, the browser should load:

  http://192.168.2.1:8080/SatelliteTracker/

The UI should show the satellite map and pass list. On first load after boot,
the Pluto clock may be wrong until the browser synchronizes time. Refresh the UI
once, then refresh/generate passes.

Support notes
-------------

This package includes the ARM binaries and bundled runtime files needed by the
application. The Pluto firmware does not need to provide a system Python runtime
for pass generation when the bundled SD runtime is present.
EOF

echo
echo "Step 5: validating SD-card installer folder"
"$ROOT/tools/validate_sd_card_installer_folder_v2.sh" "$INSTALL_DIR"
need_file "$INSTALL_README"

rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

echo
echo "Step 6: creating ZIP package"
python3 - "$INSTALL_DIR" "$ZIP_PATH" "$INSTALL_FOLDER_NAME" <<'PY'
from __future__ import annotations

import sys
import zipfile
from pathlib import Path

install_dir = Path(sys.argv[1]).resolve()
zip_path = Path(sys.argv[2]).resolve()
folder_name = sys.argv[3]

if not install_dir.is_dir():
    raise SystemExit(f"missing installer dir: {install_dir}")

zip_path.parent.mkdir(parents=True, exist_ok=True)

skip_suffixes = {".pyc", ".tmp"}
skip_names = {"__pycache__"}

with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
    for path in sorted(install_dir.rglob("*")):
        rel = path.relative_to(install_dir)
        if any(part in skip_names for part in rel.parts):
            continue
        if path.is_file() and path.suffix in skip_suffixes:
            continue
        if path.is_dir():
            continue
        arcname = Path(folder_name) / rel
        info = zipfile.ZipInfo.from_file(path, str(arcname).replace("\\", "/"))
        mode = path.stat().st_mode
        info.external_attr = (mode & 0xFFFF) << 16
        with path.open("rb") as f:
            zf.writestr(info, f.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)

print(f"PASS: wrote {zip_path}")
PY

echo
echo "Step 7: writing manifest"
{
  echo "Pluto Satellite Tracker SD-card Installer Release"
  echo "================================================"
  echo
  echo "Release tag: $RELEASE_TAG"
  echo "Built UTC:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "Repo path:   $ROOT"
  echo
  echo "Package:"
  echo "  $ZIP_NAME"
  echo
  echo "Install folder inside ZIP:"
  echo "  $INSTALL_FOLDER_NAME/"
  echo
  echo "Detailed installer README inside ZIP:"
  echo "  $INSTALL_FOLDER_NAME/INSTALL_README.txt"
  echo
  echo "Pluto install command:"
  echo "  mkdir -p /media/mmcblk0p1"
  echo "  mount /dev/mmcblk0p1 /media/mmcblk0p1 2>/dev/null || true"
  echo "  cd /media/mmcblk0p1/$INSTALL_FOLDER_NAME"
  echo "  ./install_from_sd_pluto_sat_tracker_v1.sh --start"
  echo
  echo "Web UI:"
  echo "  http://192.168.2.1:8080/SatelliteTracker/"
  echo
  echo "Included runtime hardening:"
  echo "  - SD read/write validation"
  echo "  - executable permission reset"
  echo "  - observer.json creation/copy"
  echo "  - stale refresh lock/status cleanup"
  echo "  - clean backend start"
  echo
  echo "Installer folder contents:"
  (cd "$INSTALL_DIR" && find . -maxdepth 3 -type f | sort)
} > "$MANIFEST"

echo
echo "Step 8: writing checksums"
(
  cd "$RELEASE_DIR"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$ZIP_NAME" RELEASE_MANIFEST.txt > "$SHA_FILE"
  else
    python3 - "$ZIP_NAME" RELEASE_MANIFEST.txt > "$SHA_FILE" <<'PY'
import hashlib, sys
from pathlib import Path
for name in sys.argv[1:]:
    data = Path(name).read_bytes()
    print(hashlib.sha256(data).hexdigest(), name)
PY
  fi
)

echo
echo "PASS: release package created"
echo "  Release dir: $RELEASE_DIR"
echo "  ZIP:         $ZIP_PATH"
echo "  Manifest:    $MANIFEST"
echo "  Checksums:   $SHA_FILE"
