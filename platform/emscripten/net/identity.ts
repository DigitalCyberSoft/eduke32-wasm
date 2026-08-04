// ─────────────────────────────────────────────────────────────────────────────
// IDENTITY — a stable per-tab device id + small helpers.
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
  // PER-TAB id via sessionStorage, NOT localStorage: two tabs in the same browser
  // must get DIFFERENT ids, otherwise each discards the other's signaling as its own
  // echo (signaling.ts `msg.from === DEVICE_ID`) and the WebRTC handshake never
  // completes (stuck at "Connecting to host"). sessionStorage survives a same-tab
  // reload (e.g. the GRP-download relaunch, which also uses sessionStorage) but is
  // isolated per tab, so two clients on one machine can host+join each other.
  let d: string | null = null;
  try {
    d = sessionStorage.getItem(DID_KEY);
  } catch {
    d = null;
  }
  if (!d) {
    d = uid() + "-" + uid();
    try {
      sessionStorage.setItem(DID_KEY, d);
    } catch {
      /* private mode / no storage: ephemeral id for this session */
    }
  }
  return d;
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
