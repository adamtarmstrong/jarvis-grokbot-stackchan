# Jarvis Bridge — Home Assistant OS add-on (experimental)

A Pi 5 voice service for StackChan. Microphone and speaker stay on the robot;
the Home Assistant box can be in a closet. No Mac, iOS app, cloud speech engine,
or xAI realtime session is needed. Speech recognition is faster-whisper CPU int8
(`base.en`, two CPU threads); speech output is local Piper.

**First-slice status:** local protocol tests and actual Alan -> Whisper smoke
passed on the development box; ESP32 firmware builds. Not yet installed on HA OS
or tested on a physical robot. The repository has NOT been published/pushed by
this change, so GitHub installation becomes available only after a reviewed release.

## Install after the repo is published

1. Home Assistant OS: **Settings → Add-ons → Add-on store → ⋮ → Repositories**.
   Paste the project's GitHub repository URL (not a HACS URL or a subfolder URL).
   Supervisor discovers `homeassistant/jarvis-bridge/config.yaml` in the repo.
2. Install **Jarvis Bridge**. Supervisor builds the image on the Pi; first install
   and first model load need internet and may take several minutes. Enable start
   on boot. No privileged mode, host audio, or HA administrator token is required.
3. In Configuration set a random `bridge_key` of at least 16 characters. Copy it
   to the robot's setup page later. Never post keys or put them in GitHub.
4. Recommended **existing Worker relay** mode:
   - `worker_url`: `https://YOUR-WORKER.example/YOUR_PREFIX` (includes the prefix).
   - `device_id`, `device_token`, `hmac_key`: the existing matching device identity.
   - Leave `webhook_url`, `webhook_key`, `public_reply_url` empty. The Worker
     already holds the webhook settings securely. No credentials are auto-imported.
   - The add-on signs `/ask` and `/ws` using the existing JARVIS1 HMAC scheme.
     The Worker supplies `reply_token` and its existing `/bot/reply` URL to Jarvis.
     Replies return over the outbound Worker WebSocket. No inbound WAN port/tunnel,
     Worker deployment, token mint, or increased Worker cap is needed.
   - This is the same one-device identity. Do NOT run another device Worker client
     concurrently: the Worker permits one socket and replaces an older connection.
     In HA mode the firmware does not start its legacy Worker socket.
5. `reply_timeout` defaults to 40 seconds. The *entire robot turn* has a hard
   120-second limit including connect, speech input, STT, bot wait, TTS and playback.
   Timeouts name the cause in the log (`reply_wait_timeout` vs `turn_budget_timeout`).
   Slow/long bot tasks and delayed announcements are not handled by this first slice.
   Ask Jarvis for concise answers. Each finished turn also logs a WARNING timing line
   (`listen` / `stt` / `ask` / `jarvis_wait` / `tts_first_audio` / `total`) with no
   transcript text.
6. Keep `bind_port: 10700` (the internal container port). In the add-on's Network
   section map `10700/tcp` to the LAN host port you want, default 10700. Do not
   forward this port on your router. Restrict it to the robot/trusted LAN.
7. Start. Look for **Local speech pipeline ready on port 10700** in the log.
   Alan and Whisper are downloaded once to `/data/models`, then reused.
   `/health` reports readiness only after the models have loaded.
8. Later, after the updated firmware is deliberately flashed, hold the robot's
   screen during startup to reopen setup (hold for two seconds within the
   eight-second "Hold for setup" window after the display comes up; panel or
   Si12T touch both count). Join `Jarvis-Setup`, open
   `http://192.168.44.1/`, and enter Wi-Fi, bridge URL
   `ws://PI-LAN-IP:10700/voice`, and the matching bridge key. A reserved Pi IP is
   more reliable than mDNS. Legacy Worker fields may be empty in this mode.
   Save/reboot. Setup re-entry and the firmware path still require hardware testing.
   This task does not flash anything or modify HA itself.

## Voices

Public default: official **`en_GB-alan-medium`**. We download only that voice's
`.onnx`, `.onnx.json` and `MODEL_CARD` from the official `rhasspy/piper-voices`
repository on first start. No weights are stored in Git or the container image.
There is no paid voice option and no voice is sold. See THIRD_PARTY_NOTICES.md:
upstream's model card points to ambiguous legacy dataset licensing; do not claim
unrestricted redistribution or bundle weights without resolving it.

For a voice you already have permission to use privately:

1. Use your existing HA file-sharing tooling to put both files in `/share/piper`:
   for example `private-high.onnx` and `private-high.onnx.json`.
