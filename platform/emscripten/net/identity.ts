// ─────────────────────────────────────────────────────────────────────────────
// IDENTITY — a stable per-browser device id + small helpers.
//
// Reused ~as-is from the scorchedearth-multi transport. The device id is the Nostr
// signaling address; it is NOT the Duke connectindex (that peerToken is assigned by
// the host during the join handshake — see match.ts / duke-net.ts).
// ─────────────────────────────────────────────────────────────────────────────

const DID_KEY = "eduke32-net-did";

/** Short random token (for match ids / device ids). Non-deterministic on purpose. */
export function uid(): string {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

function readDeviceId(): string {
  let d: string | null = null;
  try {
    d = localStorage.getItem(DID_KEY);
  } catch {
    d = null;
  }
  if (!d) {
    d = uid() + "-" + uid();
    try {
      localStorage.setItem(DID_KEY, d);
    } catch {
      /* private mode / no storage: ephemeral id for this session */
    }
  }
  return d;
}

/** This browser's stable device id (ephemeral if storage is blocked). */
export const DEVICE_ID = typeof window !== "undefined" ? readDeviceId() : "node-" + uid();

/** Unix seconds (Nostr created_at unit). */
export function nowSec(): number {
  return Math.floor(Date.now() / 1000);
}

/** Lowercase hex SHA-256 of a string. */
export async function sha256Hex(s: string): Promise<string> {
  const buf = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(s));
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
