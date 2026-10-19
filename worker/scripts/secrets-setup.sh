#!/bin/sh
# Put Worker secrets. Prints names only. Writes worker/secrets.generated mode 0600.
set -eu
cd "$(dirname "$0")/.."
umask 077

if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 is required" >&2
  exit 1
fi
if ! command -v npx >/dev/null 2>&1; then
  echo "npx is required (npm install in worker/)" >&2
  exit 1
fi

python3 - <<'PY'
import hashlib, os, secrets
from pathlib import Path

def generated(name, value):
    print(f"generated {name}")
    return value

env = dict(os.environ)
if not env.get("DEVICE_TOKEN"):
    env["DEVICE_TOKEN"] = generated("DEVICE_TOKEN", secrets.token_urlsafe(32))
if not env.get("DEVICE_HMAC_KEY"):
    env["DEVICE_HMAC_KEY"] = generated("DEVICE_HMAC_KEY", secrets.token_urlsafe(32))
if not env.get("PATH_PREFIX"):
    env["PATH_PREFIX"] = generated("PATH_PREFIX", secrets.token_hex(16))
if not env.get("DEVICE_ID"):
    env["DEVICE_ID"] = generated("DEVICE_ID", "jarvis-" + secrets.token_hex(4))

required = [
    "XAI_API_KEY",
    "JARVIS_WEBHOOK_KEY",
    "JARVIS_WEBHOOK_URL",
    "DEVICE_TOKEN",
    "DEVICE_HMAC_KEY",
    "DEVICE_ID",
    "PATH_PREFIX",
    "PUBLIC_BASE_URL",
]
for name in required:
    value = env.get(name, "")
    if not value or "YOUR_" in value or "example" in value.lower() or "\n" in value:
        raise SystemExit(f"{name} is missing or still a placeholder")

url = env["JARVIS_WEBHOOK_URL"]
if not url.startswith("https://") or "@" in url:
    raise SystemExit("JARVIS_WEBHOOK_URL must be https with no userinfo")
base = env["PUBLIC_BASE_URL"].rstrip("/")
if not base.startswith("https://") or "@" in base or "/" in base[8:]:
    raise SystemExit("PUBLIC_BASE_URL must be an https origin with no path")
prefix = env["PATH_PREFIX"]
if len(prefix) < 8 or "/" in prefix:
    raise SystemExit("PATH_PREFIX must be at least 8 characters and contain no slash")

token_hash = hashlib.sha256(env["DEVICE_TOKEN"].encode()).hexdigest()
lines = [
    f"XAI_API_KEY={env['XAI_API_KEY']}",
    f"JARVIS_WEBHOOK_KEY={env['JARVIS_WEBHOOK_KEY']}",
    f"JARVIS_WEBHOOK_URL={env['JARVIS_WEBHOOK_URL']}",
    f"DEVICE_TOKEN={env['DEVICE_TOKEN']}",
    f"DEVICE_TOKEN_HASH={token_hash}",
    f"DEVICE_HMAC_KEY={env['DEVICE_HMAC_KEY']}",
    f"DEVICE_ID={env['DEVICE_ID']}",
    f"PATH_PREFIX={prefix}",
    f"PUBLIC_BASE_URL={base}",
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

put XAI_API_KEY
put JARVIS_WEBHOOK_KEY
put JARVIS_WEBHOOK_URL
put DEVICE_TOKEN_HASH
put DEVICE_HMAC_KEY
put DEVICE_ID
put PATH_PREFIX
put PUBLIC_BASE_URL
echo "Next: npx wrangler deploy"
echo "Copy DEVICE_TOKEN from secrets.generated into the robot. Do not commit that file."
