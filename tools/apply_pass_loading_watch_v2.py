#!/usr/bin/env python3
# Pass loading watcher v2.
#
# Fixes the spinner disappearing because refresh() renders the old /api/passes
# payload while the non-blocking pass refresh is still running.
#
# Run:
#   python tools/apply_pass_loading_watch_v2.py .

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "PASS_LOADING_WATCH_V2"

GLOBAL_HELPER = r'''    /* PASS_LOADING_WATCH_V2 */
    function passPayloadNeedsQuickLoadV2(payload) {
      const passes = (payload && payload.passes) || [];
      if (!passes.length) return true;
      const startMs = Date.parse((payload && payload.start_utc) || "");
      if (!Number.isFinite(startMs)) return true;
      return Math.abs(Date.now() - startMs) > (15 * 60 * 1000);
    }

    function showPassLoadingFeedbackV2(title, detail) {
      window.__plutoPassLoadingWatchV2 = true;
      const passesNode = document.getElementById("passes");
      if (!passesNode) return;
      passesNode.className = "pass-loading";
      passesNode.innerHTML = `
        <div class="pass-loading-spinner" aria-hidden="true"></div>
        <div class="pass-loading-text">
          <div class="pass-loading-title">${escapeHtml(title || "Loading passes...")}</div>
          <div class="pass-loading-detail">${escapeHtml(detail || "Watching Pluto for the quick pass preview.")}</div>
        </div>
      `;
    }

    function shouldKeepPassLoadingSpinnerV2(passesPayload, refreshStatus) {
      if (!window.__plutoPassLoadingWatchV2) return false;
      const target = (refreshStatus && refreshStatus.target) || "";
      const state = (refreshStatus && refreshStatus.state) || "";
      const running = target === "passes" && !["", "idle", "ok", "failed"].includes(state);
      return running && passPayloadNeedsQuickLoadV2(passesPayload);
    }

'''

NEW_REFRESH_SNIPPET = r'''      if (shouldKeepPassLoadingSpinnerV2(passes, refreshStatus)) {
        showPassLoadingFeedbackV2(
          "Loading quick pass preview...",
          `Refresh state: ${refreshStatus.state || "running"}. Waiting for Pluto to publish a current pass list.`
        );
      } else {
        window.__plutoPassLoadingWatchV2 = false;
        renderPasses(passes);
      }
'''

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied pass loading watcher v2.")
        return 0

    backup = html.with_name(html.name + ".bak-pass-loading-watch-v2")
    shutil.copy2(html, backup)

    if ".pass-loading-spinner" not in text:
        media_marker = "    @media (max-width: 760px) {"
        css = r'''
    /* PASS_LOADING_WATCH_V2_MIN_CSS */
    .pass-loading {
      display: flex;
      align-items: center;
      gap: 12px;
      padding: 16px;
      border: 1px dashed var(--line);
      border-radius: 12px;
      background: var(--panel2);
      color: var(--ink);
    }
    .pass-loading-spinner {
      width: 22px;
      height: 22px;
      border: 3px solid rgba(30, 73, 95, 0.18);
      border-top-color: var(--accent);
      border-radius: 50%;
      animation: passLoadingSpin 0.9s linear infinite;
      flex: 0 0 auto;
    }
    .pass-loading-title {
      font-weight: 700;
      font-size: 14px;
    }
    .pass-loading-detail {
      color: var(--muted);
      font-size: 12px;
    }
    @keyframes passLoadingSpin {
      to { transform: rotate(360deg); }
    }
'''
        if media_marker not in text:
            print("ERROR: could not find CSS insertion point.")
            return 1
        text = text.replace(media_marker, css + "\n" + media_marker, 1)

    render_marker = "    function renderPasses(payload) {"
    if render_marker not in text:
        print("ERROR: could not find renderPasses().")
        return 1
    text = text.replace(render_marker, GLOBAL_HELPER + "\n" + render_marker, 1)

    text = text.replace("showPassLoadingFeedbackV1(", "showPassLoadingFeedbackV2(")

    old = "      renderPasses(passes);\n      scheduleRefreshLoop();"
    if old not in text:
        print("ERROR: could not find renderPasses(passes) refresh block.")
        return 1
    text = text.replace(old, NEW_REFRESH_SNIPPET + "      scheduleRefreshLoop();", 1)

    html.write_text(text, encoding="utf-8", newline="\n")

    print("Applied pass loading watcher v2.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
