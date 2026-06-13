#!/usr/bin/env python3
from __future__ import annotations
import argparse
import datetime as dt
from pathlib import Path

UI_MARKER = "UI_BROWSER_TIME_QUICK_FULL_REFRESH_V3_APPEND"
REFRESH_MARKER = "QUICK_FULL_PASS_REFRESH_V3_BROWSER_TIME_OWNED"

UI_BOOTSTRAP_BLOCK = r'''
    // UI_BROWSER_TIME_QUICK_FULL_REFRESH_V3_APPEND
    // Browser-owned startup recovery for standalone Pluto operation.
    // The browser is the time source: sync Pluto UTC, publish a quick pass
    // preview, and let the full 24-hour pass rebuild complete in the background.
    (function installBrowserTimeQuickFullRefreshV3() {
      if (window.__plutoBrowserTimeQuickFullRefreshV3Installed) return;
      window.__plutoBrowserTimeQuickFullRefreshV3Installed = true;

      let browserRefreshPollTimerId = 0;
      let browserBootstrapStarted = false;
      let originalPostJson = null;

      function browserEpochSeconds() {
        return Math.floor(Date.now() / 1000);
      }

      function isoEpochSeconds(value) {
        const millis = Date.parse(value || "");
        return Number.isFinite(millis) ? Math.floor(millis / 1000) : 0;
      }

      function passPayloadNeedsBrowserRefresh(payload) {
        const passes = (payload && payload.passes) || [];
        const startEpoch = isoEpochSeconds(payload && payload.start_utc);
        const nowEpoch = browserEpochSeconds();
        if (!passes.length || !startEpoch) return true;
        return Math.abs(nowEpoch - startEpoch) > 15 * 60;
      }

      function setBrowserStatus(text, state) {
        try {
          if (typeof setPillState === "function") {
            setPillState("status", text, state || "pending");
          } else {
            const status = document.getElementById("status");
            if (status) status.textContent = text;
          }
        } catch (_error) {
        }
      }

      async function syncPlutoTimeFromBrowserV3() {
        const result = await getJson(`/api/time/sync?epoch=${browserEpochSeconds()}`);
        try {
          if (typeof setPillState === "function") setPillState("timePill", "Time synced", "running");
        } catch (_error) {
        }
        return result;
      }

      function scheduleFullRefreshPollV3() {
        if (browserRefreshPollTimerId) {
          window.clearInterval(browserRefreshPollTimerId);
        }
        const startedAt = Date.now();
        browserRefreshPollTimerId = window.setInterval(async () => {
          try {
            await refresh();
            const passesPayload = await getJson("/api/passes");
            const refreshStatus = await getJson("/api/refresh/status");
            const hours = Number((passesPayload && passesPayload.hours) ||
              (refreshStatus && refreshStatus.summary && refreshStatus.summary.prediction_hours) || 0);
            const count = Number((passesPayload && passesPayload.metadata && passesPayload.metadata.pass_count) ||
              ((passesPayload && passesPayload.passes) || []).length || 0);
            if ((refreshStatus.state || "") === "ok" && hours >= 23 && count >= 20 && !passPayloadNeedsBrowserRefresh(passesPayload)) {
              window.clearInterval(browserRefreshPollTimerId);
              browserRefreshPollTimerId = 0;
              setBrowserStatus("Backend online", "running");
            }
            if (Date.now() - startedAt > 15 * 60 * 1000) {
              window.clearInterval(browserRefreshPollTimerId);
              browserRefreshPollTimerId = 0;
            }
          } catch (_error) {
          }
        }, 6000);
      }

      async function triggerBrowserOwnedPassRefreshV3() {
        await syncPlutoTimeFromBrowserV3();
        const poster = originalPostJson || postJson;
        const result = await poster("/api/refresh/passes", {});
        scheduleFullRefreshPollV3();
        return result;
      }

      async function bootstrapBrowserTimeRefreshV3() {
        if (browserBootstrapStarted) return;
        browserBootstrapStarted = true;
        try {
          setBrowserStatus("Syncing browser time...", "pending");
          await syncPlutoTimeFromBrowserV3();
          await refresh();
          const passesPayload = await getJson("/api/passes");
          if (passPayloadNeedsBrowserRefresh(passesPayload)) {
            setBrowserStatus("Loading quick pass preview...", "pending");
            await triggerBrowserOwnedPassRefreshV3();
            await refresh();
          }
        } catch (error) {
          setBrowserStatus("Startup refresh failed", "stopped");
          const passesNode = document.getElementById("passes");
          if (passesNode) passesNode.textContent = error.message || String(error);
        } finally {
          browserBootstrapStarted = false;
        }
      }

      try {
        originalPostJson = postJson;
        postJson = async function browserTimePostJsonWrapperV3(url, payload) {
          if (url === "/api/refresh/passes" || url === "/api/refresh/all") {
            await syncPlutoTimeFromBrowserV3();
            const result = await originalPostJson(url, payload);
            scheduleFullRefreshPollV3();
            return result;
          }
          return originalPostJson(url, payload);
        };
      } catch (_error) {
      }

      try {
        syncPlutoTime = async function syncPlutoTime(button) {
          button.disabled = true;
          button.textContent = "Syncing...";
          try {
            await syncPlutoTimeFromBrowserV3();
            button.textContent = "Synced";
            await refresh();
          } catch (error) {
            button.textContent = "Sync failed";
            throw error;
          } finally {
            setTimeout(() => {
              button.disabled = false;
              button.textContent = "Sync Time";
            }, 1200);
          }
        };
      } catch (_error) {
      }

      try {
        const previousRunDataRefresh = runDataRefresh;
        runDataRefresh = async function runDataRefresh(target, button) {
          if (target !== "passes" && target !== "all") {
            return previousRunDataRefresh(target, button);
          }
          const originalText = button.textContent;
          button.disabled = true;
          button.textContent = "Syncing time...";
          try {
            await syncPlutoTimeFromBrowserV3();
            button.textContent = "Loading preview...";
            await (originalPostJson || postJson)(`/api/refresh/${target}`, {});
            scheduleFullRefreshPollV3();
            button.textContent = "Done";
            await refresh();
          } catch (error) {
            button.textContent = "Failed";
            await refresh().catch(() => {});
            throw error;
          } finally {
            setTimeout(() => {
              button.disabled = false;
              button.textContent = originalText;
            }, 1200);
          }
        };
      } catch (_error) {
      }

      window.plutoBrowserTimeQuickFullRefreshV3 = {
        syncTime: syncPlutoTimeFromBrowserV3,
        triggerPassRefresh: triggerBrowserOwnedPassRefreshV3,
        bootstrap: bootstrapBrowserTimeRefreshV3,
        scheduleFullPoll: scheduleFullRefreshPollV3
      };

      window.setTimeout(() => {
        bootstrapBrowserTimeRefreshV3();
      }, 300);
    })();
'''