2. Set `custom_voice: private-high` (filename stem only; no `.onnx`, paths or URLs).
3. Restart the add-on. The share mount is read-only; it never writes, downloads
   or publishes custom models. A missing/invalid custom voice fails startup rather
   than silently selecting a different voice.
4. Clear `custom_voice` and restart to return to Alan.

Private fan-made voice files remain on the operator's HA box. They are not
included in the public repository, container image, tests, or release artifacts.
This project provides no links for obtaining those weights.

## Optional direct webhook mode (not needed for the current Worker setup)

Clear `worker_url`. Configure `webhook_url`, `webhook_key`, and `public_reply_url`.
The webhook receives JSON `{text, request_id, reply_token, reply_url}` with Bearer
key authentication, matching the existing Worker payload. Jarvis must POST
`{request_id, reply_token, text}` to `reply_url`. The token is random, stored only
as a hash, one-use, request-bound, and expires on timeout/disconnect/restart.

This mode needs an operator-provided **HTTPS callback route** to the add-on's
`POST /bot/reply`, reachable by the bot. Expose ONLY that route via your secure
reverse proxy/tunnel, NOT `/voice` or all of Home Assistant. A LAN URL is not a
usable cloud callback. No tunnel is provisioned by this slice. Changing Jarvis's
callback instructions is not required in Worker mode; direct mode must respect
the payload's `reply_url` rather than a hardcoded old Worker URL.

## Wire protocol v1

One persistent WebSocket at `ws://PI-IP:HOST-PORT/voice` (reused across turns; older firmware that closes after a turn still works), pairing header
`X-Jarvis-Bridge-Key` (never put the key in the URL). Trusted LAN only: this first
slice uses plain WebSocket, NOT encrypted transport. Do not expose it to WAN.
Frames are small JSON text messages so ESP-IDF can reuse its existing decoder.

| Direction | Message |
|---|---|
| Robot → Pi | `{"type":"start_listen","sample_rate":16000,"channels":1,"format":"s16le"}` |
| Pi → robot | `{"type":"ready","sample_rate":16000,"channels":1,"format":"s16le"}` |
| Robot → Pi | `{"type":"audio","audio":"BASE64_PCM"}` (normally 800 samples / 50ms; max 1600 samples / 100ms) |
| Robot → Pi | `{"type":"end_listen"}` on touch release, or omit for automatic pause detection |
| Pi → robot | `{"type":"processing"}` (stop mic upload; thinking/busy) |
| Pi → robot | `{"type":"play","sample_rate":16000,"channels":1,"format":"s16le"}` |
| Pi → robot | `{"type":"response.output_audio.delta","delta":"BASE64_PCM"}` repeatedly |
| Pi → robot | `{"type":"response.done"}` (all PCM sent, not necessarily played yet) |
| Robot → Pi | `{"type":"played"}` after the speaker drains; relay acknowledges played to Worker |
| Pi → robot | `{"type":"error","code":"turn_failed"}` (ends the turn; the socket stays open if it is still healthy) |
| Robot → Pi | `{"type":"usage_request"}` (idle only; asks for the latest cached usage snapshot) |
| Pi → robot | `{"type":"usage_snapshot",...}` on connect, after `usage_request`, and when a new snapshot arrives while idle. `waiting: true` if none is cached yet. |

Both ways PCM is signed little-endian int16 mono at 16kHz, no WAV header.
Piper output is resampled to 16kHz. Output is bounded to 45s and sent in paced
100ms chunks. One active turn at a time (409 busy only while a turn is in progress; an idle
socket is replaced if the robot reconnects). 401 for a wrong pairing key,
64KiB max JSON frames (usage snapshots need the extra room; audio frames stay
small), 20s max capture and a 120s total turn budget. Idle sockets
are held for up to 10 minutes of silence and accept WebSocket pings. There is no
barge-in, automatic xAI fallback, or raw transcript/audio logging/storage.
Input endpointing uses 900ms below a fixed energy threshold after speech;
Whisper also runs Silero VAD. Microphone threshold needs desk testing.

**Visual cues, no beeps on the HA path:** capture starts at wake and buffers in
PSRAM while LAN connects. The top-left green microphone means speak now; an amber
clock means thinking/waiting/speaking. The existing top-right privacy dots and
2px white bar at y=0 remain; the bar turns amber in its last 15 seconds and clears
when the turn ends. The Grok eyes/squint/expressions and lip playback are retained.
The legacy xAI path is still present if the bridge URL is left empty; it is not
used or silently retried when the bridge URL is set.

## Usage snapshots and Home Assistant sensors

