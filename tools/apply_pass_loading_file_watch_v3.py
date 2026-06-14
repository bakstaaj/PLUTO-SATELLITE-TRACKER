#!/usr/bin/env python3
# Pass loading file-watch v3.
#
# Fixes the spinner disappearing because refresh() renders the old /api/passes
# payload while the non-blocking pass refresh is still running.
#
# Run:
#   python tools/apply_pass_loading_file_watch_v3.py .

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "PASS_LOADING_FILE_WATCH_V3"

HELPER = r'''    /* PASS_LOADING_FILE_WATCH_V3 */
    function passPayloadTimestampV3(payload) {
      return String((payload && payload.generated_utc) || "");
    }

    function passPayloadStartMsV3(payload) {
      const startMs = Date.parse((payload && payload.start_utc) || "");
      return Number.isFinite(startMs) ? startMs : 0;
    }

    function rememberPassPayloadStampV3(payload) {
      const generated = passPayloadTimestampV3(payload);
      const startUtc = String((payload && payload.start_utc) || "");
      if (generated) window.__plutoLastPassGeneratedUtcV3 = generated;
      if (startUtc) window.__plutoLastPassStartUtcV3 = startUtc;
    }

    function startPassFileWatchV3(title, detail) {
      const existing = window.__plutoPassFileWatchV3 || {};
      window.__plutoPassFileWatchV3 = {
        active: true,
        startedAt: Date.now(),
        previousGeneratedUtc: existing.previousGeneratedUtc || window.__plutoLastPassGeneratedUtcV3 || "",
        previousStartUtc: existing.previousStartUtc || window.__plutoLastPassStartUtcV3 || ""
      };
      if (typeof showPassLoadingFeedbackV2 === "function") {
        showPassLoadingFeedbackV2(title, detail);
      } else if (typeof showPassLoadingFeedbackV1 === "function") {
        showPassLoadingFeedbackV1(title, detail);
      } else {
        const passesNode = document.getElementById("passes");
        if (passesNode) {
          passesNode.className = "empty";
          passesNode.textContent = title || "Loading passes...";
        }
      }
    }

    function passPayloadLooksCurrentForBrowserV3(payload) {
      const passes = (payload && payload.passes) || [];
      if (!passes.length) return false;
      const startMs = passPayloadStartMsV3(payload);
      if (!startMs) return false;
      return Math.abs(Date.now() - startMs) <= (15 * 60 * 1000);
    }

    function passPayloadLooksNewEnoughV3(payload) {
      if (!passPayloadLooksCurrentForBrowserV3(payload)) return false;
      const watch = window.__plutoPassFileWatchV3 || {};
      const generated = passPayloadTimestampV3(payload);
      if (!watch.active) return true;

      /*
       * During a manual or startup refresh, do not render the old pass list
       * simply because it has some rows. Wait for generated_utc to change.
       * If no previous stamp is known, current-enough rows are acceptable.
       */
      if (watch.previousGeneratedUtc && generated && generated === watch.previousGeneratedUtc) {
        return false;
      }
      return true;
    }

    function shouldKeepPassFileSpinnerV3(passesPayload) {
      const watch = window.__plutoPassFileWatchV3 || {};
      if (!watch.active) return false;
      if (passPayloadLooksNewEnoughV3(passesPayload)) {
        window.__plutoPassFileWatchV3.active = false;
        rememberPassPayloadStampV3(passesPayload);
        return false;
      }

      /*
       * Keep watching for up to 2 minutes. If the backend never publishes a new
       * file, let the normal error/empty state appear instead of spinning forever.
       */
      if (Date.now() - Number(watch.startedAt || 0) > (2 * 60 * 1000)) {
        window.__plutoPassFileWatchV3.active = false;
        return false;
      }
      return true;
    }

'''

NEW_REFRESH_SNIPPET = r'''      if (shouldKeepPassFileSpinnerV3(passes)) {
        const refreshState = (refreshStatus && refreshStatus.state) || "waiting";
        startPassFileWatchV3(
          "Loading quick pass preview...",
          `Watching /api/passes for a new pass file. Refresh state: ${refreshState}.`
        );
      } else {
        rememberPassPayloadStampV3(passes);
        renderPasses(passes);
      }
'''

def replace_between(text: str, start_marker: str, end_marker: str, replacement: str) -> tuple[str, bool]:
    start = text.find(start_marker)
    if start < 0:
        return text, False
    end = text.find(end_marker, start)
    if end < 0:
        return text, False
    return text[:start] + replacement + text[end:], True

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied pass loading file-watch v3.")
        return 0

    backup = html.with_name(html.name + ".bak-pass-loading-file-watch-v3")
    shutil.copy2(html, backup)

    render_marker = "    function renderPasses(payload) {"
    if render_marker not in text:
        print("ERROR: could not find renderPasses().")
        return 1
    text = text.replace(render_marker, HELPER + "\n" + render_marker, 1)

    v2_start = "      if (shouldKeepPassLoadingSpinnerV2(passes, refreshStatus)) {"
    schedule_marker = "      scheduleRefreshLoop();"
    text, ok = replace_between(text, v2_start, schedule_marker, NEW_REFRESH_SNIPPET)
    if not ok:
        old = "      renderPasses(passes);\n      scheduleRefreshLoop();"
        if old not in text:
            print("ERROR: could not find refresh pass render block.")
            return 1
        text = text.replace(old, NEW_REFRESH_SNIPPET + schedule_marker, 1)

    loading_titles = [
        "Syncing Pluto time...",
        "Loading quick pass preview...",
        "Requesting quick pass preview...",
        "Regenerating pass preview...",
    ]
    for title in loading_titles:
        text = text.replace(f'showPassLoadingFeedbackV2("{title}"', f'startPassFileWatchV3("{title}"')
        text = text.replace(f'showPassLoadingFeedbackV1("{title}"', f'startPassFileWatchV3("{title}"')

    html.write_text(text, encoding="utf-8", newline="\n")

    print("Applied pass loading file-watch v3.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
