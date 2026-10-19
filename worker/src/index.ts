import { isAllowedUrl, safeFetch } from "./allowlist";
import {
  base64Url,
  canonicalString,
  hexToBytes,
  hmacVerify,
  randomBytes,
  sha256,
  sha256Hex,
  skewOk,
  timingSafeEqual,
  timingSafeEqualText,
} from "./crypto";
import { DeviceHub } from "./device-hub";
import { limits, xaiModel, xaiVoice, type Env } from "./env";
import { logRequest, type LogFields } from "./log";

export { DeviceHub };

const MAX_BODY = 8192;
const encoder = new TextEncoder();

function bare(status: number): Response {
  return new Response(null, {
    status,
    headers: { "cache-control": "no-store" },
  });
}

function splitPath(pathname: string): { prefix: string; rest: string } {
  if (!pathname.startsWith("/")) return { prefix: "", rest: "" };
  const slash = pathname.indexOf("/", 1);
  if (slash === -1) return { prefix: pathname.slice(1), rest: "/" };
  return { prefix: pathname.slice(1, slash), rest: pathname.slice(slash) };
}

type RouteName = LogFields["route"];

function matchRoute(rest: string, method: string): RouteName | null {
  if (method === "POST" && rest === "/token") return "token";
  if (method === "POST" && rest === "/ask") return "ask";
  if (method === "POST" && rest === "/bot/reply") return "reply";
  if (method === "GET" && rest === "/ws") return "ws";
  return null;
}

function clientIp(request: Request): string {
  return request.headers.get("cf-connecting-ip") ?? "unknown";
}

async function readBody(request: Request): Promise<Uint8Array | "too-big"> {
  const declared = request.headers.get("content-length");
  if (declared && Number(declared) > MAX_BODY) return "too-big";
  const bytes = new Uint8Array(await request.arrayBuffer());
  if (bytes.byteLength > MAX_BODY) return "too-big";
  return bytes;
}

function hub(env: Env): DurableObjectStub {
  return env.DEVICE_HUB.get(env.DEVICE_HUB.idFromName(env.DEVICE_ID));
}

async function postHub(env: Env, path: string, body: unknown): Promise<Response> {
  return hub(env).fetch(`https://hub${path}`, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  });
}

interface AuthOk {
  ok: true;
}
interface AuthNo {
  ok: false;
}

async function authenticate(
  request: Request,
  env: Env,
  body: Uint8Array,
  pathname: string,
): Promise<AuthOk | AuthNo> {
  const headerToken = request.headers.get("authorization") ?? "";
  const token = headerToken.startsWith("Bearer ") ? headerToken.slice("Bearer ".length) : "";
  const deviceId = request.headers.get("x-jarvis-device-id") ?? "";
  const timestamp = request.headers.get("x-jarvis-timestamp") ?? "";
  const nonce = request.headers.get("x-jarvis-nonce") ?? "";
  const signatureHex = request.headers.get("x-jarvis-signature") ?? "";

  const timestampOk = /^[0-9]{10}$/.test(timestamp) && skewOk(Number(timestamp), Date.now());
  const nonceOk = /^[A-Za-z0-9_-]{16,128}$/.test(nonce);
  const presented = hexToBytes(signatureHex) ?? new Uint8Array(32);
  const bodyHash = await sha256Hex(body);
  const message = canonicalString(request.method, pathname, deviceId, timestamp, nonce, bodyHash);
  const signatureOk = await hmacVerify(env.DEVICE_HMAC_KEY, message, presented);

  const tokenBytes = encoder.encode(token.slice(0, 256));
  const tokenHash = await sha256(tokenBytes);
  const expectedHash = hexToBytes((env.DEVICE_TOKEN_HASH ?? "").trim().toLowerCase());
  const tokenOk = expectedHash !== null && timingSafeEqual(tokenHash, expectedHash);
  const deviceOk = timingSafeEqualText(deviceId, env.DEVICE_ID ?? "");

  if (!timestampOk || !nonceOk || !signatureOk || !tokenOk || !deviceOk) return { ok: false };

  const nonceResponse = await postHub(env, "/internal/nonce", { nonce });
  const nonceBody = (await nonceResponse.json()) as { ok?: boolean };
  if (!nonceBody.ok) return { ok: false };
  return { ok: true };
}

