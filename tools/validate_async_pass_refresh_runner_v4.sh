#!/usr/bin/env bash
set -u
ROOT="${1:-.}"
FILE="$ROOT/tools/pluto_refresh_data.sh"
fail=0
check() {
  local label="$1" pattern="$2"
  if grep -Fq -- "$pattern" "$FILE" 2>/dev/null; then
    echo "PASS: $label"
  else
    echo "FAIL: $label"
    fail=1
  fi
}
if [[ ! -f "$FILE" ]]; then
  echo "FAIL: missing $FILE"
  exit 1
fi
check "v4 async runner marker present" "ASYNC_PASS_REFRESH_RUNNER_V4"
check "passes mode queues background worker" "start_pass_worker_background"
check "passes mode returns after queueing" "Queued quick pass preview on Pluto"
check "quick worker mode present" "passes_worker)"
check "quick preview uses temporary output" "passes.quick.tmp"
check "full refresh uses temporary output" "passes.full.tmp"
check "explicit browser-sync start UTC preserved" "PLUTO_REFRESH_START_UTC"
check "stale pass locks are cleared before queueing" "clear_pass_locks"
if [[ "$fail" -ne 0 ]]; then
  echo "FAIL: async pass refresh runner v4 validation failed"
  exit 1
fi
echo "PASS: async pass refresh runner v4 validation passed"
