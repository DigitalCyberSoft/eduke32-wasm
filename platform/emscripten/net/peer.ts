// ─────────────────────────────────────────────────────────────────────────────
// PEER MANAGER (Duke fork) — WebRTC with THREE data channels per peer and binary
// framing, driving the frozen net_transport.h seam.
//
// Forked from the scorchedearth-multi PeerManager. Key Duke changes:
//   * BINARY framing: dc.binaryType='arraybuffer'; game frames are raw bytes, not
//     JSON.
//   * THREE channels per peer (duke-move / duke-rel / duke-bulk), created by the
//     offerer in one SDP and matched by label on the answerer.
//   * STAR topology is enforced by the caller (match.ts): a guest only connect()s
//     to the host, the host connect()s to each guest, and no guest-guest links are
//     ever formed. The deterministic-initiator rule (smaller device id offers) is
//     kept so a host<->guest pair never glares.
//   * A PER-PEER PHASE GATE ("attached"). Before a peer is attached to the netcode
//     the connection carries the transport's own protocol — JSON control (strings)
//     on duke-rel and GRP chunks (binary) on duke-bulk. Once attached, all three
//     channels carry ONLY raw netcode frames. Because a joiner downloads a GRP,
//     reloads, and only then attaches, the two phases never overlap on a channel.
// ─────────────────────────────────────────────────────────────────────────────
import { rtcConfig, DC_INIT, DC_LABELS, CHANNEL_TO_LABEL, type DcLabel } from "./netconfig";
import { DEVICE_ID } from "./identity";
import { sendOffer, sendAnswer, sendIceCandidate } from "./signaling";

/** First a=ice-ufrag of an SDP — the per-RTCPeerConnection generation identity. */
function sdpUfrag(sdp: string | undefined): string {
  if (!sdp) return "";
  const m = /a=ice-ufrag:([^\r\n]+)/.exec(sdp);
  return m ? m[1] : sdp.slice(0, 96);
}

const MAX_BACKOFF_MS = 60_000;
// Non-trickle resend: a lost ephemeral offer/answer on a public relay would otherwise
// stall the handshake until ICE times out (~30 s). Wait for ICE gathering so the SDP
// carries its own candidates, then resend that self-contained SDP on a short timer
// until a channel opens. Reproduced + verified in test/net (LOSS=0.6).
const RESEND_INTERVAL_MS = 1200;
const RESEND_MAX = 12; // ~14 s of resends before deferring to _scheduleReconnect
// "disconnected" is TRANSIENT by spec: ICE consent blips under load (a saturating
// GRP transfer bufferbloats the uplink and delays STUN checks) flip a healthy pc
// to "disconnected" for a few seconds and back. Acting on it immediately killed
// recoverable connections — live-reported as GRP downloads dying ~60s in. Only
// after this grace (still disconnected) is the pc treated as failed.
//
// 45s, NOT ~10: a slow machine loading the next map stalls WHOLE-SYSTEM (CPU +
// swap), starving even its network process — consent lapses for the entire
// load and the fast side flaps to "disconnected" (live-reported: the faster
// machine dropped the session before the slow host finished loading a map).
// Chrome escalates a genuinely dead pc to "failed" on its own after ~30s of
// continuous outage, so true deaths surface BEFORE this timer; the synthetic
// escalation only reaps pcs stuck in "disconnected" forever, and being more
// trigger-happy than Chrome here only ever kills recoverable sessions. In-game
// responsiveness does not come from this timer either way — the engine's own
// silence axes (10s, MODE_GAME-gated) own that.
const DISCONNECTED_GRACE_MS = 45_000;

// Test-harness fault injection for the duke-move channel (see sendNet). Parsed
// once; 0/absent = disabled (the production default).
function _numParam(name: string): number {
  try {
    if (typeof location === "undefined") return 0;
    const v = Number(new URLSearchParams(location.search).get(name));
    return Number.isFinite(v) && v > 0 ? v : 0;
  } catch {
    return 0;
  }
}
const TEST_MOVE_LOSS = _numParam("lossmove"); // percent of move frames to drop
const TEST_MOVE_JITTER = _numParam("jitmove"); // max ms of random delay (reorders)

