#!/usr/bin/env bash
# Package a validated Pluto Satellite Tracker SD-card installer release.
#
# Run from repo root:
#   ./tools/package_sd_card_installer_release_v1.sh
#
# Optional:
#   RELEASE_TAG=v0.1.0-sd-installer ./tools/package_sd_card_installer_release_v1.sh
#
# Output:
#   dist/releases/<release-name>/
#     PlutoSatelliteTrackerInstall.zip
#     SHA256SUMS.txt
#     RELEASE_MANIFEST.txt
#
# PACKAGE_SD_CARD_INSTALLER_RELEASE_V1

set -euo pipefail

ROOT="$(pwd)"
INSTALL_FOLDER_NAME="${SD_INSTALL_FOLDER_NAME:-PlutoSatelliteTrackerInstall}"
RELEASE_TAG="${RELEASE_TAG:-sd-installer-$(date -u +%Y%m%d_%H%M%S)}"
RELEASE_NAME="pluto-satellite-tracker-${RELEASE_TAG}"
RELEASE_DIR="$ROOT/dist/releases/$RELEASE_NAME"
INSTALL_DIR="$ROOT/dist/sd_card/$INSTALL_FOLDER_NAME"
ZIP_PATH="$RELEASE_DIR/${INSTALL_FOLDER_NAME}.zip"
MANIFEST="$RELEASE_DIR/RELEASE_MANIFEST.txt"
SHA_FILE="$RELEASE_DIR/SHA256SUMS.txt"

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
echo "Step 4: validating SD-card installer folder"
"$ROOT/tools/validate_sd_card_installer_folder_v2.sh" "$INSTALL_DIR"

rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

echo
echo "Step 5: creating ZIP package"
python3 - "$INSTALL_DIR" "$ZIP_PATH" "$INSTALL_FOLDER_NAME" <<'PY'
from __future__ import annotations

import os
import sys
import zipfile
from pathlib import Path

install_dir = Path(sys.argv[1]).resolve()
zip_path = Path(sys.argv[2]).resolve()
folder_name = sys.argv[3]

if not install_dir.is_dir():
    raise SystemExit(f"missing installer dir: {install_dir}")

zip_path.parent.mkdir(parents=True, exist_ok=True)

skip_suffixes = {
    ".pyc",
    ".tmp",
}
skip_names = {
    "__pycache__",
}

with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
    for path in sorted(install_dir.rglob("*")):
        rel = path.relative_to(install_dir)
        if any(part in skip_names for part in rel.parts):
            continue
        if path.is_file() and path.suffix in skip_suffixes:
            continue
        arcname = Path(folder_name) / rel
        if path.is_dir():
            # Directory records are not necessary; files carry paths.
            continue
        info = zipfile.ZipInfo.from_file(path, str(arcname).replace("\\", "/"))
        # Preserve executable bits for Unix-ish extractors where possible.
        mode = path.stat().st_mode
        info.external_attr = (mode & 0xFFFF) << 16
        with path.open("rb") as f:
            zf.writestr(info, f.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)

print(f"PASS: wrote {zip_path}")
PY

echo
echo "Step 6: writing manifest"
{
  echo "Pluto Satellite Tracker SD-card Installer Release"
  echo "================================================"
  echo
  echo "Release tag: $RELEASE_TAG"
  echo "Built UTC:   $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "Repo path:   $ROOT"
  echo
  echo "Package:"
  echo "  $(basename "$ZIP_PATH")"
  echo
  echo "Install folder inside ZIP:"
  echo "  $INSTALL_FOLDER_NAME/"
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
echo "Step 7: writing checksums"
(
  cd "$RELEASE_DIR"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$(basename "$ZIP_PATH")" RELEASE_MANIFEST.txt > "$SHA_FILE"
  else
    python3 - "$(basename "$ZIP_PATH")" RELEASE_MANIFEST.txt > "$SHA_FILE" <<'PY'
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
