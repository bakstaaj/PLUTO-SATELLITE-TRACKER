# Next Passes Scroll Panel v1

Makes the Next Passes panel scroll internally when the list is taller than the visible browser window.

## Behavior

- `#passes` gets `max-height: calc(100vh - 155px)`.
- Vertical scrolling is enabled inside the Next Passes panel.
- Horizontal overflow is hidden.
- The filter buttons stay sticky at the top of the scroll panel.
- No JavaScript or backend behavior is changed.

## Run

```bash
python tools/apply_next_passes_scroll_panel_v1.py .
./tools/validate_next_passes_scroll_panel_v1.sh .
./tools/deploy_and_reboot.sh
```

After reboot:

```bash
source .pluto.env
python tools/test_next_passes_scroll_panel_v1.py
```
