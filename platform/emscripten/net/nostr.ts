// ─────────────────────────────────────────────────────────────────────────────
// NOSTR ADAPTER — encrypted pub/sub over public relays.
//
// Reused ~as-is from the scorchedearth-multi transport. A room key (base64
// AES-256) does double duty:
//   1. AES-GCM symmetric key for event content (relays/eavesdroppers see only
//      ciphertext);
//   2. SHA-256(key) -> secp256k1 secret key -> a Nostr identity. Everyone holding
//      the key signs as the same author and subscribes by authors:[pubkey], so the
//      key alone defines who is in the room. No accounts, no server.
// ─────────────────────────────────────────────────────────────────────────────
import { SimplePool } from "nostr-tools/pool";
import { finalizeEvent, getPublicKey } from "nostr-tools/pure";
import type { Event, Filter } from "nostr-tools";
import { activeRelays } from "./netconfig";

let _pool: SimplePool | null = null;
const _connected = new Set<string>();

export function initPool(relays: readonly string[] = activeRelays()): SimplePool {
  if (!_pool) _pool = new SimplePool();
  // Re-ensure EVERY call: a relay whose socket dropped was pruned from
  // _connected by its onclose, and nothing else ever re-dialed it -- so a
  // long-lived host published its match ticket into dead sockets forever
  // (measured live: ticket 298s stale with two hosts up and 15s refreshes;
  // the user's invite code "expired" while the match was running). Every
  // publish/subscribe path funnels through here, so this heals them all;
  // ensureRelays skips relays that are still healthy.
  ensureRelays(relays);
  return _pool;
}

export function ensureRelays(relays: readonly string[]): void {
  const p = _pool ?? (_pool = new SimplePool());
  for (const url of relays) {
    if (_connected.has(url)) continue;
    p.ensureRelay(url)
      .then((relay) => {
        _connected.add(url);
        relay.onclose = () => _connected.delete(url);
      })
      .catch(() => {
        /* relay down: the pool retries on next publish/subscribe */
      });
  }
}

export function connectedRelays(): string[] {
  return [..._connected];
}

// ── Room keys + AES-GCM (Web Crypto) ─────────────────────────────────────────

/** Fresh base64 AES-256 room key (used for a private match). */
export async function generateRoomKey(): Promise<string> {
  const key = await crypto.subtle.generateKey({ name: "AES-GCM", length: 256 }, true, ["encrypt", "decrypt"]);
  const raw = await crypto.subtle.exportKey("raw", key);
  return btoa(String.fromCharCode(...new Uint8Array(raw)));
}

async function importKey(b64: string): Promise<CryptoKey> {
  const raw = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
  return crypto.subtle.importKey("raw", raw, "AES-GCM", false, ["encrypt", "decrypt"]);
}

export async function encrypt(data: unknown, b64Key: string): Promise<string> {
  const key = await importKey(b64Key);
  const iv = crypto.getRandomValues(new Uint8Array(12));
  const pt = new TextEncoder().encode(JSON.stringify(data));
  const ct = await crypto.subtle.encrypt({ name: "AES-GCM", iv }, key, pt);
  const out = new Uint8Array(12 + ct.byteLength);
  out.set(iv);
  out.set(new Uint8Array(ct), 12);
  return btoa(String.fromCharCode(...out));
}

export async function decrypt<T = unknown>(b64ct: string, b64Key: string): Promise<T | null> {
  try {
    const key = await importKey(b64Key);
    const all = Uint8Array.from(atob(b64ct), (c) => c.charCodeAt(0));
    const pt = await crypto.subtle.decrypt({ name: "AES-GCM", iv: all.slice(0, 12) }, key, all.slice(12));
    return JSON.parse(new TextDecoder().decode(pt)) as T;
  } catch {
    return null; // wrong key / tampered / not-for-us: silently ignore
  }
}

