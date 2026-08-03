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

const MAX_BACKOFF_MS = 60_000;

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
  lastHeard: number;
}

export type ConnState = RTCPeerConnectionState;

export class PeerManager {
  private _conns = new Map<string, Conn>();

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
      if (s === "connected" || s === "connecting" || s === "new") return existing;
      this._cleanup(peerId, false);
    }
    const pc = new RTCPeerConnection(rtcConfig());
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
      lastHeard: Date.now(),
    };
    this._conns.set(peerId, conn);

    pc.onicecandidate = (e) => {
      if (e.candidate) sendIceCandidate(encKey, peerId, e.candidate, relays).catch(() => {});
    };
    pc.onconnectionstatechange = () => {
      const s = pc.connectionState;
      this.onConnectionChange?.(peerId, s);
      if (s === "connected") conn.backoff = 2000;
      else if (s === "failed" || s === "disconnected") this._scheduleReconnect(peerId);
    };
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
        // Transport control JSON (pre-attach only). Never surfaced to the netcode.
        if (conn.attached) return;
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
    const anyOpen = DC_LABELS.some((l) => conn.dcs.get(l)?.readyState === "open");
    if (DEVICE_ID < peerId && !anyOpen) this._createOffer(peerId).catch(() => {});
  }

  private async _createOffer(peerId: string): Promise<void> {
    const conn = this._conns.get(peerId);
    if (!conn) return;
    // The offerer creates all three channels in one SDP; the answerer receives them
    // via ondatachannel and matches by label.
    for (const label of DC_LABELS) this._setupDc(peerId, conn.pc.createDataChannel(label, DC_INIT[label]));
    const offer = await conn.pc.createOffer();
    await conn.pc.setLocalDescription(offer);
    await sendOffer(conn.encKey, peerId, offer, conn.relays);
  }

  async handleOffer(peerId: string, offer: RTCSessionDescriptionInit, encKey: string, relays: readonly string[]): Promise<void> {
    const conn = this._ensure(peerId, encKey, relays);
    await conn.pc.setRemoteDescription(new RTCSessionDescription(offer));
    conn.remoteDescSet = true;
    await this._flushIce(peerId);
    const answer = await conn.pc.createAnswer();
    await conn.pc.setLocalDescription(answer);
    await sendAnswer(encKey, peerId, answer, relays);
  }

  async handleAnswer(peerId: string, answer: RTCSessionDescriptionInit): Promise<void> {
    const conn = this._conns.get(peerId);
    if (!conn) return;
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
      dc.send(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength) as ArrayBuffer);
      return true;
    } catch {
      return false;
    }
  }

  /** JSON control message (pre-attach) over duke-rel. */
  sendControl(peerId: string, msg: unknown): boolean {
    const dc = this._dc(peerId, "duke-rel");
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

  private _scheduleReconnect(peerId: string): void {
    const conn = this._conns.get(peerId);
    if (!conn || conn.reconnectTimer) return;
    if (DEVICE_ID > peerId) return; // only the initiator reconnects
    const delay = conn.backoff;
    conn.backoff = Math.min(conn.backoff * 2, MAX_BACKOFF_MS);
    conn.reconnectTimer = setTimeout(() => {
      conn.reconnectTimer = null;
      this._ensure(peerId, conn.encKey, conn.relays);
      this._createOffer(peerId).catch(() => {});
    }, delay);
  }

  private _cleanup(peerId: string, remove = true): void {
    const conn = this._conns.get(peerId);
    if (!conn) return;
    if (conn.reconnectTimer) clearTimeout(conn.reconnectTimer);
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
    this.onConnectionChange?.(peerId, "closed");
  }

  closeAll(): void {
    for (const id of [...this._conns.keys()]) this._cleanup(id, true);
  }
}
