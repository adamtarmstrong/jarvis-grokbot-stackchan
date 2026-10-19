import { env, fetchMock, SELF } from "cloudflare:test";
import { bytesToHex, canonicalString, hmacSign, sha256Hex } from "../src/crypto";
import { keys, type RequestRecord } from "../src/state";

export const prefix = "/testprefix";

export function origin(path: string): string {
  return `http://example.com${path}`;
}

export async function signHeaders(
  method: string,
  path: string,
  body = "",
  over: Partial<{ token: string; deviceId: string; timestamp: string; nonce: string; signature: string }> = {},
): Promise<Headers> {
  const token = over.token ?? "test-device-token";
  const deviceId = over.deviceId ?? "device-1";
  const timestamp = over.timestamp ?? String(Math.floor(Date.now() / 1000));
  const nonce = over.nonce ?? `n${crypto.randomUUID().replaceAll("-", "")}`;
  const bytes = method === "GET" ? new Uint8Array() : new TextEncoder().encode(body);
  const hash = await sha256Hex(bytes);
  const message = canonicalString(method, path, deviceId, timestamp, nonce, hash);
  const signature = over.signature ?? bytesToHex(await hmacSign("test-hmac-key", message));
  const headers = new Headers();
  headers.set("authorization", `Bearer ${token}`);
  headers.set("content-type", "application/json");
  headers.set("x-jarvis-device-id", deviceId);
  headers.set("x-jarvis-timestamp", timestamp);
  headers.set("x-jarvis-nonce", nonce);
  headers.set("x-jarvis-signature", signature);
  return headers;
}

export async function authed(path: string, method: string, body = "", over: Parameters<typeof signHeaders>[3] = {}) {
  const headers = await signHeaders(method, path, body, over);
  return SELF.fetch(origin(path), { method, headers, body: method === "GET" ? undefined : body });
}

function authorizationHeader(headers: Headers | Record<string, string> | undefined): string {
  if (!headers) return "";
  if (typeof (headers as Headers).get === "function") return (headers as Headers).get("authorization") ?? "";
  const record = headers as Record<string, string>;
  return record.authorization ?? record.Authorization ?? "";
}

export function mockXai(capture?: { body: string; authorization: string }) {
  fetchMock
    .get("https://api.x.ai")
    .intercept({ path: "/v1/realtime/client_secrets", method: "POST" })
    .reply((opts) => {
      if (capture) {
        capture.body = String(opts.body ?? "");
        capture.authorization = authorizationHeader(opts.headers);
      }
      return {
        statusCode: 200,
        data: JSON.stringify({
          value: "ephemeral-test-value",
          expires_at: Math.floor(Date.now() / 1000) + 900,
        }),
        responseOptions: { headers: { "content-type": "application/json" } },
      };
    });
}

export function mockWebhook(bucket: { raw: string; authorization: string }[] = []) {
  fetchMock
    .get("https://webhook.example")
    .intercept({ path: "/hook/test", method: "POST" })
    .reply((opts) => {
      bucket.push({
        raw: String(opts.body ?? ""),
        authorization: authorizationHeader(opts.headers),
      });
      return { statusCode: 200, data: "{}" };
    });
}

export async function resetHub(): Promise<void> {
  const stub = env.DEVICE_HUB.get(env.DEVICE_HUB.idFromName(env.DEVICE_ID));
  const { runInDurableObject } = await import("cloudflare:test");
  await runInDurableObject(stub, async (_instance: unknown, state: DurableObjectState) => {
    await state.storage.deleteAll();
  });
}

export async function seedHub(entries: Record<string, unknown>): Promise<void> {
  const stub = env.DEVICE_HUB.get(env.DEVICE_HUB.idFromName(env.DEVICE_ID));
  const { runInDurableObject } = await import("cloudflare:test");
  await runInDurableObject(stub, async (_instance: unknown, state: DurableObjectState) => {
    for (const [key, value] of Object.entries(entries)) await state.storage.put(key, value);
  });
}

export async function readRequest(id: string): Promise<RequestRecord | undefined> {
  const stub = env.DEVICE_HUB.get(env.DEVICE_HUB.idFromName(env.DEVICE_ID));
  const { runInDurableObject } = await import("cloudflare:test");
  return runInDurableObject(stub, async (_instance: unknown, state: DurableObjectState) => {
    return state.storage.get<RequestRecord>(keys.request(id));
  });
}

const earlyMessages = new WeakMap<WebSocket, Record<string, unknown>[]>();

export function waitFor(ws: WebSocket, pred: (message: Record<string, unknown>) => boolean): Promise<Record<string, unknown>> {
  const queued = earlyMessages.get(ws) ?? [];
  const already = queued.find(pred);
  if (already) return Promise.resolve(already);
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(new Error("websocket message timeout")), 3000);
    const onMessage = (event: MessageEvent) => {
      const text = typeof event.data === "string" ? event.data : "";
      const message = JSON.parse(text) as Record<string, unknown>;
      if (!pred(message)) return;
      clearTimeout(timer);
      ws.removeEventListener("message", onMessage);
      resolve(message);
    };
    ws.addEventListener("message", onMessage);
  });
}

export async function openSocket(): Promise<WebSocket> {
  const headers = await signHeaders("GET", `${prefix}/ws`);
  headers.set("Upgrade", "websocket");
  const response = await SELF.fetch(origin(`${prefix}/ws`), { headers });
  const ws = response.webSocket;
  if (!ws) throw new Error(`websocket upgrade failed: ${response.status}`);
  const queued: Record<string, unknown>[] = [];
  earlyMessages.set(ws, queued);
  ws.addEventListener("message", (event) => {
    const text = typeof event.data === "string" ? event.data : "";
    try {
      queued.push(JSON.parse(text) as Record<string, unknown>);
    } catch {
      // ignore non-json
    }
  });
  ws.accept();
  return ws;
}
