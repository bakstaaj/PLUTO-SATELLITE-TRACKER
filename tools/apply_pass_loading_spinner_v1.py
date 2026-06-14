#!/usr/bin/env python3
"""
Add spinner/loading feedback to the Next Passes panel during browser-owned quick
pass loading.

Run:
  python tools/apply_pass_loading_spinner_v1.py .
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

MARKER = "PASS_LOADING_SPINNER_V1"

CSS = """
    /* PASS_LOADING_SPINNER_V1 */
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

    .pass-loading-text {
      display: flex;
      flex-direction: column;
      gap: 2px;
      min-width: 0;
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
      to {
        transform: rotate(360deg);
      }
    }
"""

HELPER = """
      /* PASS_LOADING_SPINNER_V1 */
      function showPassLoadingFeedbackV1(title, detail) {
        const passesNode = document.getElementById("passes");
        if (!passesNode) return;
        passesNode.className = "pass-loading";
        passesNode.innerHTML = `
          <div class="pass-loading-spinner" aria-hidden="true"></div>
          <div class="pass-loading-text">
            <div class="pass-loading-title">${escapeHtml(title || "Loading passes...")}</div>
            <div class="pass-loading-detail">${escapeHtml(detail || "The quick preview will appear first; the full 24-hour rebuild continues in the background.")}</div>
          </div>
        `;
      }
"""

NEW_INITIAL = """<div id="passes" class="pass-loading">
          <div class="pass-loading-spinner" aria-hidden="true"></div>
          <div class="pass-loading-text">
            <div class="pass-loading-title">Loading passes...</div>
            <div class="pass-loading-detail">Connecting to Pluto and preparing the quick pass preview.</div>
          </div>
        </div>"""

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied pass loading spinner v1.")
        return 0

    backup = html.with_name(html.name + ".bak-pass-loading-spinner-v1")
    shutil.copy2(html, backup)

    media_marker = "    @media (max-width: 760px) {"
    if media_marker not in text:
        print("ERROR: could not find CSS insertion point.")
        return 1
    text = text.replace(media_marker, CSS + "\n" + media_marker, 1)

    old_initial = '<div id="passes" class="empty">Loading passes...</div>'
    if old_initial in text:
        text = text.replace(old_initial, NEW_INITIAL, 1)
    else:
        print("WARN: initial passes placeholder not found; continuing with runtime spinner only.")

    insertion_point = "      async function syncPlutoTimeFromBrowserV3() {"
    if insertion_point not in text:
        print("ERROR: could not find browser refresh helper insertion point.")
        return 1
    text = text.replace(insertion_point, HELPER + "\n" + insertion_point, 1)

    replacements = [
        (
            '          setBrowserStatus("Syncing browser time...", "pending");\n'
            '          await syncPlutoTimeFromBrowserV3();',
            '          setBrowserStatus("Syncing browser time...", "pending");\n'
            '          showPassLoadingFeedbackV1("Syncing Pluto time...", "Using browser time before requesting pass predictions.");\n'
            '          await syncPlutoTimeFromBrowserV3();'
        ),
        (
            '            setBrowserStatus("Loading quick pass preview...", "pending");\n'
            '            await triggerBrowserOwnedPassRefreshV3();',
            '            setBrowserStatus("Loading quick pass preview...", "pending");\n'
            '            showPassLoadingFeedbackV1("Loading quick pass preview...", "A short pass list will appear first; the full 24-hour pass rebuild continues after that.");\n'
            '            await triggerBrowserOwnedPassRefreshV3();'
        ),
        (
            '        const result = await poster("/api/refresh/passes", {});\n'
            '        scheduleFullRefreshPollV3();',
            '        showPassLoadingFeedbackV1("Requesting quick pass preview...", "Waiting for Pluto to publish the first pass results.");\n'
            '        const result = await poster("/api/refresh/passes", {});\n'
            '        scheduleFullRefreshPollV3();'
        ),
        (
            '            button.textContent = "Loading preview...";\n'
            '            await (originalPostJson || postJson)(`/api/refresh/${target}`, {});',
            '            button.textContent = "Loading preview...";\n'
            '            showPassLoadingFeedbackV1("Regenerating pass preview...", "The quick pass list will update first; full results continue in the background.");\n'
            '            await (originalPostJson || postJson)(`/api/refresh/${target}`, {});'
        ),
        (
            '        if (passesNode) passesNode.textContent = error.message || String(error);',
            '        if (passesNode) {\n'
            '          passesNode.className = "empty";\n'
            '          passesNode.textContent = error.message || String(error);\n'
            '        }'
        ),
    ]

    missing = []
    for old, new in replacements:
        if old not in text:
            missing.append(old.splitlines()[0].strip())
            continue
        text = text.replace(old, new, 1)

    if missing:
        print("ERROR: could not find expected startup refresh blocks:")
        for item in missing:
            print(f"  - {item}")
        return 1

    html.write_text(text, encoding="utf-8", newline="\n")
    print("Applied pass loading spinner v1.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