interface Conn {
  pc: RTCPeerConnection;
  dcs: Map<DcLabel, RTCDataChannel>;
  encKey: string;
  relays: readonly string[];
  iceQueue: RTCIceCandidateInit[];
  backoff: number;
  reconnectTimer: ReturnType<typeof setTimeout> | null;
  remoteDescSet: boolean;
  attached: boolean; // false = transport lobby protocol; true = raw netcode frames
  readyFired: boolean; // onChannelsReady emitted once all 3 channels opened
  resendTimer: ReturnType<typeof setInterval> | null; // periodic offer/answer resend until connected
  lastHeard: number;
  created: number; // pc birth (drives the never-connected eviction in match._prune)
  offerUfrag: string; // ice-ufrag of the remote OFFER we answered ("" = we offered)
  discoTimer: ReturnType<typeof setTimeout> | null; // "disconnected" grace escalation
}

export type ConnState = RTCPeerConnectionState;

export class PeerManager {
  private _conns = new Map<string, Conn>();
  /** Offer ufrags already negotiated per peer. A relay can redeliver a superseded
   *  generation's offer for minutes; answering it would resurrect a dead session
   *  over a live one. Survives conn replacement (that is the point); cleared only
   *  on a deliberate close(). Mirrors the native host's nn_peer seenOfferUfrags_. */
  private _seenOfferUfrags = new Map<string, string[]>();
  /** Set by the owning Match: local-only matches build STUN-less peer connections
   *  (host candidates only -> same-network pairs only). See netconfig.rtcConfig. */
  localOnly = false;

  /** Inbound JSON control (pre-attach) from a peer's duke-rel channel. */
  onControl: ((peerId: string, msg: unknown) => void) | null = null;
  /** Inbound GRP chunk (pre-attach) from a peer's duke-bulk channel (raw bytes). */
  onBulkChunk: ((peerId: string, bytes: Uint8Array) => void) | null = null;
  /** Inbound raw netcode frame (post-attach) — (peerId, channelIndex, bytes). */
  onNetFrame: ((peerId: string, channel: number, bytes: Uint8Array) => void) | null = null;
  /** All three data channels to a peer are open. */
  onChannelsReady: ((peerId: string) => void) | null = null;
  onConnectionChange: ((peerId: string, state: ConnState) => void) | null = null;

