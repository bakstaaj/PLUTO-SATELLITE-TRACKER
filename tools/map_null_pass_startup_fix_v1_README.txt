# Map Null-Pass Startup Fix v1

Fixes this UI startup crash:

`Cannot read properties of null (reading 'aos_utc')`

The map panel can render before a pass is selected. The live-look helper must not call `passTimingState(pass)` when `pass` is null.

## Run

```bash
python tools/apply_map_null_pass_startup_fix_v1.py .
./tools/validate_map_null_pass_startup_fix_v1.sh .
./tools/deploy_and_reboot.sh
```

After reboot:

```bash
source .pluto.env
python tools/diagnose_pluto_startup_backend_v1.py
```
