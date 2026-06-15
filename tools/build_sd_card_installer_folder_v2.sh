#!/usr/bin/env bash
# Build a self-contained SD-card installer folder for Pluto Satellite Tracker v2.
#
# v2 fixes the Windows/MSYS executable-bit issue:
#   - ARM binaries are accepted if they are regular files and `file` reports ARM ELF.
#   - They do not need to be executable on the Windows/MSYS host.
#   - The installer/stage still chmods them executable for Pluto.
#
# Creates:
#   dist/sd_card/PlutoSatelliteTrackerInstall/
#
# Optional:
#   ./tools/build_sd_card_installer_folder_v2.sh --sd-root /x

set -euo pipefail

ROOT="$(pwd)"
FOLDER_NAME="${SD_INSTALL_FOLDER_NAME:-PlutoSatelliteTrackerInstall}"
OUT_DIR="$ROOT/dist/sd_card/$FOLDER_NAME"
COPY_TO_SD_ROOT=""
ALLOW_SOURCE_ONLY=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --sd-root)
      COPY_TO_SD_ROOT="${2:-}"
      [[ -n "$COPY_TO_SD_ROOT" ]] || { echo "ERROR: --sd-root needs a path" >&2; exit 2; }
      shift 2
      ;;
    --output)
      OUT_DIR="${2:-}"
      [[ -n "$OUT_DIR" ]] || { echo "ERROR: --output needs a path" >&2; exit 2; }
      shift 2
      ;;
    --allow-source-only)
      ALLOW_SOURCE_ONLY=1
      shift
      ;;
    *)
      echo "ERROR: unknown option: $1" >&2
      exit 2
      ;;
  esac
done

fail() { echo "FAIL: $*" >&2; exit 1; }
need_file() { [[ -f "$1" ]] || fail "missing required file: $1"; }

is_arm_elf() {
  local path="$1"
  [[ -f "$path" ]] || return 1
  if command -v file >/dev/null 2>&1; then
    file "$path" | grep -Eiq 'ELF .*ARM'
  else
    # No file command: fall back to accepting regular file.
    return 0
  fi
}

find_arm_binary() {
  local name="$1"
  local p
  for p in "$ROOT/build/$name" "$ROOT/dist/$name" "$ROOT/bin/$name" "$ROOT/out/$name" "$ROOT/$name"; do
    if is_arm_elf "$p"; then
      echo "$p"
      return 0
    fi
  done
  while IFS= read -r p; do
    if is_arm_elf "$p"; then
      echo "$p"
      return 0
    fi
  done < <(find "$ROOT" \
    -path "$ROOT/.git" -prune -o \
    -path "$ROOT/.venv" -prune -o \
    -path "$ROOT/dist/sd_card" -prune -o \
    -type f -name "$name" -print 2>/dev/null)
  return 1
}

try_build_existing() {
  for s in "$ROOT/tools/build_pluto_binaries.sh" "$ROOT/tools/build_pluto_app.sh" "$ROOT/tools/build.sh" "$ROOT/tools/build_pluto_v0_39.sh"; do
    if [[ -x "$s" ]]; then
      echo "INFO: running existing build helper $s"
      "$s" || true
    fi
  done
}

copy_dir_if_exists() {
  local src="$1"
  local dst="$2"
  if [[ -d "$src" ]]; then
    mkdir -p "$dst"
    cp -a "$src"/. "$dst"/
  fi
}

echo "Building self-contained SD-card installer folder v2"
echo "Repo: $ROOT"
echo "Output folder: $OUT_DIR"

need_file "$ROOT/src/pluto_sat_tracker.c"
need_file "$ROOT/src/pluto_fm_receiver.c"
need_file "$ROOT/web/index.html"
need_file "$ROOT/tools/install_from_sd_pluto_sat_tracker_v1.sh"
need_file "$ROOT/tools/autorun_pluto_sat_tracker_v1.sh"

# Try known-good local build helper if present, but do not require it if recovered ARM binaries already exist.
try_build_existing

APP_BIN="$(find_arm_binary pluto_sat_tracker || true)"
FM_BIN="$(find_arm_binary pluto_fm_receiver || true)"

