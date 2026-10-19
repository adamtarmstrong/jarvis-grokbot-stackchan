import { timingSafeEqual, hexToBytes } from "./crypto";

export interface SessionRecord {
  id: string;
  expiresAt: number;
  charge: number;
  grantedAt: number;
}

export interface DailyRecord {
  day: string;
  minutes: number;
}

export interface RecentCommand {
  hash: string;
  at: number;
}

export interface RequestRecord {
  tokenHash: string;
  createdAt: number;
  expiresAt: number;
  status: "pending" | "delivered" | "received" | "played" | "timed_out";
  /** Reply text waiting for the device. Cleared after it is played. */
  text?: string;
  /** True once a connected device has been told this request timed out. */
  told?: boolean;
}

export const keys = {
  session: "session",
  daily: "daily",
  grants: "grants",
  recent: "recent",
  queue: "queue",
  request: (id: string) => `req:${id}`,
  nonce: (nonce: string) => `n:${nonce}`,
};

export function utcDay(nowMs: number): string {
  return new Date(nowMs).toISOString().slice(0, 10);
}

export type ReserveReason = "ok" | "session" | "daily" | "hourly";

export async function reserveSession(
  storage: DurableObjectStorage,
  now: number,
  ttlSec: number,
  dailyMax: number,
  hourlyMax: number,
  reservationId: string,
): Promise<ReserveReason> {
  let reason: ReserveReason = "ok";
  await storage.transaction(async (txn) => {
    const session = await txn.get<SessionRecord>(keys.session);
    if (session && session.expiresAt > now) {
      reason = "session";
      return;
    }
    const charge = Math.ceil(ttlSec / 60);
    const existing = await txn.get<DailyRecord>(keys.daily);
    const daily: DailyRecord =
      existing && existing.day === utcDay(now) ? existing : { day: utcDay(now), minutes: 0 };
    if (daily.minutes + charge > dailyMax) {
      reason = "daily";
      return;
    }
    const grants = ((await txn.get<number[]>(keys.grants)) ?? []).filter((t) => now - t < 3_600_000);
    if (grants.length >= hourlyMax) {
      reason = "hourly";
      return;
    }
    grants.push(now);
    daily.minutes += charge;
    await txn.put(keys.grants, grants);
    await txn.put(keys.daily, daily);
    const next: SessionRecord = {
      id: reservationId,
      expiresAt: now + ttlSec * 1000,
      charge,
      grantedAt: now,
    };
    await txn.put(keys.session, next);
    reason = "ok";
  });
  return reason;
}

export async function releaseSession(storage: DurableObjectStorage, reservationId: string): Promise<void> {
  await storage.transaction(async (txn) => {
    const session = await txn.get<SessionRecord>(keys.session);
    if (!session || session.id !== reservationId) return;
    await txn.delete(keys.session);
    const daily = await txn.get<DailyRecord>(keys.daily);
    if (daily) {
      daily.minutes = Math.max(0, daily.minutes - session.charge);
      await txn.put(keys.daily, daily);
    }
    const grants = (await txn.get<number[]>(keys.grants)) ?? [];
    const idx = grants.lastIndexOf(session.grantedAt);
    if (idx >= 0) grants.splice(idx, 1);
    await txn.put(keys.grants, grants);
  });
}

export async function claimNonce(storage: DurableObjectStorage, nonce: string, now: number): Promise<boolean> {
  const key = keys.nonce(nonce);
  const remembered = await storage.list<number>({ prefix: "n:" });
  for (const [storedKey, seenAt] of remembered) {
    if (typeof seenAt === "number" && now - seenAt > 180_000) await storage.delete(storedKey);
  }
  let fresh = false;
  await storage.transaction(async (txn) => {
    const prev = await txn.get<number>(key);
    if (typeof prev === "number" && now - prev <= 180_000) {
      fresh = false;
      return;
    }
    await txn.put(key, now);
    fresh = true;
  });
  return fresh;
}

const DUMMY_HASH = new Uint8Array(32);

export async function acceptReply(
  storage: DurableObjectStorage,
  requestId: string,
  presentedHashHex: string,
  text: string,
  now: number,
): Promise<"ok" | "reject"> {
  const presented = hexToBytes(presentedHashHex);
  let result: "ok" | "reject" = "reject";
  await storage.transaction(async (txn) => {
    const rec = await txn.get<RequestRecord>(keys.request(requestId));
    const stored = rec ? hexToBytes(rec.tokenHash) : null;
    const hashOk = timingSafeEqual(presented ?? DUMMY_HASH, stored ?? DUMMY_HASH);
    if (!rec || !presented || !stored || rec.tokenHash.length === 0 || !hashOk) return;
    if (rec.status === "timed_out" || rec.expiresAt <= now) {
      rec.status = "timed_out";
      rec.tokenHash = "";
      rec.text = undefined;
      await txn.put(keys.request(requestId), rec);
      return;
    }
    if (rec.status !== "pending") return;
    rec.tokenHash = "";
    rec.text = text;
    rec.status = "pending";
    await txn.put(keys.request(requestId), rec);
    const queue = (await txn.get<string[]>(keys.queue)) ?? [];
    if (!queue.includes(requestId)) queue.push(requestId);
    await txn.put(keys.queue, queue.slice(-20));
    result = "ok";
  });
  return result;
}

export async function sweepExpired(storage: DurableObjectStorage, now: number): Promise<string[]> {
  const listed = await storage.list<RequestRecord>({ prefix: "req:" });
  const timedOut: string[] = [];
  for (const [key, rec] of listed) {
    if (rec.status === "played" || rec.status === "timed_out") continue;
    if (rec.expiresAt > now) continue;
    rec.status = "timed_out";
    rec.text = undefined;
    rec.tokenHash = "";
    rec.told = false;
    await storage.put(key, rec);
    timedOut.push(key.slice("req:".length));
  }
  if (timedOut.length > 0) {
    const queue = (await storage.get<string[]>(keys.queue)) ?? [];
    const drop = new Set(timedOut);
    await storage.put(
      keys.queue,
      queue.filter((id) => !drop.has(id)),
    );
  }
  return timedOut;
}

export async function untoldTimeouts(storage: DurableObjectStorage, now: number): Promise<string[]> {
  await sweepExpired(storage, now);
  const listed = await storage.list<RequestRecord>({ prefix: "req:" });
  const ids: string[] = [];
  for (const [key, rec] of listed) {
    if (rec.status === "timed_out" && rec.told !== true) ids.push(key.slice("req:".length));
  }
  return ids;
}

export async function markTold(storage: DurableObjectStorage, ids: string[]): Promise<void> {
  for (const id of ids) {
    const rec = await storage.get<RequestRecord>(keys.request(id));
    if (!rec) continue;
    rec.told = true;
    await storage.put(keys.request(id), rec);
  }
}