export async function rateLimit(env: Env, key: string): Promise<boolean> {
  const result = await env.RATE_LIMITER.limit({ key });
  return result.success;
}

function replyUrl(env: Env): string | null {
  const base = (env.PUBLIC_BASE_URL ?? "").replace(/\/$/, "");
  if (!base.startsWith("https://") || base.includes("?")) return null;
  let parsed: URL;
  try {
    parsed = new URL(base);
  } catch {
    return null;
  }
  if (parsed.username || parsed.password || parsed.pathname !== "/" && parsed.pathname !== "") return null;
  if (parsed.port !== "" && parsed.port !== "443") return null;
  return `${parsed.origin}/${env.PATH_PREFIX}/bot/reply`;
}

async function mintToken(env: Env, requestId: string): Promise<Response> {
  const cap = limits(env);
  const reservationId = crypto.randomUUID();
  const reserved = await postHub(env, "/internal/reserve", {
    ttlSec: cap.tokenTtl,
    dailyMax: cap.dailyMinutes,
    hourlyMax: cap.tokensPerHour,
    reservationId,
  });
  const reservation = (await reserved.json()) as { reason?: string };
  if (reservation.reason !== "ok") return bare(429);

  const voice = xaiVoice(env);
  const model = xaiModel(env);
  let upstream: Response;
  try {
    upstream = await safeFetch(
      "https://api.x.ai/v1/realtime/client_secrets",
      {
        method: "POST",
        headers: {
          authorization: `Bearer ${env.XAI_API_KEY}`,
          "content-type": "application/json",
        },
        body: JSON.stringify({ expires_after: { seconds: cap.tokenTtl } }),
      },
      env.JARVIS_WEBHOOK_URL,
    );
  } catch {
    await postHub(env, "/internal/release", { reservationId });
    return bare(502);
  }
  if (upstream.status === 502 || !upstream.ok) {
    await upstream.arrayBuffer().catch(() => undefined);
    await postHub(env, "/internal/release", { reservationId });
    return bare(502);
  }
  let payload: { value?: unknown; expires_at?: unknown };
  try {
    payload = (await upstream.json()) as { value?: unknown; expires_at?: unknown };
  } catch {
    await postHub(env, "/internal/release", { reservationId });
    return bare(502);
  }
  if (typeof payload.value !== "string" || payload.value.length === 0) {
    await postHub(env, "/internal/release", { reservationId });
    return bare(502);
  }
  const nowSec = Math.floor(Date.now() / 1000);
  let expiresAt = nowSec + cap.tokenTtl;
  if (typeof payload.expires_at === "number" && Number.isFinite(payload.expires_at)) {
    const upstreamExpiry = payload.expires_at > 10_000_000_000 ? Math.floor(payload.expires_at / 1000) : payload.expires_at;
    expiresAt = Math.min(expiresAt, upstreamExpiry);
  }
  if (expiresAt <= nowSec) {
    await postHub(env, "/internal/release", { reservationId });
    return bare(502);
  }
  return Response.json(
    {
      client_secret: payload.value,
      expires_at: expiresAt,
      expires_in: expiresAt - nowSec,
      model,
      voice,
      realtime_url: `wss://api.x.ai/v1/realtime?model=${model}`,
    },
    { headers: { "cache-control": "no-store", "x-request-id": requestId } },
  );
}

