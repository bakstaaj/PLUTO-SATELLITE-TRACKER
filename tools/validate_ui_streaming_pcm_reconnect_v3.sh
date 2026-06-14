#!/usr/bin/env bash
set -u

ROOT="${1:-.}"
HTML="$ROOT/web/index.html"

if [[ ! -f "$HTML" ]]; then
  echo "FAIL: missing $HTML"
  exit 1
fi

python - "$HTML" <<'PY'
import sys
from pathlib import Path
text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")

checks = [
    ("UI_STREAMING_PCM_RECONNECT_V3 marker present", "UI_STREAMING_PCM_RECONNECT_V3" in text),
    ("streamUrl uses live.wav stream=1", "streamUrl: `/api/radio/audio/live.wav?stream=1&${params.toString()}`" in text),
    ("streaming fetch reader used", "response.body.getReader()" in text),
    ("PCM16 converter present", "pcm16ToAudioBuffer" in text),
    ("WAV header skip present", "pending = pending.slice(44);" in text),
    ("reconnect loop present", "const reconnectLoop = async () =>" in text),
    ("segment-ended message replaced", "Backend audio stream ended." not in text),
    ("backend DSP start route still used", "await postJson(audioUrls.startUrl, {});" in text),
    ("backend stop route still used", "await postJson(session.stopUrl, {});" in text),
    ("old block fetch pump removed", "from=${session.cursor}&samples=${liveBlockSamples}" not in text),
    ("old decodeAudioData scheduling removed", "decodeAudioData" not in text),
    ("native audio element removed", "new Audio()" not in text),
]
failed = False
for name, ok in checks:
    print(("PASS: " if ok else "FAIL: ") + name)
    failed = failed or not ok
if failed:
    print("Validation failed.")
else:
    print("Validation passed.")
sys.exit(1 if failed else 0)
PY