  private _ensure(peerId: string, encKey: string, relays: readonly string[]): Conn {
    const existing = this._conns.get(peerId);
    if (existing) {
      const s = existing.pc.connectionState;
      // "disconnected" is a recovering pc, not a dead one — destroying it here
      // (this runs on every ~5s presence tick via connect()) was one of the
      // mid-transfer killers. Only failed/closed conns get rebuilt.
      if (s === "connected" || s === "connecting" || s === "new" || s === "disconnected") return existing;
      this._cleanup(peerId, false);
    }
    const pc = new RTCPeerConnection(rtcConfig(this.localOnly));
    const conn: Conn = {
      pc,
      dcs: new Map(),
      encKey,
      relays,
      iceQueue: [],
      backoff: 2000,
      reconnectTimer: null,
      remoteDescSet: false,
      attached: false,
      readyFired: false,
      resendTimer: null,
      lastHeard: Date.now(),
      created: Date.now(),
      offerUfrag: "",
      discoTimer: null,
    };
    this._conns.set(peerId, conn);

    pc.onicecandidate = (e) => {
      if (e.candidate) sendIceCandidate(encKey, peerId, e.candidate, relays).catch(() => {});
    };
    pc.onconnectionstatechange = () => {
      const s = pc.connectionState;
      console.log(`[dnet] pc ${peerId.slice(0, 8)} connectionState=${s}`);
      if (s === "disconnected") {
        // Hold the flap back from upper layers and give ICE its recovery window;
        // escalate to a real failure only if it sticks past the grace.
        if (!conn.discoTimer)
          conn.discoTimer = setTimeout(() => {
            conn.discoTimer = null;
            if (this._conns.get(peerId) !== conn) return; // replaced meanwhile
            const now = conn.pc.connectionState;
            if (now === "connected" || now === "connecting" || now === "new") return; // recovered
            console.log(`[dnet] pc ${peerId.slice(0, 8)} disconnected > ${DISCONNECTED_GRACE_MS / 1000}s — treating as failed`);
            this.onConnectionChange?.(peerId, "failed");
            this._scheduleReconnect(peerId);
          }, DISCONNECTED_GRACE_MS);
        return;
      }
      if (conn.discoTimer) { clearTimeout(conn.discoTimer); conn.discoTimer = null; }
      this.onConnectionChange?.(peerId, s);
      if (s === "connected") conn.backoff = 2000;
      else if (s === "failed") this._scheduleReconnect(peerId);
    };
    pc.oniceconnectionstatechange = () => console.log(`[dnet] pc ${peerId.slice(0, 8)} iceConnectionState=${pc.iceConnectionState}`);
    pc.ondatachannel = (e) => this._setupDc(peerId, e.channel);
    return conn;
  }

  private _setupDc(peerId: string, dc: RTCDataChannel): void {
    const conn = this._conns.get(peerId);
    if (!conn) return;
    const label = dc.label as DcLabel;
    if (!DC_LABELS.includes(label)) {
      // Unknown label — not one of ours; ignore defensively.
      try {
        dc.close();
      } catch {
        /* ignore */
      }
      return;
    }
    dc.binaryType = "arraybuffer";
    conn.dcs.set(label, dc);

    dc.onopen = () => {
      conn.lastHeard = Date.now();
      this._clearResend(peerId); // connected: stop resending the offer/answer
      console.log(`[dnet] channel '${label}' open to ${peerId.slice(0, 8)}`);
      // Re-announce the connection so listeners see the now-usable channel (the pc
      // reaches "connected" before the channels open).
      this.onConnectionChange?.(peerId, conn.pc.connectionState);
      if (!conn.readyFired && DC_LABELS.every((l) => conn.dcs.get(l)?.readyState === "open")) {
        conn.readyFired = true;
        this.onChannelsReady?.(peerId);
      }
    };
    dc.onmessage = (e) => {
      conn.lastHeard = Date.now();
      const data = e.data;
      if (typeof data === "string") {
        // Transport control JSON. Strings are ALWAYS control (netcode frames are binary),
        // so they are safe to process even AFTER attach -- this is what keeps rtt_ping/
        // rtt_pong (the real per-peer ping) and the local-only kick working once a peer has
        // joined. Never surfaced to the netcode.
        try {
          this.onControl?.(peerId, JSON.parse(data));
        } catch {
          /* malformed control frame: ignore */
        }
        return;
      }
      // Binary frame (binaryType='arraybuffer' so binary arrives as an ArrayBuffer).
      if (!(data instanceof ArrayBuffer)) return;
      const bytes = new Uint8Array(data);
      if (conn.attached) {
        const channel = CHANNEL_TO_LABEL.indexOf(label);
        if (channel >= 0) this.onNetFrame?.(peerId, channel, bytes);
      } else if (label === "duke-bulk") {
        this.onBulkChunk?.(peerId, bytes); // GRP transfer chunk (pre-attach)
      }
      // else: unexpected binary on move/rel pre-attach — ignore.
    };
    dc.onclose = () => {
      if (conn.dcs.get(label) === dc) conn.dcs.delete(label);
    };
  }

