# Current voice architecture


## Architecture revision — Nov 8, 2026, America/Detroit (current decision)

This dated section supersedes the realtime voice and iOS-app proposals above;
older sections are retained as history, not the current build specification.

- Robot: keep WakeNet Hey Jarvis, Grok eyes, squint/expressions, privacy dots,
  speaker/lip playback, and 2px white 60-second bar at y=0 (amber final 15s).
- Updated cue requirement: **no single/double-beep waiting sequence** on this
  path. Speak immediately after wake. Robot starts capture into a bounded PSRAM
  buffer while LAN connects. A top-left microphone icon means ready/capturing;
  an amber clock means busy/thinking/waiting/speaking. Neither overlaps the
  top-right privacy dots, eyes, or session bar. No barge-in.
- Home Assistant OS + Supervisor on Raspberry Pi 5: a custom add-on, installed
  from a GitHub repository URL in the add-on store (not HACS). The robot and Pi
  communicate over LAN Wi-Fi/Ethernet; the Pi may be in a closet. No Mac needed.
- Add-on: 16kHz mono PCM -> local faster-whisper CPU int8 -> Jarvis text webhook
  -> local Piper -> resampled 16kHz PCM streamed back to robot.
- Reuse existing Worker via signed `/ask` + `/ws`; its webhook payload remains
  `{text, request_id, reply_token, reply_url}` and its `/bot/reply` token checking
  stays unchanged. The Pi owns the one-device socket in this mode; the firmware
  skips its legacy Worker connection. No live xAI session, token mint or cap change.
  An optional direct-webhook mode needs an operator-provided HTTPS `/bot/reply`
  callback; it is not required for the recommended existing-Worker route.
- Public voice: official Piper `en_GB-alan-medium`, downloaded on first add-on
  start with its upstream model card; no large weights in git. Upstream's model
  card delegates licensing to a legacy dataset page with an ambiguous rights
  notice: clarify before redistributing bundled weights. Do not claim MIT weights.
- Private hobby voice: user-supplied `.onnx` + `.onnx.json` in `/share/piper`,
  selected by a filename stem option. No fan-made models downloaded, linked,
  vendored, packaged, published, sold or charged for by the project.
- No paid voice upgrade plan and **no iOS app**. Robot setup page stores bridge
  URL/key, follow toggle, days, start/end hours, and color. Default intended
  schedule: every day 08:00–17:00 America/Detroit, DST aware; after hours sleeping
  eyes and **no hunting, tracking or head movements**. Voice still works after
  hours. This supersedes the earlier camera-activity exception outside hours.

### First local slice and release gates

Implementation: `homeassistant/jarvis-bridge`, root `repository.yaml`, firmware
HA URL/NVS/portal transport selection, immediate audio buffer and corner icons.
Protocol and installation: `docs/home-assistant-voice.md` and add-on README.
Models preload before accepting connections; one 60-second turn cap includes
input, STT, bot wait, TTS and playback. Long asynchronous announcements/late reply
recovery need a later slice. HA failures never silently fall back to paid xAI.

Camera remains off in the current firmware; follow/schedule/color fields are
persisted hooks only, not active controls. The existing random idle glances and
awake idle face remain until the separate motion/sleep implementation. Do not
claim camera follow or after-hours sleep is already implemented.

This work is local only: no cloud agent, push, HA deployment, robot flash, real
webhook call, or paid speech test. Full Pi/Supervisor container installation and
physical mic/playback, icons, endpoint threshold, timing and memory tests remain
necessary before calling this hardware-working. The public repo URL must exist
before the documented add-on-store install can be used.
