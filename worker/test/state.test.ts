import { env, runInDurableObject } from "cloudflare:test";
import { beforeEach, describe, expect, it } from "vitest";
import { keys, reserveSession, sessionMinutesUsed, settleSession } from "../src/state";
import { readHub, resetHub, seedHub } from "./helpers";

beforeEach(resetHub);
const day = (now: number) => new Date(now).toISOString().slice(0, 10);

describe("reservation accounting without live xAI", () => {
  it.each([[0, 1], [1, 1], [60_000, 1], [60_001, 2], [600_000, 10], [900_000, 10]])(
    "charges the rounded elapsed minutes after %i ms", (elapsed, expected) => {
      expect(sessionMinutesUsed(1000, 1000 + elapsed, 10)).toBe(expected);
    },
  );

  it("refunds nine minutes once, releases the lock, and keeps the hourly grant", async () => {
    const now = Date.now();
    await seedHub({
      session: { id: "reservation", grantedAt: now - 10_000, expiresAt: now + 590_000, charge: 10 },
      daily: { day: day(now), minutes: 10 }, grants: [now - 10_000],
    });
    const stub = env.DEVICE_HUB.get(env.DEVICE_HUB.idFromName(env.DEVICE_ID));
    await runInDurableObject(stub, async (_instance, state) => {
      await settleSession(state.storage, now);
      await settleSession(state.storage, now);
    });
    expect(await readHub(keys.session)).toBeUndefined();
    expect(await readHub(keys.daily)).toEqual({ day: day(now), minutes: 1 });
    expect(await readHub(keys.grants)).toEqual([now - 10_000]);
  });

  it("a full daily ledger rejects even after the old session and hourly grants expire", async () => {
    const now = Date.now();
    await seedHub({
      session: { id: "expired", grantedAt: now - 7_200_000, expiresAt: now - 6_600_000, charge: 10 },
      daily: { day: day(now), minutes: 60 }, grants: [now - 7_200_000],
    });
    const stub = env.DEVICE_HUB.get(env.DEVICE_HUB.idFromName(env.DEVICE_ID));
    const reason = await runInDurableObject(stub, (_instance, state) =>
      reserveSession(state.storage, now, 600, 60, 6, "must-not-reserve"));
    expect(reason).toBe("daily");
    expect(await readHub(keys.daily)).toEqual({ day: day(now), minutes: 60 });
    expect(await readHub(keys.grants)).toEqual([now - 7_200_000]);
  });

  it("does not subtract an old session refund from a different day's ledger", async () => {
    const now = Date.now();
    await seedHub({
      session: { id: "yesterday", grantedAt: now - 86_400_000, expiresAt: now - 85_800_000, charge: 10 },
      daily: { day: day(now), minutes: 5 },
    });
    const stub = env.DEVICE_HUB.get(env.DEVICE_HUB.idFromName(env.DEVICE_ID));
    await runInDurableObject(stub, (_instance, state) => settleSession(state.storage, now));
    expect(await readHub(keys.session)).toBeUndefined();
    expect(await readHub(keys.daily)).toEqual({ day: day(now), minutes: 5 });
  });
});