  /** Begin connecting to a peer. Only the smaller device id actually offers (glare
   *  avoidance); the larger waits for the offer. STAR topology is the CALLER's
   *  responsibility (which peers this is invoked for). */
  connect(peerId: string, encKey: string, relays: readonly string[]): void {
    const conn = this._ensure(peerId, encKey, relays);
    if (DEVICE_ID >= peerId) return; // only the smaller device id offers (glare avoidance)
    const anyOpen = DC_LABELS.some((l) => conn.dcs.get(l)?.readyState === "open");
    if (anyOpen) return;
    // Do NOT stack offers. connect() is called on every ~5s presence tick; without this
    // guard each call re-ran _createOffer, creating a SECOND (third, ...) set of data
    // channels on the same pc and renegotiating -- duplicate channels piled up and wedged
    // the handshake, which is the "Connecting to host" stall. Offer only when the
    // connection is fresh (no channels yet, signalingState stable). A genuinely lost offer
    // is retried on a fresh pc by _scheduleReconnect once ICE fails.
    if (conn.dcs.size > 0 || conn.pc.signalingState !== "stable") return;
    this._createOffer(peerId).catch(() => {});
  }

  private async _createOffer(peerId: string): Promise<void> {
    const conn = this._conns.get(peerId);
    if (!conn) return;
    // Never renegotiate a live pair: a reconnect timer can fire after the pc
    // recovered on its own (disconnected-grace), and a second channel set on the
    // same pc wedges the handshake (see connect()).
    if (conn.dcs.size > 0 || conn.pc.signalingState !== "stable" || conn.pc.connectionState === "connected") return;
    // The offerer creates all three channels in one SDP; the answerer receives them
    // via ondatachannel and matches by label.
    for (const label of DC_LABELS) this._setupDc(peerId, conn.pc.createDataChannel(label, DC_INIT[label]));
    const offer = await conn.pc.createOffer();
    await conn.pc.setLocalDescription(offer);
    // Non-trickle: wait for candidates so the SDP is self-contained, then resend it until
    // connected so a lost ephemeral offer self-heals instead of stalling.
    await this._awaitIceGathering(conn.pc);
    const send = () =>
      void sendOffer(conn.encKey, peerId, { type: "offer", sdp: conn.pc.localDescription?.sdp ?? offer.sdp }, conn.relays).catch(() => {});
    send();
    console.log(`[dnet] -> offer sent to ${peerId.slice(0, 8)}`);
    this._armResend(peerId, send);
  }

  async handleOffer(peerId: string, offer: RTCSessionDescriptionInit, encKey: string, relays: readonly string[]): Promise<void> {
    // Offer GENERATIONS, keyed by the offer's own ice-ufrag (mirrors the native
    // host's nn_peer logic — the two sides must agree on this protocol):
    //   * same ufrag we already answered  -> resend that answer, paired (gen) to
    //     THE OFFER IT ANSWERS. The old code stamped the OLD answer with the NEW
    //     offer's ufrag, defeating the receiver's staleness guard and poisoning
    //     a reconnecting peer's fresh pc (wrong DTLS fingerprint, ~6s death loop).
    //   * a previously-seen other ufrag   -> a relay redelivered a superseded
    //     generation; answering would resurrect a dead session. Ignore.
    //   * a never-seen ufrag on a conn we already negotiated -> the peer rebuilt
    //     its pc (reload / reconnect). Retire ours, negotiate fresh.
    const ufrag = sdpUfrag(offer.sdp);
    const existing = this._conns.get(peerId);
    if (existing && existing.remoteDescSet && ufrag === existing.offerUfrag) {
      if (existing.pc.localDescription?.type === "answer")
        void sendAnswer(encKey, peerId, { type: "answer", sdp: existing.pc.localDescription.sdp }, relays, existing.offerUfrag).catch(() => {});
      return;
    }
    const seen = this._seenOfferUfrags.get(peerId) ?? [];
    if (seen.includes(ufrag)) return; // superseded generation: never resurrect it
    if (existing && existing.remoteDescSet) {
      console.log(`[dnet] re-offer from ${peerId.slice(0, 8)}: retiring stale conn, negotiating fresh`);
      this._cleanup(peerId, true);
      this.onConnectionChange?.(peerId, "closed"); // upstream frees seat/slot state
    }
    const conn = this._ensure(peerId, encKey, relays);
    console.log(`[dnet] <- offer from ${peerId.slice(0, 8)}, answering`);
    try {
      await conn.pc.setRemoteDescription(new RTCSessionDescription(offer));
      conn.remoteDescSet = true;
      conn.offerUfrag = ufrag;
      seen.push(ufrag);
      if (seen.length > 32) seen.shift();
      this._seenOfferUfrags.set(peerId, seen);
      await this._flushIce(peerId);
      const answer = await conn.pc.createAnswer();
      await conn.pc.setLocalDescription(answer);
      await this._awaitIceGathering(conn.pc);
      const send = () =>
        void sendAnswer(encKey, peerId, { type: "answer", sdp: conn.pc.localDescription?.sdp ?? answer.sdp }, relays, ufrag).catch(() => {});
      send();
      this._armResend(peerId, send);
    } catch (e) {
      // A malformed/mismatched SDP must never kill the signaling receive path.
      console.log(`[dnet] handleOffer ${peerId.slice(0, 8)} failed: ${String(e)}`);
    }
  }

