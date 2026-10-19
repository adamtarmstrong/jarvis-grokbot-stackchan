# Contributing

Jarvis is a small hobby tree. Changes should stay easy to review.

## Secrets

Do not commit keys, webhook URLs, account ids, Worker hosts, Wi-Fi names, or `sdkconfig`. Templates use placeholders such as `YOUR_XAI_API_KEY`. `worker/scripts/check-config.mjs` rejects obvious live patterns in the Wrangler templates.

Install gitleaks and point Git at the hooks in this repo:

```sh
git config core.hooksPath .githooks
```

The pre-commit hook refuses the commit if `gitleaks` is not on `PATH`. Releases are published at https://github.com/gitleaks/gitleaks/releases . The hook runs:

```sh
gitleaks protect --staged --redact --config .gitleaks.toml
```

## Checks

Worker:

```sh
cd worker
npm ci
npm test
npx tsc --noEmit
```

Host logic (no board):

```sh
firmware/host/test.sh
```

Firmware, when Docker is available:

```sh
docker run --rm -v "$PWD":/project -w /project/firmware espressif/idf:v5.5.4 \
  bash -lc 'idf.py set-target esp32s3 && idf.py build && python3 tools/check_static_ram.py --board cores3 build/jarvis.map'
```

## Licenses

New code in this repository is MIT. Do not relicense a groki-bot file (BSL-1.0) or the zongzhu eye model (MIT, keep the copyright). Do not add `firmware/third_party/emotion-ball/` to a `CMakeLists.txt`. The default face stays the MIT eye model.

Do not add the groki-bot login-voice code or a `gbot` client.

## Commits

Use a conventional prefix: `feat`, `fix`, `chore`, `docs`, `refactor`, or `test`.
