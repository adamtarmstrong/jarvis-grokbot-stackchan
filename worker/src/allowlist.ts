const XAI_HOST = "api.x.ai";

export class FetchBlocked extends Error {
  constructor() {
    super("blocked");
    this.name = "FetchBlocked";
  }
}

export function webhookHost(webhookUrl: string): string | null {
  try {
    const url = new URL(webhookUrl);
    if (url.protocol !== "https:") return null;
    if (url.username || url.password) return null;
    if (url.port !== "" && url.port !== "443") return null;
    if (!url.hostname || url.hostname === XAI_HOST) return null;
    return url.hostname;
  } catch {
    return null;
  }
}

export function isAllowedUrl(raw: string, configuredWebhook: string): boolean {
  let url: URL;
  try {
    url = new URL(raw);
  } catch {
    return false;
  }
  if (url.protocol !== "https:") return false;
  if (url.username || url.password) return false;
  if (url.port !== "" && url.port !== "443") return false;
  if (url.hostname === XAI_HOST) return true;
  const host = webhookHost(configuredWebhook);
  return host !== null && url.hostname === host;
}

/**
 * Fetch only https://api.x.ai or the configured webhook host.
 * Redirects are not followed. The caller must not log the URL if it could
 * contain a secret path; webhook URLs are secrets.
 */
export async function safeFetch(raw: string, init: RequestInit, configuredWebhook: string): Promise<Response> {
  if (!isAllowedUrl(raw, configuredWebhook)) throw new FetchBlocked();
  const response = await fetch(raw, { ...init, redirect: "manual" });
  if (response.status >= 300 && response.status < 400) {
    // Drop the body and the Location header so a redirect target is never reused.
    await response.arrayBuffer().catch(() => undefined);
    return new Response(null, { status: 502 });
  }
  return response;
}