  async handleAnswer(peerId: string, answer: RTCSessionDescriptionInit, gen?: string): Promise<void> {
    const conn = this._conns.get(peerId);
    if (!conn) return;
    if (conn.pc.signalingState === "stable") return; // duplicate answer (resend); already applied
    // Generation pairing: relays redeliver old answers for minutes; an answer
    // tagged for a different offer than ours belongs to a dead session and
    // would poison this pc (wrong DTLS fingerprint -> channels never open).
    if (gen && sdpUfrag(conn.pc.localDescription?.sdp) !== gen) {
      console.log(`[dnet] <- stale answer from ${peerId.slice(0, 8)} (gen mismatch) — ignored`);
      return;
    }
    console.log(`[dnet] <- answer from ${peerId.slice(0, 8)}`);
    this._clearResend(peerId); // answer arrived; stop resending our offer
    await conn.pc.setRemoteDescription(new RTCSessionDescription(answer));
    conn.remoteDescSet = true;
    await this._flushIce(peerId);
  }

  async addIceCandidate(peerId: string, candidate: RTCIceCandidateInit): Promise<void> {
    const conn = this._conns.get(peerId);
    if (!conn) return;
    if (!conn.remoteDescSet) conn.iceQueue.push(candidate);
    else await conn.pc.addIceCandidate(new RTCIceCandidate(candidate)).catch(() => {});
  }

  private async _flushIce(peerId: string): Promise<void> {
    const conn = this._conns.get(peerId);
    if (!conn) return;
    for (const c of conn.iceQueue) await conn.pc.addIceCandidate(new RTCIceCandidate(c)).catch(() => {});
    conn.iceQueue = [];
  }

  // ── Phase control ────────────────────────────────────────────────────────

  /** Flip a peer between the transport lobby protocol and raw netcode frames.
   *  Called by duke-net once the join handshake completes (attach=true) or on
   *  leaving a match (attach=false). */
  setAttached(peerId: string, attached: boolean): void {
    const conn = this._conns.get(peerId);
    if (conn) conn.attached = attached;
  }

  isAttached(peerId: string): boolean {
    return this._conns.get(peerId)?.attached ?? false;
  }

  /** True once all three channels to the peer are open. */
  channelsReady(peerId: string): boolean {
    const conn = this._conns.get(peerId);
    return !!conn && DC_LABELS.every((l) => conn.dcs.get(l)?.readyState === "open");
  }

  // ── Sends ────────────────────────────────────────────────────────────────

