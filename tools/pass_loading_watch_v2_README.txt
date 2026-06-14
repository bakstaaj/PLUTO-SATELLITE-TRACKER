# Pass Loading Watch v2

Fixes the spinner disappearing while the backend pass refresh is still running.

## Why it happened

The refresh endpoint is non-blocking. The UI asked for a pass refresh, then `refresh()` immediately rendered the old `/api/passes` payload, replacing the spinner.

## New behavior

The UI keeps a quick-load watch active while pass refresh is running. During normal `refresh()` calls:

- If `/api/refresh/status` says `target=passes` and the payload is still stale/empty, the spinner stays visible.
- As soon as `/api/passes` looks current enough for the browser time, the normal pass list renders.
- The full 24-hour background refresh can continue without hiding the pass list.

## Run

```bash
python tools/apply_pass_loading_watch_v2.py .
./tools/validate_pass_loading_watch_v2.sh .
./tools/deploy_and_reboot.sh
```
