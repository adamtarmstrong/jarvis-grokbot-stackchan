import { fetchMock } from "cloudflare:test";
import { beforeAll, beforeEach, describe, expect, it } from "vitest";
import { bytesToHex, canonicalString, hmacSign, sha256Hex, skewOk, timingSafeEqual } from "../src/crypto";
import { isAllowedUrl, safeFetch } from "../src/allowlist";
import { formatLog } from "../src/log";
import { authed, mockXai, openSocket, prefix, resetHub } from "./helpers";

beforeAll(() => {
  fetchMock.activate();
  fetchMock.disableNetConnect();
});

beforeEach(async () => {
  await resetHub();
});

describe("crypto contract", () => {
  it("matches the published HMAC vector", async () => {
    const body = new TextEncoder().encode('{"text":"hello"}');
    const hash = await sha256Hex(body);
    expect(hash).toBe("cbbbdcd27692344de5dbab3abcaba413fb0f45307267de7081401576df1cb176");
    const message = canonicalString("POST", "/testprefix/ask", "device-1", "1760000000", "nonce-1", hash);
    expect(bytesToHex(await hmacSign("test-hmac-key", message))).toBe(
      "bf4779d5fb191b1473cfdc44e099a9c5716e647b6aed119584d784eee1e795c6",
    );
  });

  it("accepts 60 seconds of skew and rejects 61", () => {
    const now = 1_760_000_000_000;
    expect(skewOk(1_760_000_000 - 60, now)).toBe(true);
    expect(skewOk(1_760_000_000 + 60, now)).toBe(true);
    expect(skewOk(1_760_000_000 - 61, now)).toBe(false);
    expect(skewOk(1_760_000_000 + 61, now)).toBe(false);
  });

  it("compares full hashes and rejects a one-bit difference", () => {
    const a = new Uint8Array([1, 2, 3, 4]);
    const b = new Uint8Array([1, 2, 3, 4]);
    const c = new Uint8Array([1, 2, 3, 5]);
    expect(timingSafeEqual(a, b)).toBe(true);
    expect(timingSafeEqual(a, c)).toBe(false);
    expect(timingSafeEqual(a, new Uint8Array([1, 2, 3]))).toBe(false);
  });

  it("logs only the route, status, timing and request id", () => {
    const line = formatLog({ requestId: "req-1", route: "token", status: 200, ms: 12.4 });
    expect(JSON.parse(line)).toEqual({ requestId: "req-1", route: "token", status: 200, ms: 12 });
    expect(line).not.toContain("secret");
    expect(line).not.toContain("transcript");
  });
});

describe("allowlist", () => {
  const webhook = "https://webhook.example/hook/test";

  it("allows only api.x.ai and the configured webhook host", () => {
    expect(isAllowedUrl("https://api.x.ai/v1/realtime/client_secrets", webhook)).toBe(true);
    expect(isAllowedUrl("https://webhook.example/hook/test", webhook)).toBe(true);
    expect(isAllowedUrl("https://evil.example/steal", webhook)).toBe(false);
    expect(isAllowedUrl("https://api.x.ai.evil.example/v1", webhook)).toBe(false);
    expect(isAllowedUrl("http://api.x.ai/v1", webhook)).toBe(false);
    expect(isAllowedUrl("https://user:pass@api.x.ai/v1", webhook)).toBe(false);
    expect(isAllowedUrl("https://api.x.ai:8443/v1", webhook)).toBe(false);
  });

  it("does not call a blocked host", async () => {
    await expect(safeFetch("https://evil.example/x", { method: "POST" }, webhook)).rejects.toThrow(/blocked/);
  });
});

describe("routing and device auth", () => {
  it("returns an empty 404 for unknown paths, the root and a wrong prefix", async () => {
    for (const path of ["/", "/token", "/testprefix", "/otherprefix/token", "/testprefix/debug"]) {
      const response = await authed(path, "POST", "{}");
      expect(response.status).toBe(404);
      expect(await response.text()).toBe("");
    }
  });

  it("rejects a missing token, a wrong token, a bad signature and a second device id", async () => {
    const path = `${prefix}/token`;
    const missing = await authed(path, "POST", "{}", { token: "" });
    const wrong = await authed(path, "POST", "{}", { token: "not-the-token" });
    const badSig = await authed(path, "POST", "{}", { signature: "00".repeat(32) });
    const otherDevice = await authed(path, "POST", "{}", { deviceId: "device-2" });
    expect(missing.status).toBe(401);
    expect(wrong.status).toBe(401);
    expect(badSig.status).toBe(401);
    expect(otherDevice.status).toBe(401);
    expect(await otherDevice.text()).toBe("");
  });

  it("rejects timestamps more than 60 seconds off", async () => {
    const path = `${prefix}/token`;
    const oldTs = String(Math.floor(Date.now() / 1000) - 90);
    const futureTs = String(Math.floor(Date.now() / 1000) + 90);
    expect((await authed(path, "POST", "{}", { timestamp: oldTs })).status).toBe(401);
    expect((await authed(path, "POST", "{}", { timestamp: futureTs })).status).toBe(401);
  });

  it("rejects a replayed nonce", async () => {
    mockXai();
    const path = `${prefix}/token`;
    const nonce = "nonce-replay-0001";
    const first = await authed(path, "POST", "{}", { nonce });
    const second = await authed(path, "POST", "{}", { nonce });
    expect(first.status).toBe(200);
    expect(second.status).toBe(401);
  });

  it("mints a 5 to 10 minute grok-voice-3.0 secret for leo and does not send the voice to xAI", async () => {
    const captured = { body: "", authorization: "" };
    mockXai(captured);
    const response = await authed(`${prefix}/token`, "POST", "{}");
    expect(response.status).toBe(200);
    const json = (await response.json()) as {
      client_secret: string;
      expires_in: number;
      model: string;
      voice: string;
      realtime_url: string;
    };
    expect(json.client_secret).toBe("ephemeral-test-value");
    expect(json.expires_in).toBeGreaterThanOrEqual(300);
    expect(json.expires_in).toBeLessThanOrEqual(600);
    expect(json.model).toBe("grok-voice-3.0");
    expect(json.voice).toBe("leo");
    expect(json.realtime_url).toBe("wss://api.x.ai/v1/realtime?model=grok-voice-3.0");
    expect(captured.body).toBe(JSON.stringify({ expires_after: { seconds: 600 } }));
    expect(captured.authorization).toBe("Bearer test-xai-key");
    expect(JSON.stringify(json)).not.toContain("test-xai-key");
  });

  it("does not follow an xAI redirect", async () => {
    fetchMock
      .get("https://api.x.ai")
      .intercept({ path: "/v1/realtime/client_secrets", method: "POST" })
      .reply(302, "", { headers: { location: "https://evil.example/steal" } });
    const response = await authed(`${prefix}/token`, "POST", "{}");
    expect(response.status).toBe(502);
    expect(await response.text()).toBe("");
  });

  it("replaces an existing device websocket instead of keeping both", async () => {
    const first = await openSocket();
    const closed = new Promise<number>((resolve) => {
      first.addEventListener("close", (event) => resolve(event.code));
    });
    const second = await openSocket();
    expect(await closed).toBe(4000);
    expect(second.readyState).toBe(WebSocket.OPEN);
    second.close();
  });
});