async function askBot(env: Env, body: Uint8Array, requestId: string): Promise<Response> {
  let parsed: { text?: unknown; interrupt?: unknown };
  try {
    parsed = JSON.parse(new TextDecoder().decode(body)) as { text?: unknown; interrupt?: unknown };
  } catch {
    return bare(400);
  }
  if (typeof parsed.text !== "string") return bare(400);
  const text = parsed.text.trim();
  const cap = limits(env);
  if (text.length === 0 || text.length > cap.maxChars) return bare(400);

  const callback = replyUrl(env);
  if (!callback || !isAllowedUrl(env.JARVIS_WEBHOOK_URL, env.JARVIS_WEBHOOK_URL)) return bare(502);

  const replyToken = base64Url(randomBytes(32));
  const tokenHash = await sha256Hex(encoder.encode(replyToken));
  const textHash = await sha256Hex(encoder.encode(text));
  const appRequestId = crypto.randomUUID();
  const expiresAt = Date.now() + cap.commandTimeout * 1000;
  const asked = await postHub(env, "/internal/ask", {
    textHash,
    requestId: appRequestId,
    tokenHash,
    windowMs: cap.duplicateWindow * 1000,
    interrupt: parsed.interrupt === true,
    expiresAt,
  });
  const askBody = (await asked.json()) as { ok?: boolean; reason?: string };
  if (!askBody.ok && askBody.reason === "duplicate") return bare(409);
  if (!askBody.ok) return bare(400);

  let webhook: Response;
  try {
    webhook = await safeFetch(
      env.JARVIS_WEBHOOK_URL,
      {
        method: "POST",
        headers: {
          authorization: `Bearer ${env.JARVIS_WEBHOOK_KEY}`,
          "content-type": "application/json",
        },
        body: JSON.stringify({
          text,
          request_id: appRequestId,
          reply_token: replyToken,
          reply_url: callback,
        }),
      },
      env.JARVIS_WEBHOOK_URL,
    );
  } catch {
    await postHub(env, "/internal/cancel", { requestId: appRequestId });
    return bare(502);
  }
  if (!webhook.ok) {
    await webhook.arrayBuffer().catch(() => undefined);
    await postHub(env, "/internal/cancel", { requestId: appRequestId });
    return bare(502);
  }
  await webhook.arrayBuffer().catch(() => undefined);
  return Response.json(
    { request_id: appRequestId, status: "pending" },
    { headers: { "cache-control": "no-store", "x-request-id": requestId } },
  );
}

async function botReply(env: Env, body: Uint8Array): Promise<Response> {
  let parsed: { request_id?: unknown; reply_token?: unknown; text?: unknown };
  try {
    parsed = JSON.parse(new TextDecoder().decode(body)) as {
      request_id?: unknown;
      reply_token?: unknown;
      text?: unknown;
    };
  } catch {
    return bare(401);
  }
  if (typeof parsed.request_id !== "string" || typeof parsed.reply_token !== "string" || typeof parsed.text !== "string") {
    return bare(401);
  }
  const text = parsed.text.trim();
  if (text.length === 0 || text.length > limits(env).maxChars) return bare(400);
  const tokenHash = await sha256Hex(encoder.encode(parsed.reply_token));
  const result = await postHub(env, "/internal/reply", {
    requestId: parsed.request_id,
    tokenHash,
    text,
  });
  const reply = (await result.json()) as { ok?: boolean };
  if (!reply.ok) return bare(401);
  return bare(204);
}

export default {
  async fetch(request: Request, env: Env): Promise<Response> {
    const started = Date.now();
    const requestId = crypto.randomUUID();
    let route: RouteName = "denied";
    let status = 404;
    try {
      const url = new URL(request.url);
      const { prefix, rest } = splitPath(url.pathname);
      const prefixOk =
        (env.PATH_PREFIX ?? "").length >= 8 && timingSafeEqualText(prefix, env.PATH_PREFIX);
      const matched = prefixOk ? matchRoute(rest, request.method) : null;
      if (!prefixOk || !matched) {
        status = 404;
        return bare(404);
      }
      route = matched;

      if (!(await rateLimit(env, `ip:${clientIp(request)}`))) {
        status = 429;
        return bare(429);
      }

      const body = matched === "ws" ? new Uint8Array() : await readBody(request);
      if (body === "too-big") {
        status = 400;
        return bare(400);
      }

      if (matched !== "reply") {
        const auth = await authenticate(request, env, body, url.pathname);
        if (!auth.ok) {
          status = 401;
          return bare(401);
        }
        if (!(await rateLimit(env, `dev:${env.DEVICE_ID}`))) {
          status = 429;
          return bare(429);
        }
      }

      if (matched === "ws") {
        if ((request.headers.get("upgrade") ?? "").toLowerCase() !== "websocket") {
          status = 426;
          return bare(426);
        }
        const upgraded = await hub(env).fetch(request);
        status = upgraded.status;
        return upgraded;
      }
      if (matched === "token") {
        const response = await mintToken(env, requestId);
        status = response.status;
        return response;
      }
      if (matched === "ask") {
        const response = await askBot(env, body, requestId);
        status = response.status;
        return response;
      }
      const response = await botReply(env, body);
      status = response.status;
      return response;
    } catch {
      status = 500;
      return bare(500);
    } finally {
      logRequest({ requestId, route, status, ms: Date.now() - started });
    }
  },
};
