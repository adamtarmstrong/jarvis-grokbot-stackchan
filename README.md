# Jarvis

Jarvis is a hobby voice robot for the M5Stack StackChan K151 (CoreS3, ESP32-S3).

**Current direction (Nov 8, 2026):** a [Home Assistant OS custom add-on](homeassistant/jarvis-bridge/README.md)
runs local Whisper speech recognition and Piper Alan TTS, with the existing
Cloudflare Worker routing text to the Jarvis Grok Bot webhook. Robot wake capture
is immediate, with corner ready/busy icons instead of beeps. See
[installation and status](docs/home-assistant-voice.md). This is an experimental
local slice, not a deployed or hardware-validated release.

The older xAI realtime path remains in the tree for compatibility when no HA
bridge URL is configured; its original setup documentation below is historical.
When HA is configured, it never mints an xAI token or falls back to that path.

This tree is meant to be published. It contains no real keys, account ids, webhook ids, or Wi-Fi names. Setup for your own accounts is `docs/setup.md`.

Our firmware and Worker code are MIT; the separate Home Assistant bridge is GPL-3.0-or-later. groki-bot firmware pieces are BSL-1.0, the zongzhu eye model is MIT, and the unused Emotion Ball eye data is non-commercial. See `THIRD_PARTY_NOTICES.md`.

## What is here

| Path | Role |
| --- | --- |
| `homeassistant/jarvis-bridge/` | Custom add-on: local STT, webhook relay, Piper, LAN WebSocket. |
| `repository.yaml` | Supervisor custom repository metadata. |
| `worker/` | Cloudflare Worker and Durable Object. Device HMAC, one-time reply tokens, xAI client secrets, caps. |
| `bot/WEBHOOK_ROUTINE.md` | Text to paste into the Jarvis Grok Bot webhook routine. |
| `docs/setup.md` | Ten-step setup. |
| `firmware/` | ESP-IDF 5.5.4 project. Not flashed in this environment. |

## Worker

```sh
cd worker
npm ci
npm test
npx tsc --noEmit
```

Tests use Miniflare. They do not call xAI or a webhook. The xAI and webhook fetches are mocked.

Deploy only after you have created your own secrets:

```sh
npx wrangler login
./scripts/secrets-setup.sh
npx wrangler deploy
```

Secret names and the panic script are in `docs/setup.md`. `wrangler.example.jsonc` matches `wrangler.jsonc` and has no account id.

## Firmware

Hardware paths are marked untested in `firmware/docs/UNTESTED.md`. A host check that does not need a board:

```sh
firmware/host/test.sh
```

The ESP-IDF build, when Docker is available:

```sh
docker run --rm -v "$PWD":/project -w /project/firmware espressif/idf:v5.5.4 \
  bash -lc 'idf.py set-target esp32s3 && idf.py build && python3 tools/check_static_ram.py --board cores3 build/jarvis.map'
```

The authoring environment had no Docker daemon, so that image was not run here. The same command is `.github/workflows/firmware.yml`.

## Secret scanning

```sh
git config core.hooksPath .githooks
gitleaks detect --source . --redact --config .gitleaks.toml
```

GitHub secret scanning and push protection are settings on the repository, not files in this tree.
