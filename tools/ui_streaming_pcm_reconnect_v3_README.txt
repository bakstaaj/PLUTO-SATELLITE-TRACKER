# UI Streaming PCM Reconnect v3

This patch fixes the UI message:

`Backend Audio Stream Ended`

The backend route is valid, but the HTTP stream can close after a segment. This UI patch treats that as reconnectable while the user has Analog Audio enabled.

## Behavior

- Start backend DSP once.
- Fetch `/api/radio/audio/live.wav?stream=1&downlink_hz=...`.
- Skip the 44-byte WAV header.
- Convert PCM16 to WebAudio buffers.
- When the stream segment closes, reopen it automatically.
- Stop button aborts fetch, stops WebAudio sources, closes AudioContext, and calls backend stop.

## Run

```bash
python tools/apply_ui_streaming_pcm_reconnect_v3.py .
./tools/validate_ui_streaming_pcm_reconnect_v3.sh .
./tools/deploy_and_reboot.sh
```
