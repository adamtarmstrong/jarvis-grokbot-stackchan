import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = dirname(fileURLToPath(import.meta.url));
const files = ["wrangler.jsonc", "wrangler.example.jsonc", ".dev.vars.example"];
const forbidden = [
  /account_id/i,
  /api[_-]?token/i,
  /\bsk-[a-z0-9]{8,}/i,
  /xai-[a-z0-9]/i,
  /AKIA[0-9A-Z]{16}/,
  /ghp_[a-zA-Z0-9]{20,}/,
  /api2\.cursor\.sh/i,
];

let failed = false;
for (const name of files) {
  const text = readFileSync(join(root, "..", name), "utf8");
  for (const pattern of forbidden) {
    if (pattern.test(text)) {
      console.error(`${name} matched ${pattern}`);
      failed = true;
    }
  }
  if (name.endsWith(".jsonc") && text.includes("YOUR_")) {
    console.error(`${name} still has a placeholder where a real config key would be a secret`);
    failed = true;
  }
}

const example = readFileSync(join(root, "..", ".dev.vars.example"), "utf8");
for (const key of [
  "XAI_API_KEY",
  "JARVIS_WEBHOOK_KEY",
  "JARVIS_WEBHOOK_URL",
  "DEVICE_TOKEN_HASH",
  "DEVICE_HMAC_KEY",
  "DEVICE_ID",
  "PATH_PREFIX",
  "PUBLIC_BASE_URL",
]) {
  if (!example.includes(`${key}=`)) {
    console.error(`.dev.vars.example is missing ${key}`);
    failed = true;
  }
}

if (failed) process.exit(1);
console.log("worker config has no secrets");
