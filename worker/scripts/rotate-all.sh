#!/bin/sh
# Panic button. Rotates device auth and disables the xAI and webhook keys in the Worker.
# Prints names only. Does not revoke the xAI key in the xAI console; you still do that.
set -eu
cd "$(dirname "$0")/.."

if [ "${1:-}" = "--dry-run" ]; then
  echo "would overwrite: DEVICE_TOKEN_HASH DEVICE_HMAC_KEY PATH_PREFIX DEVICE_ID XAI_API_KEY JARVIS_WEBHOOK_KEY"
  echo "would leave: JARVIS_WEBHOOK_URL PUBLIC_BASE_URL"
  echo "would write: secrets.generated (mode 0600, values not shown)"
  echo "the robot would need setup again"
  exit 0
fi

if [ "${1:-}" != "--yes" ]; then
  echo "usage: ./scripts/rotate-all.sh --yes | --dry-run" >&2
  exit 1
fi

umask 077
python3 - <<'PY'
import os, secrets
from pathlib import Path
token = secrets.token_urlsafe(32)
hmac_key = secrets.token_urlsafe(32)
prefix = secrets.token_hex(16)
device_id = "jarvis-" + secrets.token_hex(4)
# Values that will not authenticate. Replace them from the consoles, then rerun secrets-setup.sh.
disabled = "disabled-" + secrets.token_hex(16)
lines = [
    f"DEVICE_TOKEN={token}",
    "DEVICE_TOKEN_HASH=" + __import__("hashlib").sha256(token.encode()).hexdigest(),
    f"DEVICE_HMAC_KEY={hmac_key}",
    f"PATH_PREFIX={prefix}",
    f"DEVICE_ID={device_id}",
    f"XAI_API_KEY={disabled}",
    f"JARVIS_WEBHOOK_KEY={disabled}",
]
Path("secrets.generated").write_text("\n".join(lines) + "\n")
os.chmod("secrets.generated", 0o600)
print("wrote secrets.generated (mode 0600). Values were not printed.")
PY

put() {
  name="$1"
  python3 - "$name" <<'PY' | npx wrangler secret put "$name"
import sys
from pathlib import Path
want = sys.argv[1]
for line in Path("secrets.generated").read_text().splitlines():
    key, _, value = line.partition("=")
    if key == want:
        sys.stdout.write(value)
        break
else:
    raise SystemExit(f"missing {want}")
PY
  echo "put $name"
}

put DEVICE_TOKEN_HASH
put DEVICE_HMAC_KEY
put PATH_PREFIX
put DEVICE_ID
put XAI_API_KEY
put JARVIS_WEBHOOK_KEY

cat <<'EOF'
Rotated device auth. The robot will fail closed until you enter the new token, HMAC key, prefix, and id from secrets.generated.
XAI_API_KEY and JARVIS_WEBHOOK_KEY in the Worker are now unusable values.
Revoke the old xAI key in the xAI console, rotate the webhook key in the bot panel, then run ./scripts/secrets-setup.sh with the new values.
JARVIS_WEBHOOK_URL and PUBLIC_BASE_URL were not changed.
EOF
