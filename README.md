# Jarvis

Jarvis is a hobby voice robot for the M5Stack StackChan K151 (CoreS3, ESP32-S3). The robot talks to an [xAI](https://docs.x.ai/docs/guides/voice/agent) realtime voice session and, for anything that needs tools or another bot, to a Grok Bot webhook. A Cloudflare Worker sits between them. The xAI key never leaves the Worker.

This tree is meant to be published. It contains no real keys, account ids, webhook ids, or Wi-Fi names. Setup for your own accounts is `docs/setup.md`.

Our code is MIT. groki-bot firmware pieces are BSL-1.0, the zongzhu eye model is MIT, and the unused Emotion Ball eye data is non-commercial. See `THIRD_PARTY_NOTICES.md`.

## What is here

| Path | Role |
| --- | --- |
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
