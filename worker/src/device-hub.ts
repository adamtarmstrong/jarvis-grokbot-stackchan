import {
  acceptReply,
  claimNonce,
  keys,
  markTold,
  releaseSession,
  reserveSession,
  settleSession,
  untoldTimeouts,
  type RequestRecord,
} from "./state";

function parseMessage(raw: string | ArrayBuffer): Record<string, unknown> | null {
  try {
    const text = typeof raw === "string" ? raw : new TextDecoder().decode(raw);
    const value = JSON.parse(text) as unknown;
    if (!value || typeof value !== "object") return null;
    return value as Record<string, unknown>;
  } catch {
    return null;
  }
}

export class DeviceHub implements DurableObject {
  constructor(
    private readonly ctx: DurableObjectState,
    _env: unknown,
  ) {}

  async fetch(request: Request): Promise<Response> {
    const url = new URL(request.url);
    const now = Date.now();

    if (request.headers.get("Upgrade") === "websocket") {
      return this.openSocket(now);
    }

    await this.announce(null, now);

    let body: Record<string, unknown> = {};
    if (request.method === "POST") {
      try {
        body = (await request.json()) as Record<string, unknown>;
      } catch {
        return Response.json({ ok: false }, { status: 400 });
      }
    }

    switch (url.pathname) {
      case "/internal/nonce":
        return Response.json({ ok: await claimNonce(this.ctx.storage, String(body.nonce ?? ""), now) });
      case "/internal/reserve":
        return Response.json({
          reason: await reserveSession(
            this.ctx.storage,
            now,
            Number(body.ttlSec),
            Number(body.dailyMax),
            Number(body.hourlyMax),
            String(body.reservationId ?? ""),
          ),
        });
      case "/internal/release":
        await releaseSession(this.ctx.storage, String(body.reservationId ?? ""));
        return Response.json({ ok: true });
      case "/internal/ask":
        return Response.json(await this.ask(body, now));
      case "/internal/cancel":
        await this.ctx.storage.delete(keys.request(String(body.requestId ?? "")));
        return Response.json({ ok: true });
      case "/internal/reply":
        return Response.json(await this.reply(body, now));
      default:
        return new Response(null, { status: 404 });
    }
  }

  async alarm(): Promise<void> {
    await this.announce(null, Date.now());
  }

  async webSocketMessage(ws: WebSocket, message: string | ArrayBuffer): Promise<void> {
    const msg = parseMessage(message);
    if (!msg) return;
    if (msg.type === "session_end") {
      await settleSession(this.ctx.storage, Date.now());
      ws.send(JSON.stringify({ type: "session_end", ok: true }));
      return;
    }
    if (msg.type === "state" && typeof msg.request_id === "string") {
      if (msg.state !== "received" && msg.state !== "played") return;
      const rec = await this.ctx.storage.get<RequestRecord>(keys.request(msg.request_id));
      if (!rec || rec.status === "timed_out") return;
      rec.status = msg.state;
      if (msg.state === "played") rec.text = undefined;
      await this.ctx.storage.put(keys.request(msg.request_id), rec);
      if (msg.state === "received" || msg.state === "played") await this.dropQueued(msg.request_id);
      ws.send(JSON.stringify({ type: "state", request_id: msg.request_id, state: msg.state }));
    }
  }

  async webSocketClose(): Promise<void> {}

  private async openSocket(now: number): Promise<Response> {
    const pair = new WebSocketPair();
    const client = pair[0];
    const server = pair[1];
    this.ctx.acceptWebSocket(server);
    for (const existing of this.ctx.getWebSockets()) {
      if (existing !== server) existing.close(4000, "replaced");
    }
    await this.announce(server, now);
    await this.flush(server, now);
    return new Response(null, { status: 101, webSocket: client });
  }

  private async ask(body: Record<string, unknown>, now: number): Promise<{ ok: boolean; reason: string }> {
    const textHash = String(body.textHash ?? "");
    const requestId = String(body.requestId ?? "");
    const tokenHash = String(body.tokenHash ?? "");
    const windowMs = Number(body.windowMs);
    const interrupt = body.interrupt === true;
    const expiresAt = Number(body.expiresAt);
    if (!textHash || !requestId || !tokenHash || !Number.isFinite(expiresAt)) {
      return { ok: false, reason: "bad" };
    }
    const recent = ((await this.ctx.storage.get<{ hash: string; at: number }[]>(keys.recent)) ?? []).filter(
      (item) => now - item.at < windowMs,
    );
    if (!interrupt && recent.some((item) => item.hash === textHash)) {
      return { ok: false, reason: "duplicate" };
    }
    recent.push({ hash: textHash, at: now });
    await this.ctx.storage.put(keys.recent, recent.slice(-50));
    const rec: RequestRecord = { tokenHash, createdAt: now, expiresAt, status: "pending" };
    await this.ctx.storage.put(keys.request(requestId), rec);
    const current = await this.ctx.storage.getAlarm();
    if (current === null || expiresAt < current) await this.ctx.storage.setAlarm(expiresAt);
    return { ok: true, reason: "ok" };
  }

  private async reply(body: Record<string, unknown>, now: number): Promise<{ ok: boolean }> {
    const requestId = String(body.requestId ?? "");
    const tokenHash = String(body.tokenHash ?? "");
    const text = String(body.text ?? "");
    const accepted = await acceptReply(this.ctx.storage, requestId, tokenHash, text, now);
    if (accepted !== "ok") return { ok: false };
    const sockets = this.ctx.getWebSockets();
    const live = sockets[sockets.length - 1];
    if (live) await this.flush(live, now);
    return { ok: true };
  }

  private async flush(ws: WebSocket, now: number): Promise<void> {
    const queue = (await this.ctx.storage.get<string[]>(keys.queue)) ?? [];
    const remain: string[] = [];
    for (const id of queue) {
      const rec = await this.ctx.storage.get<RequestRecord>(keys.request(id));
      if (!rec) continue;
      if (rec.expiresAt <= now || rec.status === "timed_out") {
        rec.status = "timed_out";
        rec.text = undefined;
        rec.tokenHash = "";
        await this.ctx.storage.put(keys.request(id), rec);
        ws.send(JSON.stringify({ type: "state", request_id: id, state: "timed_out" }));
        continue;
      }
      if (rec.status === "received" || rec.status === "played" || rec.text === undefined) continue;
      ws.send(JSON.stringify({ type: "reply", request_id: id, text: rec.text }));
      rec.status = "delivered";
      await this.ctx.storage.put(keys.request(id), rec);
      remain.push(id);
    }
    await this.ctx.storage.put(keys.queue, remain);
  }

  private async announce(ws: WebSocket | null, now: number): Promise<void> {
    const ids = await untoldTimeouts(this.ctx.storage, now);
    if (ids.length === 0) return;
    const targets = ws ? [ws] : this.ctx.getWebSockets();
    if (targets.length === 0) return;
    for (const target of targets) {
      for (const id of ids) target.send(JSON.stringify({ type: "state", request_id: id, state: "timed_out" }));
    }
    await markTold(this.ctx.storage, ids);
  }

  private async dropQueued(requestId: string): Promise<void> {
    const queue = (await this.ctx.storage.get<string[]>(keys.queue)) ?? [];
    await this.ctx.storage.put(
      keys.queue,
      queue.filter((id) => id !== requestId),
    );
  }
}
