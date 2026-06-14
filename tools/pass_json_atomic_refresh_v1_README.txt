# Pass JSON Atomic Refresh v1

Fixes `/api/passes` invalid JSON errors such as:

`Expected ',' or '}' after property value in JSON at position 32768`

## Cause

`tools/update_pass_predictions.py` wrote `passes.json` directly to the final path. The UI polls `/api/passes` during startup and refreshes, so it could read the file while Python was still writing it.

## Fix

The pass generator now:

1. Builds the full JSON body in memory.
2. Validates the JSON with `json.loads`.
3. Writes `passes.json.tmp`.
4. Flushes and fsyncs the temp file.
5. Atomically replaces `passes.json` with `os.replace`.

## Run

```bash
python tools/apply_pass_json_atomic_refresh_v1.py .
./tools/validate_pass_json_atomic_refresh_v1.sh .
./tools/deploy_and_reboot.sh
```

After reboot:

```bash
source .pluto.env
python tools/repair_and_validate_passes_json_v1.py
```