The add-on does not scrape Grok Bot usage itself. A routine on the owner's
desktop posts a `stackchan.usage.v1` snapshot to the existing Worker
(`POST /{prefix}/usage`). This add-on polls `GET /{prefix}/usage` on startup
and once an hour, using the same device HMAC credentials as `/ask`. A
successful snapshot is:

- appended to `/data/usage/snapshots.jsonl` (90-day retention; identical
  `fetched_at` values are not duplicated)
- published to Home Assistant as sensors, so the recorder keeps history
- sent to the robot over the persistent `/voice` socket

`homeassistant_api: true` is required so the add-on can `POST` to
`/core/api/states/...` with the Supervisor token. If that token is missing
(for example in unit tests), sensor publish is skipped and the JSONL + robot
path still work. Snapshots marked `_example: true` are ignored.

Sensors (attributes include `fetched_at`; bot sensors also include `name`,
`color`, and `shape`):

| Entity | State |
|---|---|
| `sensor.grokbot_usage_used_pct` | Weekly percent used |
| `sensor.grokbot_usage_resets_at` | Reset timestamp |
| `sensor.grokbot_usage_on_demand` | `off`, `available`, or `active` |
| `sensor.grokbot_usage_bot_<id>` | That bot's share of the week |
| `sensor.grokbot_usage_routine_<bot>_<name>` | A listed routine's share of that bot |

## Settings and remaining work

Robot setup stores follow enabled, weekdays (1=Monday through 7=Sunday), start/end
(default every day 08:00–17:00 America/Detroit) and color. These are clearly labeled
**future hooks**, not functioning tracking/theme controls in this slice. Camera
support is currently off; tracking, no-movement enforcement after hours, and the
sleep schedule still need implementation. Existing random idle head glances are
not changed by this voice slice. Do not claim it already follows a person or
sleeps after 17:00. Voice must remain usable after hours when that work lands.
No iOS app is planned. Asynchronous bot announcements and offline/late reply
recovery through local Piper need a follow-up slice.

## Changelog

### 0.1.5
- Poll the Worker `GET /usage` endpoint hourly and on startup, store every
  snapshot in `/data/usage/snapshots.jsonl` (90-day retention), and publish
  Home Assistant usage sensors (`sensor.grokbot_usage_*`) including per-bot
  color and shape attributes.
- Send `usage_snapshot` to the robot on `/voice` connect, on `usage_request`,
  and when a new snapshot arrives while the socket is idle. A missing cache
  is advertised as `waiting: true`. Older firmware that ignores the message
  keeps working.

### 0.1.4
- Keep the `/voice` WebSocket open after `played`, `response.done`, or a
  per-turn error, and wait for the next `start_listen` on the same socket.
- The busy lock is held only during an active turn, so an idle connection never
  409s a real wake. A new connection while an older socket is idle is accepted
  and the stale socket is closed (robot reconnect after a Wi-Fi drop).
- Heartbeats stay on while idle; idle sockets time out after 10 minutes of
  silence and close cleanly when the client disconnects.
- Older firmware that closes after each turn remains compatible.

### 0.1.3
- Raise the whole-turn budget from 60 s to 120 s so a long listen plus
  `reply_timeout` no longer aborts while Jarvis is still answering.
- Distinguish `reply_wait_timeout` from `turn_budget_timeout` in the turn-ended log.
- Log one WARNING `turn timing` line per turn (listen/stt/ask/jarvis_wait/tts_first_audio/total).

### 0.1.2
- Fix log redaction inserting `***` between every character when an empty secret
  (common in Worker mode where `webhook_key` is unused) was passed to `str.replace`.
- Worker `/ask` rejections now log HTTP status and a short reason class
  (`auth_failed`, `upstream_failed`, `rate_limited`, etc.) without bodies or tokens.

### 0.1.1
- Initial experimental add-on slice (STT, Piper Alan, Worker relay / direct webhook).

## Development checks

```sh
python3 -m venv /tmp/jarvis-test
/tmp/jarvis-test/bin/pip install -r requirements.txt
/tmp/jarvis-test/bin/python -m unittest discover -s tests -v
# Optional: downloads only official Alan + Whisper tiny.en, writes generated test audio outside git:
/tmp/jarvis-test/bin/python tests/smoke_speech.py /tmp/jarvis-models
```

Dependencies have CPU aarch64 wheels; Docker uses Python 3.11 Debian Bookworm.
No Docker daemon was available for development verification; Supervisor/Pi build,
startup, resource/latency checks and physical robot testing remain release gates.
First install needs internet, subsequent speech is local except the Jarvis text
webhook request. Bot webhook runs still use the owner's normal bot allowance.