def utc_stamp() -> str:
    return dt.datetime.now(dt.UTC).strftime("%Y%m%d%H%M%S")

def backup(path: Path) -> Path:
    bak = path.with_name(path.name + f".bak-{utc_stamp()}")
    bak.write_text(path.read_text(encoding="utf-8"), encoding="utf-8")
    return bak

def patch_web_index(root: Path) -> None:
    path = root / "web" / "index.html"
    if not path.exists():
        raise SystemExit(f"ERROR: not found: {path}")
    text = path.read_text(encoding="utf-8")
    if UI_MARKER in text:
        print(f"PASS: {path} already contains {UI_MARKER}")
        return
    insert_at = text.rfind("  </script>")
    if insert_at < 0:
        insert_at = text.rfind("</script>")
    if insert_at < 0:
        raise SystemExit("ERROR: could not find closing </script> in web/index.html")
    bak = backup(path)
    text = text[:insert_at] + UI_BOOTSTRAP_BLOCK + "\n" + text[insert_at:]
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"PASS: patched {path}")
    print(f"Backup: {bak}")

def check_refresh_runner(root: Path) -> None:
    path = root / "tools" / "pluto_refresh_data.sh"
    if not path.exists():
        print(f"WARN: refresh runner not found: {path}")
        return
    text = path.read_text(encoding="utf-8", errors="replace")
    if REFRESH_MARKER in text:
        print("PASS: refresh runner already has browser-time quick/full marker")
    else:
        print("WARN: refresh runner marker missing; apply the quick/full refresh runner patch before deploying")

def main() -> int:
    parser = argparse.ArgumentParser(description="Append robust browser-time quick/full startup bootstrap to web/index.html.")
    parser.add_argument("repo", nargs="?", default=".")
    root = Path(parser.parse_args().repo).resolve()
    check_refresh_runner(root)
    patch_web_index(root)
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