  private _dc(peerId: string, label: DcLabel): RTCDataChannel | null {
    const dc = this._conns.get(peerId)?.dcs.get(label);
    return dc && dc.readyState === "open" ? dc : null;
  }

  /** Raw netcode frame to one peer on a logical channel (post-attach). Copies out
   *  of the caller's view; returns false if the channel is not open. */
  sendNet(peerId: string, channel: number, bytes: Uint8Array): boolean {
    const label = CHANNEL_TO_LABEL[channel];
    if (!label) return false;
    const dc = this._dc(peerId, label);
    if (!dc) return false;
    try {
      const buf = bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer;
      // TEST HARNESS ONLY (?lossmove=<pct>&jitmove=<ms>): synthetic loss/reorder
      // on the duke-move channel, so the tic-indexed protocol's repair paths can
      // be soaked on a loopback bench where the real wire never drops anything.
      // Loopback DCs are lossless, so without this the loss tolerance the
      // protocol exists for would ship untested.
      if (channel === 0 && (TEST_MOVE_LOSS > 0 || TEST_MOVE_JITTER > 0)) {
        if (TEST_MOVE_LOSS > 0 && Math.random() * 100 < TEST_MOVE_LOSS) return true; // "sent" (dropped)
        if (TEST_MOVE_JITTER > 0 && Math.random() < 0.5) {
          const delay = Math.random() * TEST_MOVE_JITTER;
          setTimeout(() => {
            try {
              if (dc.readyState === "open") dc.send(buf);
            } catch { /* channel died while delayed: same as a drop */ }
          }, delay);
          return true;
        }
      }
      dc.send(buf);
      return true;
    } catch {
      return false;
    }
  }

  /** JSON control message (pre-attach) over duke-rel — or any channel: strings
   *  are control on EVERY channel (see onmessage). GRP framing (grp_begin/end)
   *  must ride duke-bulk WITH the chunks: separate SCTP streams are round-robin
   *  scheduled, so a tiny control message on duke-rel OVERTAKES megabytes of
   *  queued chunks — grp_end then reached the receiver mid-stream and the
   *  transfer "failed verification" at whatever fraction had landed
   *  (live-reported dying at 64-68%). Same-stream ordering ends that race. */
  sendControl(peerId: string, msg: unknown, label: DcLabel = "duke-rel"): boolean {
    const dc = this._dc(peerId, label);
    if (!dc) return false;
    try {
      dc.send(JSON.stringify(msg));
      return true;
    } catch {
      return false;
    }
  }

  /** A GRP transfer chunk (pre-attach) over duke-bulk (raw bytes). */
  sendBulkChunk(peerId: string, bytes: Uint8Array): boolean {
    const dc = this._dc(peerId, "duke-bulk");
    if (!dc) return false;
    try {
      dc.send(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer);
      return true;
    } catch {
      return false;
    }
  }

  /** Amount of duke-bulk data still queued (backpressure gate for GRP streaming). */
  bulkBufferedAmount(peerId: string): number {
    return this._conns.get(peerId)?.dcs.get("duke-bulk")?.bufferedAmount ?? 0;
  }

  isConnected(peerId: string): boolean {
    const conn = this._conns.get(peerId);
    return !!conn && conn.pc.connectionState === "connected" && this.channelsReady(peerId);
  }

  connectedPeers(): string[] {
    return [...this._conns.keys()].filter((id) => this.channelsReady(id));
  }

  msSinceHeard(peerId: string): number {
    const conn = this._conns.get(peerId);
    if (!conn || !this.channelsReady(peerId)) return Infinity;
    return Date.now() - conn.lastHeard;
  }

  /** True if the CURRENT conn to this peer has ever had all three channels open.
   *  Distinguishes "never completed a handshake" (evictable as stuck) from "was
   *  connected, currently flapping" (recoverable — must NOT be evicted). */
  everReady(peerId: string): boolean {
    return this._conns.get(peerId)?.readyFired ?? false;
  }

