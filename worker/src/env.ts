export interface RateLimiter {
  limit(options: { key: string }): Promise<{ success: boolean }>;
}

export interface Env {
  DEVICE_HUB: DurableObjectNamespace;
  RATE_LIMITER: RateLimiter;
  XAI_API_KEY: string;
  JARVIS_WEBHOOK_KEY: string;
  JARVIS_WEBHOOK_URL: string;
  DEVICE_TOKEN_HASH: string;
  DEVICE_HMAC_KEY: string;
  DEVICE_ID: string;
  PATH_PREFIX: string;
  PUBLIC_BASE_URL: string;
  MAX_SESSION_MINUTES_PER_DAY: string;
  MAX_TOKENS_PER_HOUR: string;
  TOKEN_TTL_SECONDS: string;
  COMMAND_TIMEOUT_SECONDS: string;
  MAX_COMMAND_CHARS: string;
  DUPLICATE_WINDOW_SECONDS: string;
  XAI_VOICE: string;
  XAI_MODEL: string;
}

export function intVar(raw: string | undefined, fallback: number, min: number, max: number): number {
  const n = Number(raw);
  if (!Number.isInteger(n)) return fallback;
  if (n < min) return min;
  if (n > max) return max;
  return n;
}

/** Ephemeral xAI secrets live 5–10 minutes. Longer values are clamped. */
export function tokenTtlSeconds(env: Env): number {
  return intVar(env.TOKEN_TTL_SECONDS, 600, 300, 600);
}

export function limits(env: Env) {
  return {
    dailyMinutes: intVar(env.MAX_SESSION_MINUTES_PER_DAY, 60, 1, 24 * 60),
    tokensPerHour: intVar(env.MAX_TOKENS_PER_HOUR, 6, 1, 60),
    tokenTtl: tokenTtlSeconds(env),
    commandTimeout: intVar(env.COMMAND_TIMEOUT_SECONDS, 300, 30, 300),
    maxChars: intVar(env.MAX_COMMAND_CHARS, 2000, 1, 2000),
    duplicateWindow: intVar(env.DUPLICATE_WINDOW_SECONDS, 30, 1, 300),
  };
}

/** Voice defaults to leo. altair is the only other voice this bridge will ask for. */
export function xaiVoice(env: Env): "leo" | "altair" {
  return env.XAI_VOICE === "altair" ? "altair" : "leo";
}

export function xaiModel(_env: Env): "grok-voice-3.0" {
  return "grok-voice-3.0";
}
