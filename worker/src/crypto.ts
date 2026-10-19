const encoder = new TextEncoder();

export function bytesToHex(bytes: ArrayBuffer | Uint8Array): string {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes);
  let out = "";
  for (let i = 0; i < view.length; i++) out += view[i]!.toString(16).padStart(2, "0");
  return out;
}

export function hexToBytes(hex: string): Uint8Array | null {
  if (hex.length % 2 !== 0 || !/^[0-9a-fA-F]*$/.test(hex)) return null;
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = Number.parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  return out;
}

export async function sha256(bytes: Uint8Array): Promise<Uint8Array> {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return new Uint8Array(digest);
}

export async function sha256Hex(bytes: Uint8Array): Promise<string> {
  return bytesToHex(await sha256(bytes));
}

/**
 * Compare two byte strings in constant time. Length is checked up front because
 * every hash we compare is fixed-width; a length mismatch still walks a buffer
 * so the function does not return on the first differing byte.
 */
export function timingSafeEqual(a: Uint8Array, b: Uint8Array): boolean {
  const len = Math.max(a.length, b.length, 1);
  let diff = a.length ^ b.length;
  for (let i = 0; i < len; i++) {
    const av = i < a.length ? a[i]! : 0;
    const bv = i < b.length ? b[i]! : 0;
    diff |= av ^ bv;
  }
  return diff === 0;
}

export function timingSafeEqualText(a: string, b: string): boolean {
  return timingSafeEqual(encoder.encode(a), encoder.encode(b));
}

/**
 * Canonical request string. Firmware and this Worker must build the same bytes.
 *
 *   JARVIS1\n
 *   METHOD\n
 *   PATH\n          // pathname only, including the secret prefix, no query
 *   DEVICE_ID\n
 *   TIMESTAMP\n     // unix seconds, 10 digits
 *   NONCE\n
 *   HEX(SHA256(body))
 */
export function canonicalString(
  method: string,
  path: string,
  deviceId: string,
  timestamp: string,
  nonce: string,
  bodySha256Hex: string,
): string {
  return ["JARVIS1", method.toUpperCase(), path, deviceId, timestamp, nonce, bodySha256Hex].join("\n");
}

export async function importHmacKey(secret: string): Promise<CryptoKey> {
  return crypto.subtle.importKey("raw", encoder.encode(secret), { name: "HMAC", hash: "SHA-256" }, false, [
    "sign",
    "verify",
  ]);
}

export async function hmacSign(secret: string, message: string): Promise<Uint8Array> {
  const key = await importHmacKey(secret);
  const sig = await crypto.subtle.sign("HMAC", key, encoder.encode(message));
  return new Uint8Array(sig);
}

export async function hmacVerify(secret: string, message: string, signature: Uint8Array): Promise<boolean> {
  const key = await importHmacKey(secret);
  // Copy into a fresh ArrayBuffer. A Uint8Array view can be backed by a larger buffer.
  const raw = new Uint8Array(signature.byteLength);
  raw.set(signature);
  return crypto.subtle.verify("HMAC", key, raw, encoder.encode(message));
}

export function randomBytes(n: number): Uint8Array {
  const out = new Uint8Array(n);
  crypto.getRandomValues(out);
  return out;
}

export function base64Url(bytes: Uint8Array): string {
  let bin = "";
  for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]!);
  return btoa(bin).replaceAll("+", "-").replaceAll("/", "_").replaceAll("=", "");
}

export function skewOk(timestampSec: number, nowMs: number, maxSkewSec = 60): boolean {
  if (!Number.isFinite(timestampSec)) return false;
  const delta = Math.abs(nowMs / 1000 - timestampSec);
  return delta <= maxSkewSec;
}