/** key -> the room's deterministic Nostr keypair. */
export async function deriveKeypair(b64Key: string): Promise<{ skBytes: Uint8Array; pubkey: string }> {
  const raw = Uint8Array.from(atob(b64Key), (c) => c.charCodeAt(0));
  const skBytes = new Uint8Array(await crypto.subtle.digest("SHA-256", raw));
  return { skBytes, pubkey: getPublicKey(skBytes) };
}

// ── Ephemeral events (signaling + presence) ──────────────────────────────────

export async function publishEphemeral(
  kind: number,
  encKey: string,
  data: unknown,
  relays: readonly string[] = activeRelays(),
): Promise<void> {
  const { skBytes } = await deriveKeypair(encKey);
  const event = finalizeEvent(
    { kind, created_at: Math.floor(Date.now() / 1000), tags: [], content: await encrypt(data, encKey) },
    skBytes,
  );
  await Promise.allSettled(initPool(relays).publish([...relays], event));
}

export async function subscribeEphemeral<T = unknown>(
  kind: number,
  encKey: string,
  onEvent: (data: T) => void,
  relays: readonly string[] = activeRelays(),
): Promise<() => void> {
  const { pubkey } = await deriveKeypair(encKey);
  const filter: Filter = { kinds: [kind], authors: [pubkey], since: Math.floor(Date.now() / 1000) };
  const sub = initPool(relays).subscribeMany([...relays], filter, {
    async onevent(event: Event) {
      const data = await decrypt<T>(event.content, encKey);
      if (data !== null) onEvent(data);
    },
  });
  return () => sub.close();
}

// ── Replaceable events (NIP-78 kind 30078): public match list + start metric ──

export async function publishReplaceable(
  kind: number,
  encKey: string,
  dTag: string,
  data: unknown,
  relays: readonly string[] = activeRelays(),
): Promise<void> {
  const { skBytes } = await deriveKeypair(encKey);
  const event = finalizeEvent(
    {
      kind,
      created_at: Math.floor(Date.now() / 1000),
      tags: [["d", dTag]],
      content: await encrypt(data, encKey),
    },
    skBytes,
  );
  await Promise.allSettled(initPool(relays).publish([...relays], event));
}

export interface ReplaceableRecord<T = unknown> {
  dTag: string;
  data: T;
  createdAt: number;
}

/** One-shot query of all current replaceable records for a key (latest per d-tag). */
export async function queryReplaceable<T = unknown>(
  kind: number,
  encKey: string,
  relays: readonly string[] = activeRelays(),
): Promise<ReplaceableRecord<T>[]> {
  const { pubkey } = await deriveKeypair(encKey);
  const events = await initPool(relays).querySync([...relays], { kinds: [kind], authors: [pubkey] });
  const out: ReplaceableRecord<T>[] = [];
  for (const ev of events) {
    const dTag = ev.tags.find((t) => t[0] === "d")?.[1];
    if (dTag === undefined) continue;
    const data = await decrypt<T>(ev.content, encKey);
    if (data !== null) out.push({ dTag, data, createdAt: ev.created_at });
  }
  return out;
}

/** Live subscription to replaceable records for a key. */
export async function subscribeReplaceable<T = unknown>(
  kind: number,
  encKey: string,
  onRecord: (rec: ReplaceableRecord<T>) => void,
  relays: readonly string[] = activeRelays(),
): Promise<() => void> {
  const { pubkey } = await deriveKeypair(encKey);
  const filter: Filter = { kinds: [kind], authors: [pubkey] };
  const sub = initPool(relays).subscribeMany([...relays], filter, {
    async onevent(event: Event) {
      const dTag = event.tags.find((t) => t[0] === "d")?.[1];
      if (dTag === undefined) return;
      const data = await decrypt<T>(event.content, encKey);
      if (data !== null) onRecord({ dTag, data, createdAt: event.created_at });
    },
  });
  return () => sub.close();
}
