# Rename Subsat to Ground Point v1

Label-only UI cleanup.

## Change

Replaces the technical label:

```text
Subsat
```

with the clearer label:

```text
Ground Point
```

No behavior or calculations are changed.

## Run

```bash
python tools/apply_rename_subsat_ground_point_v1.py .
./tools/validate_rename_subsat_ground_point_v1.sh .
./tools/deploy_and_reboot.sh
```

After reboot:

```bash
source .pluto.env
python tools/test_rename_subsat_ground_point_v1.py
```