  /** Age of the current conn's pc in ms (Infinity when no conn exists), for
   *  never-connected eviction timers. A replacement conn restarts the clock. */
  connAgeMs(peerId: string): number {
    const conn = this._conns.get(peerId);
    return conn ? Date.now() - conn.created : Infinity;
  }

  /** Resolve when ICE gathering completes (all candidates in the SDP) or a timeout, so
   *  the offer/answer we (re)send is self-contained. Bounded so a slow/blocked STUN
   *  server cannot hang the handshake. Polls to stay portable across WebRTC impls. */
  private _awaitIceGathering(pc: RTCPeerConnection, timeoutMs = 2500): Promise<void> {
    return new Promise((resolve) => {
      if (pc.iceGatheringState === "complete") return resolve();
      const t0 = Date.now();
      const iv = setInterval(() => {
        if (pc.iceGatheringState === "complete" || Date.now() - t0 > timeoutMs) { clearInterval(iv); resolve(); }
      }, 80);
    });
  }

  /** Resend a signaling payload on a short timer until a channel opens (bounded), so a
   *  lost ephemeral offer/answer self-heals instead of stalling. */
  private _armResend(peerId: string, send: () => void): void {
    const conn = this._conns.get(peerId);
    if (!conn) return;
    if (conn.resendTimer) clearInterval(conn.resendTimer);
    let attempts = 0;
    conn.resendTimer = setInterval(() => {
      const c = this._conns.get(peerId);
      if (!c) return;
      if (DC_LABELS.some((l) => c.dcs.get(l)?.readyState === "open") || ++attempts > RESEND_MAX) {
        this._clearResend(peerId);
        return;
      }
      send();
    }, RESEND_INTERVAL_MS);
  }

  private _clearResend(peerId: string): void {
    const conn = this._conns.get(peerId);
    if (conn?.resendTimer) { clearInterval(conn.resendTimer); conn.resendTimer = null; }
  }

  private _scheduleReconnect(peerId: string): void {
    const conn = this._conns.get(peerId);
    if (!conn || conn.reconnectTimer) return;
    if (DEVICE_ID > peerId) return; // only the initiator reconnects
    const delay = conn.backoff;
    conn.backoff = Math.min(conn.backoff * 2, MAX_BACKOFF_MS);
    conn.reconnectTimer = setTimeout(() => {
      conn.reconnectTimer = null;
      const cur = this._conns.get(peerId);
      if (cur && cur.pc.connectionState === "connected") return; // recovered on its own
      // A stuck-disconnected pc would be REUSED by _ensure (deliberately, see
      // above) — but a reconnect needs a genuinely fresh pc, so retire it first.
      if (cur) this._cleanup(peerId, true);
      this._ensure(peerId, conn.encKey, conn.relays);
      this._createOffer(peerId).catch(() => {});
    }, delay);
  }

  private _cleanup(peerId: string, remove = true): void {
    const conn = this._conns.get(peerId);
    if (!conn) return;
    if (conn.reconnectTimer) clearTimeout(conn.reconnectTimer);
    if (conn.resendTimer) clearInterval(conn.resendTimer);
    if (conn.discoTimer) clearTimeout(conn.discoTimer);
    for (const dc of conn.dcs.values())
      try {
        dc.close();
      } catch {
        /* already closed */
      }
    try {
      conn.pc.close();
    } catch {
      /* already closed */
    }
    if (remove) this._conns.delete(peerId);
  }

  close(peerId: string): void {
    this._cleanup(peerId, true);
    this._seenOfferUfrags.delete(peerId); // deliberate close: forget generations too
    this.onConnectionChange?.(peerId, "closed");
  }

  closeAll(): void {
    for (const id of [...this._conns.keys()]) this._cleanup(id, true);
    this._seenOfferUfrags.clear();
  }
}
