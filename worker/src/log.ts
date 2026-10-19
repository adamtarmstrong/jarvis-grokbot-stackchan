export interface LogFields {
  requestId: string;
  route: "token" | "ask" | "reply" | "ws" | "denied";
  status: number;
  ms: number;
}

/** Logs carry a request id, a route class, a status and a duration. Nothing else. */
export function formatLog(fields: LogFields): string {
  return JSON.stringify({
    requestId: fields.requestId,
    route: fields.route,
    status: fields.status,
    ms: Math.max(0, Math.round(fields.ms)),
  });
}

export function logRequest(fields: LogFields): void {
  console.log(formatLog(fields));
}
