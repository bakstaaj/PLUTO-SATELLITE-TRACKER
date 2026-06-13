#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-.}"
WEB="$ROOT/web/index.html"
REFRESH="$ROOT/tools/pluto_refresh_data.sh"
python - "$WEB" "$REFRESH" <<'PY'
from pathlib import Path
import sys
web = Path(sys.argv[1])
refresh = Path(sys.argv[2])
failed = False

def check(name, cond):
    global failed
    if cond:
        print(f"PASS: {name}")
    else:
        print(f"FAIL: {name}")
        failed = True

wt = web.read_text(encoding='utf-8', errors='replace') if web.exists() else ''
rt = refresh.read_text(encoding='utf-8', errors='replace') if refresh.exists() else ''
check('refresh runner browser-time quick/full marker present', 'QUICK_FULL_PASS_REFRESH_V3_BROWSER_TIME_OWNED' in rt)
check('UI v3 append marker present', 'UI_BROWSER_TIME_QUICK_FULL_REFRESH_V3_APPEND' in wt)
check('browser epoch sync helper present', 'syncPlutoTimeFromBrowserV3' in wt)
check('startup bootstrap scheduled', 'bootstrapBrowserTimeRefreshV3();' in wt)
check('pass refresh trigger helper present', 'triggerBrowserOwnedPassRefreshV3' in wt)
check('postJson wrapper syncs refresh calls', 'browserTimePostJsonWrapperV3' in wt and '/api/refresh/passes' in wt)
check('background full refresh poller present', 'scheduleFullRefreshPollV3' in wt)
check('manual regenerate override present', 'previousRunDataRefresh' in wt and 'Loading preview' in wt)
if failed:
    raise SystemExit('FAIL: UI browser-time bootstrap patch v3 validation failed')
print('PASS: UI browser-time bootstrap patch v3 validation passed')
PY
