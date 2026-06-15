#!/usr/bin/env bash
# Validate SD install runtime hardening is present in installer/autorun scripts.

set -euo pipefail

ROOT="${1:-.}"
failed=0

check() {
  local file="$1"
  local pattern="$2"
  local label="$3"
  if grep -q "$pattern" "$ROOT/$file"; then
    echo "PASS: $label"
  else
    echo "FAIL: $label"
    failed=1
  fi
}

check "tools/install_from_sd_pluto_sat_tracker_v1.sh" "INSTALL_RUNTIME_HARDENING_V1" "installer hardening marker"
check "tools/install_from_sd_pluto_sat_tracker_v1.sh" "ensure_sd_writable" "installer checks SD writability"
check "tools/install_from_sd_pluto_sat_tracker_v1.sh" "reset_permissions" "installer resets permissions"
check "tools/install_from_sd_pluto_sat_tracker_v1.sh" "ensure_observer_json" "installer creates observer.json"
check "tools/install_from_sd_pluto_sat_tracker_v1.sh" "clear_stale_refresh_state" "installer clears stale refresh state"
check "tools/autorun_pluto_sat_tracker_v1.sh" "AUTORUN_RUNTIME_HARDENING_V1" "autorun hardening marker"
check "tools/autorun_pluto_sat_tracker_v1.sh" "ensure_sd_mounted_rw" "autorun checks SD writability"
check "tools/autorun_pluto_sat_tracker_v1.sh" "reset_permissions" "autorun resets permissions"
check "tools/autorun_pluto_sat_tracker_v1.sh" "ensure_initial_files" "autorun ensures initial data/config"

bash -n "$ROOT/tools/install_from_sd_pluto_sat_tracker_v1.sh"
echo "PASS: installer shell syntax"

bash -n "$ROOT/tools/autorun_pluto_sat_tracker_v1.sh"
echo "PASS: autorun shell syntax"

if [[ "$failed" -ne 0 ]]; then
  echo "Validation failed."
  exit 1
fi

echo "Validation passed."
