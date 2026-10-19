import { env, fetchMock } from "cloudflare:test";
import { beforeAll, beforeEach, describe, expect, it } from "vitest";
import { rateLimit } from "../src/index";
import { keys } from "../src/state";
import { authed, mockXai, openSocket, prefix, resetHub, seedHub, waitFor } from "./helpers";

beforeAll(() => {
  fetchMock.activate();
  fetchMock.disableNetConnect();
});

beforeEach(async () => {
  await resetHub();
});

describe("session and spend caps", () => {
  it("allows one active voice session", async () => {
    mockXai();
    expect((await authed(`${prefix}/token`, "POST", "{}")).status).toBe(200);
    const again = await authed(`${prefix}/token`, "POST", "{}");
    expect(again.status).toBe(429);
    expect(await again.text()).toBe("");
  });

  it("frees the session when the device ends it", async () => {
    mockXai();
    mockXai();
    const socket = await openSocket();
    expect((await authed(`${prefix}/token`, "POST", "{}")).status).toBe(200);
    const ended = waitFor(socket, (message) => message.type === "session_end");
    socket.send(JSON.stringify({ type: "session_end" }));
    expect((await ended).ok).toBe(true);
    expect((await authed(`${prefix}/token`, "POST", "{}")).status).toBe(200);
    socket.close();
  });

  it("stops issuing tokens once the daily minute cap is reached", async () => {
    await seedHub({ daily: { day: new Date().toISOString().slice(0, 10), minutes: 55 } });
    const response = await authed(`${prefix}/token`, "POST", "{}");
    expect(response.status).toBe(429);
  });

  it("stops issuing tokens once the hourly grant cap is reached", async () => {
    const now = Date.now();
    await seedHub({ grants: [now, now, now, now, now, now] });
    const response = await authed(`${prefix}/token`, "POST", "{}");
    expect(response.status).toBe(429);
  });

  it("releases a reservation when xAI fails so the next try can proceed", async () => {
    fetchMock
      .get("https://api.x.ai")
      .intercept({ path: "/v1/realtime/client_secrets", method: "POST" })
      .reply(500, "nope");
    expect((await authed(`${prefix}/token`, "POST", "{}")).status).toBe(502);
    mockXai();
    expect((await authed(`${prefix}/token`, "POST", "{}")).status).toBe(200);
  });

  it("fails closed when the Workers rate limiter rejects the caller IP", async () => {
    const ip = "203.0.113.50";
    let blocked = false;
    for (let i = 0; i < 120; i++) {
      const ok = await rateLimit(env, `ip:${ip}`);
      if (!ok) {
        blocked = true;
        break;
      }
    }
    expect(blocked).toBe(true);
    const headers = await (await import("./helpers")).signHeaders("POST", `${prefix}/token`, "{}");
    headers.set("cf-connecting-ip", ip);
    const response = await (await import("cloudflare:test")).SELF.fetch(`http://example.com${prefix}/token`, {
      method: "POST",
      headers,
      body: "{}",
    });
    expect(response.status).toBe(429);
    const stored = env.DEVICE_HUB.get(env.DEVICE_HUB.idFromName(env.DEVICE_ID));
    const { runInDurableObject } = await import("cloudflare:test");
    const session = await runInDurableObject(stored, async (_instance: unknown, state: DurableObjectState) => {
      return state.storage.get(keys.session);
    });
    expect(session).toBeUndefined();
  });
});
