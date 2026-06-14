#!/usr/bin/env python3
"""
Patch web/index.html so the UI streaming PCM player automatically reopens the
backend audio stream when the HTTP stream ends.

Symptom fixed:
  UI says "Backend audio stream ended."

Cause:
  The backend route is valid and returns RIFF/WAVE, but the response can close.
  The UI should treat that as a reconnectable stream segment while the audio
  session is active.

Run from repo root:
  python tools/apply_ui_streaming_pcm_reconnect_v3.py .
"""

from __future__ import annotations

import re
import shutil
import sys
from pathlib import Path


MARKER = "UI_STREAMING_PCM_RECONNECT_V3"
START = "function analogAudioUrl(pass)"
END = "function bindAnalogAudio(pass, node)"


NEW_BLOCK = r"""    /* UI_STREAMING_PCM_RECONNECT_V3
     * Backend-owned streaming audio with browser PCM playback and reconnect.
     *
     * Some backend WAV streams close after a short segment. While the session is
     * active, the browser reopens /api/radio/audio/live.wav?stream=1 instead of
     * stopping playback. The backend still owns tuning, DSP, and PCM generation;
     * the browser only plays decoded PCM.
     */
    function analogAudioUrl(pass) {
      const radio = pass && pass.radio ? pass.radio : {};
      const downlink = radio.downlink_hz || (pass && pass.downlinks_hz ? pass.downlinks_hz[0] : 0);
      if (!downlink) return "";
      const params = new URLSearchParams({ downlink_hz: String(downlink) });
      return {
        downlink,
        startUrl: `/api/radio/audio/live/start?${params.toString()}`,
        streamUrl: `/api/radio/audio/live.wav?stream=1&${params.toString()}`,
        stopUrl: `/api/radio/audio/live/stop`
      };
    }

    function concatUint8Arrays(a, b) {
      if (!a || !a.length) return b || new Uint8Array();
      if (!b || !b.length) return a;
      const out = new Uint8Array(a.length + b.length);
      out.set(a, 0);
      out.set(b, a.length);
      return out;
    }

    function pcm16ToAudioBuffer(context, bytes, sampleRate) {
      const sampleCount = Math.floor(bytes.length / 2);
      const buffer = context.createBuffer(1, sampleCount, sampleRate);
      const channel = buffer.getChannelData(0);
      const view = new DataView(bytes.buffer, bytes.byteOffset, sampleCount * 2);
      for (let i = 0; i < sampleCount; i += 1) {
        const sample = view.getInt16(i * 2, true);
        channel[i] = Math.max(-1, Math.min(1, sample / 32768));
      }
      return buffer;
    }

    async function stopAnalogAudio(reason = "Analog monitor stopped.") {
      const session = analogAudioSession;
      analogAudioSession = null;
      if (!session) return;

      session.stopped = true;
      if (session.reconnectTimer) {
        window.clearTimeout(session.reconnectTimer);
        session.reconnectTimer = 0;
      }
      try {
        if (session.controller) session.controller.abort();
      } catch (_error) {
      }

      try {
        for (const source of session.sources || []) {
          try { source.stop(); } catch (_error) {}
        }
      } catch (_error) {
      }

      try {
        if (session.stopUrl) {
          await postJson(session.stopUrl, {});
        }
      } catch (_error) {
      }

      try {
        if (session.context) await session.context.close();
      } catch (_error) {
      }

      if (session.button?.isConnected) {
        session.button.textContent = "Start Analog Audio";
      }
      if (session.statusNode?.isConnected) {
        session.statusNode.textContent = reason;
      }
    }

    async function startAnalogAudio(pass, button, statusNode) {
      const audioUrls = analogAudioUrl(pass);
      const AudioCtx = window.AudioContext || window.webkitAudioContext;
      const sampleRate = 24000;
      const targetChunkBytes = 4096 * 2;
      if (!AudioCtx) {
        throw new Error("Web Audio is not available in this browser.");
      }
      if (!audioUrls) {
        throw new Error("No usable downlink is available for this pass.");
      }

      await stopAnalogAudio();

      statusNode.textContent = "Planning backend tuning and Doppler tracking...";
      try {
        await getJson(`/api/radio/plan?${new URLSearchParams({
          name: pass.name || "",
          norad: String(pass.norad_id || ""),
          aos: pass.aos_utc || "",
          downlink: String(audioUrls.downlink),
          mode: (pass.radio && pass.radio.mode) || (pass.modes || [])[0] || "",
          description: (pass.radio && pass.radio.description) || ""
        }).toString()}`);
      } catch (_error) {
        // Continue: live/start can still tune directly by downlink_hz.
      }

      if (pass.doppler_plan && (pass.doppler_plan.points || []).length) {
        try {
          await postJson("/api/radio/track/plan", dopplerTrackPayload(pass));
          await getJson("/api/radio/track/auto/start");
        } catch (_error) {
          // Continue with fixed-frequency audio if auto tracking cannot start.
        }
      }

      statusNode.textContent = "Starting backend audio DSP...";
      await postJson(audioUrls.startUrl, {});

      const context = new AudioCtx({ sampleRate });
      await context.resume();

      const session = {
        button,
        context,
        controller: null,
        nextTime: context.currentTime + 0.35,
        passKey: passKey(pass),
        reconnectCount: 0,
        reconnectTimer: 0,
        sources: [],
        statusNode,
        stopUrl: audioUrls.stopUrl,
        stopped: false,
        totalBytes: 0,
        totalBuffers: 0
      };
      analogAudioSession = session;

      button.textContent = "Stop Analog Audio";
      statusNode.textContent = "Opening backend decoded audio stream...";

      const schedulePcmBytes = (pcmBytes) => {
        if (session.stopped || !pcmBytes.length) return;
        const evenLength = pcmBytes.length - (pcmBytes.length % 2);
        if (evenLength <= 0) return;

        const audioBuffer = pcm16ToAudioBuffer(context, pcmBytes.slice(0, evenLength), sampleRate);
        const source = context.createBufferSource();
        source.buffer = audioBuffer;
        source.connect(context.destination);
        source.onended = () => {
          const index = session.sources.indexOf(source);
          if (index >= 0) session.sources.splice(index, 1);
        };

        const startAt = Math.max(context.currentTime + 0.05, session.nextTime);
        source.start(startAt);
        session.nextTime = startAt + audioBuffer.duration;
        session.sources.push(source);
        session.totalBuffers += 1;

        const bufferedSeconds = Math.max(0, session.nextTime - context.currentTime);
        if (statusNode?.isConnected) {
          statusNode.textContent = `Playing backend decoded audio from Pluto (${bufferedSeconds.toFixed(1)}s buffered, reconnects ${session.reconnectCount}).`;
        }
      };

      const readOneStreamSegment = async () => {
        const controller = new AbortController();
        session.controller = controller;
        const streamUrl = `${audioUrls.streamUrl}&request=${Date.now()}&reconnect=${session.reconnectCount}`;
        const response = await fetch(streamUrl, {
          cache: "no-store",
          signal: controller.signal
        });

        if (!response.ok) {
          throw new Error(`${streamUrl}: ${response.status}`);
        }
        if (!response.body || !response.body.getReader) {
          throw new Error("Browser streaming fetch is not available.");
        }

        const reader = response.body.getReader();
        let pending = new Uint8Array();
        let headerSkipped = false;
        let segmentBytes = 0;

        const processPending = (force = false) => {
          if (!headerSkipped) {
            if (pending.length < 44) return;
            pending = pending.slice(44);
            headerSkipped = true;
          }

          while (pending.length >= targetChunkBytes || (force && pending.length >= 2)) {
            const take = force ? pending.length - (pending.length % 2) : targetChunkBytes;
            const chunk = pending.slice(0, take);
            pending = pending.slice(take);
            schedulePcmBytes(chunk);
          }
        };

        while (!session.stopped) {
          const { value, done } = await reader.read();
          if (done) break;
          if (!value || !value.length) continue;

          segmentBytes += value.length;
          session.totalBytes += value.length;
          pending = concatUint8Arrays(pending, value);
          processPending(false);

          if (statusNode?.isConnected && session.totalBuffers === 0) {
            statusNode.textContent = `Receiving backend audio stream (${session.totalBytes} bytes)...`;
          }
        }

        processPending(true);
        return segmentBytes;
      };

      const reconnectLoop = async () => {
        while (!session.stopped && analogAudioSession === session) {
          try {
            const segmentBytes = await readOneStreamSegment();
            if (session.stopped || analogAudioSession !== session) return;

            session.reconnectCount += 1;
            const bufferedSeconds = Math.max(0, session.nextTime - context.currentTime);
            if (statusNode?.isConnected) {
              statusNode.textContent =
                `Backend stream segment ended after ${segmentBytes} bytes; reconnecting (${bufferedSeconds.toFixed(1)}s buffered).`;
            }

            // Reconnect quickly. If the buffer is empty, reconnect immediately.
            const delayMs = bufferedSeconds > 0.8 ? 200 : 20;
            await new Promise((resolve) => {
              session.reconnectTimer = window.setTimeout(resolve, delayMs);
            });
            session.reconnectTimer = 0;
          } catch (error) {
            if (session.stopped || analogAudioSession !== session) return;
            const message = error && error.message ? error.message : "Backend audio stream failed.";
            if (statusNode?.isConnected) statusNode.textContent = message;
            const statusBar = document.getElementById("status");
            if (statusBar) statusBar.textContent = message;
            await stopAnalogAudio(message);
            return;
          }
        }
      };

      reconnectLoop();
    }

"""

def main() -> int:
    root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    html = root / "web" / "index.html"
    if not html.exists():
        print(f"ERROR: missing {html}")
        return 1

    text = html.read_text(encoding="utf-8", errors="replace")
    if MARKER in text:
        print("Already applied UI streaming PCM reconnect v3.")
        return 0

    start = text.find(START)
    end = text.find(END, start if start >= 0 else 0)
    if start < 0 or end < 0 or end <= start:
        print("ERROR: could not locate analog audio function block.")
        print(f"  start_found={start >= 0} end_found={end >= 0}")
        return 1

    backup = html.with_name(html.name + ".bak-ui-streaming-pcm-reconnect-v3")
    shutil.copy2(html, backup)

    new_text = text[:start] + NEW_BLOCK + text[end:]
    html.write_text(new_text, encoding="utf-8", newline="\n")

    print("Applied UI streaming PCM reconnect v3.")
    print(f"Updated: {html}")
    print(f"Backup:  {backup}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
