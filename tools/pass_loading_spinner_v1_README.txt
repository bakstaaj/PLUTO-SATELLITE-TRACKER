# Pass Loading Spinner v1

Adds visible loading feedback in the **Next Passes** panel while browser-owned quick pass loading is running.

## Behavior

- Initial page load shows a spinner in the pass panel.
- Startup browser time sync shows: `Syncing Pluto time...`
- Quick preview request shows: `Loading quick pass preview...`
- Manual **Regenerate Passes** shows: `Regenerating pass preview...`
- Once the quick preview returns, the normal pass list replaces the spinner.
- Full 24-hour refresh continues in the background.

## Run

```bash
python tools/apply_pass_loading_spinner_v1.py .
./tools/validate_pass_loading_spinner_v1.sh .
./tools/deploy_and_reboot.sh
```
