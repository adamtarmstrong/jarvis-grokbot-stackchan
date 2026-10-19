import { createHash } from "node:crypto";
import { defineWorkersConfig } from "@cloudflare/vitest-pool-workers/config";

const deviceTokenHash = createHash("sha256").update("test-device-token").digest("hex");

export default defineWorkersConfig({
  test: {
    fileParallelism: false,
    poolOptions: {
      workers: {
        wrangler: { configPath: "./wrangler.jsonc" },
        miniflare: {
          bindings: {
            XAI_API_KEY: "test-xai-key",
            JARVIS_WEBHOOK_KEY: "test-webhook-key",
            JARVIS_WEBHOOK_URL: "https://webhook.example/hook/test",
            DEVICE_TOKEN_HASH: deviceTokenHash,
            DEVICE_HMAC_KEY: "test-hmac-key",
            DEVICE_ID: "device-1",
            PATH_PREFIX: "testprefix",
            PUBLIC_BASE_URL: "https://jarvis.example",
            MAX_SESSION_MINUTES_PER_DAY: "60",
            MAX_TOKENS_PER_HOUR: "6",
            TOKEN_TTL_SECONDS: "600",
            COMMAND_TIMEOUT_SECONDS: "300",
            MAX_COMMAND_CHARS: "2000",
            DUPLICATE_WINDOW_SECONDS: "30",
            XAI_VOICE: "leo",
            XAI_MODEL: "grok-voice-3.0",
          },
        },
      },
    },
  },
});
