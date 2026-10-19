# Security

Jarvis is a hobby robot. The Worker is the only place that holds the xAI key, and it should accept two callers: one robot, and the Jarvis bot answering a one-time token.

## Report a problem

Open a private security advisory on the GitHub repository if you have one, or contact the repository owner out of band. Do not open a public issue that includes a token, a webhook URL, a Worker host, or a Wi-Fi name.

## What the Worker does

- Device routes sit behind a secret path prefix of at least 8 characters.
- The device token is stored as a SHA-256 hash and compared in constant time.
- Requests are HMAC-SHA256 over `JARVIS1`, method, path, device id, timestamp, nonce, and the hex SHA-256 of the body. Clock skew over 60 seconds is rejected. Nonces are remembered in the Durable Object.
- The WebSocket is accepted only after that check. One device id owns the Durable Object. A new socket from that id closes the previous one with code 4000.
- `POST /bot/reply` burns a one-time `reply_token` (32 random bytes, stored as a hash, 5 minute TTL, tied to `request_id`).
- Outgoing fetches are allowed only to `api.x.ai` and the hostname of `JARVIS_WEBHOOK_URL`, HTTPS, port 443, no redirects followed.
- A wrong path is an empty 404. A failed device or reply authentication is an empty 401. Caps return an empty 429.
- Logs are a request id, a route name, a status, and a duration.

## What you turn on outside this repo

These cannot be set from a file in the clone:

- GitHub secret scanning and push protection, in the repository settings.
- Cloudflare 2FA with a passkey or hardware key, on a separate Jarvis account, with no extra members.
- A custom API token with an expiry. Scopes used here: Workers Scripts Edit, Workers KV Storage Edit, Account Settings Read. Add a Durable Objects permission only if deploy names one. Do not use the Global API Key.
- An xAI monthly spending limit and a key that is used only for this robot.

## Rotation

`worker/scripts/rotate-all.sh --yes` is the panic button. It replaces the device token, HMAC key, path prefix, and device id, and it writes unusable values over the xAI key and the webhook key so the Worker stops presenting the old ones. Revoke the real xAI key in the xAI console and rotate the webhook key in the bot panel, then run `secrets-setup.sh` with the replacements. The robot must be set up again. `--dry-run` prints names only.

## Scanning in this repo

- `.githooks/pre-commit` runs `gitleaks protect --staged`. Install gitleaks and set `git config core.hooksPath .githooks`.
- `.github/workflows/gitleaks.yml` runs gitleaks on every push and pull request, with full history.
- Before a public push, also run `gitleaks detect --source . --log-opts=--all` from a fresh clone.

If anything secret was ever committed, start the public history over. Do not rely on a later delete.
