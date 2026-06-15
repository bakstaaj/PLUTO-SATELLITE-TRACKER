#!/usr/bin/env bash
# Validate a self-contained SD-card installer folder v2.
#
# v2 requires ARM ELF binary files, but does not fail on missing Windows/MSYS executable bits.

set -euo pipefail

FOLDER="${1:-}"
if [[ -z "$FOLDER" || ! -d "$FOLDER" ]]; then
  echo "FAIL: pass installer folder path"
  exit 1
fi

failed=0

check_file() {
  local label="$1"; local path="$2"
  if [[ -f "$path" ]]; then echo "PASS: $label"; else echo "FAIL: $label"; failed=1; fi
}

check_dir() {
  local label="$1"; local path="$2"
  if [[ -d "$path" ]]; then echo "PASS: $label"; else echo "FAIL: $label"; failed=1; fi
}

check_arm_elf() {
  local label="$1"; local path="$2"
  check_file "$label exists" "$path"
  [[ -f "$path" ]] || return
  if command -v file >/dev/null 2>&1; then
    local desc
    desc="$(file "$path" 2>/dev/null || true)"
    echo "INFO: $desc"
    if echo "$desc" | grep -Eiq 'ELF .*ARM'; then
      echo "PASS: $label is ARM ELF"
    else
      echo "FAIL: $label is not ARM ELF"
      failed=1
    fi
  else
    echo "WARN: file command not available; skipped ARM ELF check for $label"
  fi
  if [[ -x "$path" ]]; then
    echo "PASS: $label host executable bit set"
  else
    echo "WARN: $label host executable bit not set; installer chmods on Pluto"
  fi
}

check_file "installer version" "$FOLDER/INSTALLER_VERSION.txt"
check_file "SD installer" "$FOLDER/install_from_sd_pluto_sat_tracker_v1.sh"
check_file "autorun" "$FOLDER/autorun.sh"
check_dir "bin dir" "$FOLDER/bin"
check_arm_elf "tracker binary" "$FOLDER/bin/pluto_sat_tracker"
check_arm_elf "FM receiver binary" "$FOLDER/bin/pluto_fm_receiver"
check_file "web index" "$FOLDER/web/index.html"
check_file "tracker source" "$FOLDER/src/pluto_sat_tracker.c"
check_file "FM receiver source" "$FOLDER/src/pluto_fm_receiver.c"
check_dir "tools dir" "$FOLDER/tools"
check_dir "data dir" "$FOLDER/data"
check_file "Pluto owner README" "$FOLDER/README_INSTALL_ON_PLUTO.txt"

if [[ "$failed" -ne 0 ]]; then
  echo "Validation failed."
  exit 1
fi

echo "Validation passed."
