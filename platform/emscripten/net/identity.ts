// ─────────────────────────────────────────────────────────────────────────────
// IDENTITY — a stable per-tab device id + small helpers.
//
// Reused ~as-is from the scorchedearth-multi transport. The device id is the Nostr
// signaling address; it is NOT the Duke connectindex (that peerToken is assigned by
// the host during the join handshake — see match.ts / duke-net.ts).
// ─────────────────────────────────────────────────────────────────────────────

/** Short random token (for match ids / device ids). Non-deterministic on purpose. */
export function uid(): string {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

function readDeviceId(): string {
  // Minted fresh on EVERY page load, stored nowhere. It must be unique per live
  // JS context: each peer discards signaling where `msg.from === DEVICE_ID` as its
  // own echo, so two contexts sharing an id can never connect to each other.
  // sessionStorage looked per-tab but is NOT collision-safe: Chrome's "Duplicate
  // Tab" and session restore copy it wholesale, and two same-profile windows then
  // mutually self-filter (live-reported: "they seem to be trying to share
  // something"). Nothing needs the id stable across a reload — the GRP-download
  // relaunch rejoins by saved MATCH info (roomKey/hostId), not by own id.
  return uid() + "-" + uid();
}

/** This tab's stable device id (per-tab so two tabs on one machine never collide;
 *  ephemeral if storage is blocked). */
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