if [[ -z "$APP_BIN" || -z "$FM_BIN" ]]; then
  if [[ "$ALLOW_SOURCE_ONLY" -ne 1 ]]; then
    echo "ERROR: required ARM ELF binaries were not found." >&2
    [[ -n "$APP_BIN" ]] || echo "  missing ARM ELF: pluto_sat_tracker" >&2
    [[ -n "$FM_BIN" ]] || echo "  missing ARM ELF: pluto_fm_receiver" >&2
    echo "Run: python tools/recover_existing_pluto_arm_binaries_v1.py ." >&2
    exit 1
  fi
  echo "WARN: source-only folder requested; recipient Pluto cannot start unless binaries are later added."
fi

rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"/{bin,src,web,tools,data,config,logs}

cat > "$OUT_DIR/README_INSTALL_ON_PLUTO.txt" <<'EOF'
Pluto Satellite Tracker SD-card installer

From the Pluto SSH shell:

  mkdir -p /media/mmcblk0p1
  mount /dev/mmcblk0p1 /media/mmcblk0p1 2>/dev/null || true
  cd /media/mmcblk0p1/PlutoSatelliteTrackerInstall
  ./install_from_sd_pluto_sat_tracker_v1.sh --start

If the SD card is already mounted, the mount command may print an error. That is OK.

After install, open:

  http://192.168.2.1:8080/SatelliteTracker/

The installer is non-destructive. It creates runtime folders on the SD card and installs /mnt/jffs2/autorun.sh.
EOF

printf "SD_CARD_INSTALLER_FOLDER_V2\n" > "$OUT_DIR/INSTALLER_VERSION.txt"
printf "%s\n" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" > "$OUT_DIR/BUILT_UTC.txt"

cp -a "$ROOT/src"/. "$OUT_DIR/src"/
cp -a "$ROOT/web"/. "$OUT_DIR/web"/
cp -a "$ROOT/tools"/. "$OUT_DIR/tools"/
copy_dir_if_exists "$ROOT/config" "$OUT_DIR/config"
copy_dir_if_exists "$ROOT/data" "$OUT_DIR/data"

if [[ -n "$APP_BIN" ]]; then
  cp "$APP_BIN" "$OUT_DIR/bin/pluto_sat_tracker"
  chmod +x "$OUT_DIR/bin/pluto_sat_tracker" 2>/dev/null || true
fi
if [[ -n "$FM_BIN" ]]; then
  cp "$FM_BIN" "$OUT_DIR/bin/pluto_fm_receiver"
  chmod +x "$OUT_DIR/bin/pluto_fm_receiver" 2>/dev/null || true
fi

cp "$ROOT/tools/install_from_sd_pluto_sat_tracker_v1.sh" "$OUT_DIR/install_from_sd_pluto_sat_tracker_v1.sh"
cp "$ROOT/tools/autorun_pluto_sat_tracker_v1.sh" "$OUT_DIR/autorun.sh"
chmod +x "$OUT_DIR/install_from_sd_pluto_sat_tracker_v1.sh" "$OUT_DIR/autorun.sh" 2>/dev/null || true

find "$OUT_DIR" -type f \( \
  -name '*.bak-*' -o \
  -name '*.tmp' -o \
  -name '*_patch_context*.txt' -o \
  -name 'pluto_dual_rx_capability_report*.txt' -o \
  -name '*.raw' \
\) -delete

if [[ -n "$COPY_TO_SD_ROOT" ]]; then
  DEST="$COPY_TO_SD_ROOT/$FOLDER_NAME"
  echo "Copying installer folder to SD root: $DEST"
  rm -rf "$DEST"
  mkdir -p "$COPY_TO_SD_ROOT"
  cp -a "$OUT_DIR" "$DEST"
  echo "PASS: copied to $DEST"
fi

echo
echo "PASS: SD-card installer folder created:"
echo "  $OUT_DIR"
echo
if command -v file >/dev/null 2>&1; then
  file "$OUT_DIR/bin/pluto_sat_tracker" "$OUT_DIR/bin/pluto_fm_receiver" || true
fi
echo
echo "Validate:"
echo "  ./tools/validate_sd_card_installer_folder_v2.sh '$OUT_DIR'"
