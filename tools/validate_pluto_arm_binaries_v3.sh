#!/usr/bin/env bash
# Validate built Pluto ARM binaries v3.
#
# v3 treats ARM ELF as the hard requirement and executable bit as a warning,
# because Windows/MSYS files can be valid ARM executables while [[ -x ]] reports false.

set -euo pipefail

ROOT="${1:-.}"
APP="$ROOT/build/pluto_sat_tracker"
FM="$ROOT/build/pluto_fm_receiver"

failed=0

check_file() {
  local label="$1"
  local path="$2"
  if [[ -f "$path" ]]; then
    echo "PASS: $label exists: $path"
  else
    echo "FAIL: $label missing: $path"
    failed=1
  fi
}

check_arm_elf() {
  local label="$1"
  local path="$2"
  if ! [[ -f "$path" ]]; then
    return
  fi
  if command -v file >/dev/null 2>&1; then
    local desc
    desc="$(file "$path" 2>/dev/null || true)"
    echo "INFO: $desc"
    if echo "$desc" | grep -Eiq 'ELF .*ARM'; then
      echo "PASS: $label is ARM ELF"
    else
      echo "FAIL: $label is not reported as ARM ELF"
      failed=1
    fi
    if echo "$desc" | grep -Eiq 'statically linked'; then
      echo "PASS: $label is statically linked"
    else
      echo "WARN: $label is dynamically linked"
    fi
  else
    echo "WARN: file command not available; skipped architecture check for $label"
  fi

  if [[ -x "$path" ]]; then
    echo "PASS: $label executable bit is set on host"
  else
    echo "WARN: $label executable bit is not set on host; installer will chmod on Pluto"
  fi
}

check_file "pluto_sat_tracker" "$APP"
check_file "pluto_fm_receiver" "$FM"
check_arm_elf "pluto_sat_tracker" "$APP"
check_arm_elf "pluto_fm_receiver" "$FM"

if [[ "$failed" -ne 0 ]]; then
  echo "Validation failed."
  exit 1
fi

echo "Validation passed."
