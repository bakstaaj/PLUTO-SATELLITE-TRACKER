# Pass Loading File-Watch v3

Fixes the pass spinner disappearing and showing the old filtered pass-list result while the non-blocking quick refresh is still producing a new `passes.json`.

## Behavior

The UI now watches `/api/passes` itself:

- remembers the old `generated_utc`;
- keeps the spinner visible;
- polls through normal `refresh()` calls;
- only renders passes after `start_utc` is near browser time and `generated_utc` changes;
- times out after 2 minutes to avoid infinite spinning if the backend fails.

## Run

```bash
python tools/apply_pass_loading_file_watch_v3.py .
./tools/validate_pass_loading_file_watch_v3.sh .
./tools/deploy_and_reboot.sh
```
