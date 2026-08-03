// ─────────────────────────────────────────────────────────────────────────────
// PING — the lobby's per-row latency, plus the high-ping exclude filter.
//
// Two sources, coarse-to-fine:
//   1. RELAY-RTT PROXY (list-wide, before connecting): we cannot measure true P2P
//      latency to a host we have not dialed yet, so we estimate. Each host puts a
//      `pingHint` (its own median relay RTT) in its MatchInfo; the joiner adds its
//      OWN median relay RTT. The sum of the two legs to the relay cloud is a rough
//      proxy for "how far apart are we". Either leg unknown -> unknown ("?").
//   2. TRUE DATA-CHANNEL RTT (once connecting): a ping/pong over the reliable
//      channel gives the real round-trip, which replaces the proxy for that row.
//
// The high-ping filter EXCLUDES rows over a chosen threshold, but a row whose ping
// is UNKNOWN is shown "?" and always INCLUDED (never hidden for lack of data).
// ─────────────────────────────────────────────────────────────────────────────

// ── High-ping exclude filter ─────────────────────────────────────────────────

export interface PingPreset {
  label: string;
  maxMs: number; // Infinity == no limit
}

/** Presets offered in the lobby's "hide matches slower than…" control. */
export const PING_PRESETS: readonly PingPreset[] = [
  { label: "Any ping", maxMs: Infinity },
  { label: "< 100 ms", maxMs: 100 },
  { label: "< 150 ms", maxMs: 150 },
  { label: "< 250 ms", maxMs: 250 },
  { label: "< 400 ms", maxMs: 400 },
];

/**
 * Does a row pass the high-ping filter? Unknown ping (null/undefined/NaN) ALWAYS
 * passes — we never hide a match just because we could not estimate its latency.
 */
export function passesPingFilter(pingMs: number | null | undefined, maxMs: number): boolean {
  if (maxMs === Infinity) return true;
  if (pingMs == null || Number.isNaN(pingMs)) return true; // unknown -> included
  return pingMs <= maxMs;
}

/** Display string for a ping value: "?" when unknown, otherwise "NN ms". */
export function formatPing(pingMs: number | null | undefined): string {
  if (pingMs == null || Number.isNaN(pingMs)) return "?";
  return Math.round(pingMs) + " ms";
}

/** Relay-RTT proxy estimate for a row: sum of both legs to the relay cloud. Returns
 *  null if either leg is unknown. */
export function estimatePing(myRelayRttMs: number | null, hostHintMs: number | null | undefined): number | null {
  if (myRelayRttMs == null || hostHintMs == null || Number.isNaN(myRelayRttMs) || Number.isNaN(hostHintMs)) return null;
  return myRelayRttMs + hostHintMs;
}

// ── True data-channel RTT tracker (per peer) ─────────────────────────────────

interface RttState {
  ewmaMs: number | null;
  lastPingId: number;
  pending: Map<number, number>; // id -> t0 (performance.now())
}

/**
 * Tracks smoothed round-trip time per peer from ping/pong control frames. The
 * transport (duke-net.ts) sends {t:'rtt_ping', id} on the reliable channel and
 * replies to inbound pings with {t:'rtt_pong', id}; the numbers land here.
 */
export class PingTracker {
  private readonly peers = new Map<string, RttState>();
  private seq = 1;

  /** Allocate the next ping id for `peerId` and record its send time. */
  startPing(peerId: string, now = performance.now()): number {
    const st = this.peers.get(peerId) ?? { ewmaMs: null, lastPingId: 0, pending: new Map() };
    const id = this.seq++;
    st.lastPingId = id;
    st.pending.set(id, now);
    // Bound the pending map so a peer that never pongs cannot leak.
    if (st.pending.size > 8) {
      const oldest = [...st.pending.keys()][0];
      st.pending.delete(oldest);
    }
    this.peers.set(peerId, st);
    return id;
  }

  /** Record a pong; updates the EWMA RTT. Returns the sample RTT or null if the id
   *  was unknown/stale. */
  onPong(peerId: string, id: number, now = performance.now()): number | null {
    const st = this.peers.get(peerId);
    if (!st) return null;
    const t0 = st.pending.get(id);
    if (t0 == null) return null;
    st.pending.delete(id);
    const rtt = now - t0;
    st.ewmaMs = st.ewmaMs == null ? rtt : st.ewmaMs * 0.7 + rtt * 0.3;
    return rtt;
  }

  /** Current smoothed RTT for a peer, or null if never measured. */
  rtt(peerId: string): number | null {
    return this.peers.get(peerId)?.ewmaMs ?? null;
  }

  forget(peerId: string): void {
    this.peers.delete(peerId);
  }
}

// ── Relay-RTT measurement (browser only) ─────────────────────────────────────

/**
 * Measure the median WebSocket-open RTT to a set of relays as a list-wide latency
 * proxy. Browser-only (needs WebSocket); resolves to null where unavailable so the
 * lobby simply shows "?" and includes every row. Never rejects.
 */
export async function measureRelayRtt(relays: readonly string[], timeoutMs = 4000): Promise<number | null> {
  if (typeof WebSocket === "undefined") return null;
  const samples = await Promise.all(
    relays.slice(0, 5).map(
      (url) =>
        new Promise<number | null>((resolve) => {
          let done = false;
          const t0 = performance.now();
          let ws: WebSocket;
          const finish = (v: number | null) => {
            if (done) return;
            done = true;
            try {
              ws.close();
            } catch {
              /* ignore */
            }
            resolve(v);
          };
          try {
            ws = new WebSocket(url);
          } catch {
            resolve(null);
            return;
          }
          ws.onopen = () => finish(performance.now() - t0);
          ws.onerror = () => finish(null);
          setTimeout(() => finish(null), timeoutMs);
        }),
    ),
  );
  const ok = samples.filter((s): s is number => s != null).sort((a, b) => a - b);
  if (ok.length === 0) return null;
  return ok[Math.floor(ok.length / 2)]; // median
}
