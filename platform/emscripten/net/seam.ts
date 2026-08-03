// ─────────────────────────────────────────────────────────────────────────────
// SEAM — the runtime half of the net_transport.h bridge.
//
// The C engine's transport functions live in net/seam_library.js (an Emscripten
// --js-library linked INTO the wasm). Those tiny shims forward to the methods this
// class exposes on window.DukeNet:
//
//   C net_send(peerToken, channel, reliable, data, len)  -> Seam.send(...)
//   C net_broadcast(channel, reliable, data, len)        -> Seam.broadcast(...)
//   C net_poll()                                         -> Seam.drain()  (JS->C:
//                                                            Net_ReceiveFrame /
//                                                            Net_PeerEvent per item)
//   C net_transport_init()      -> Seam.init()
//   C net_transport_shutdown()  -> Seam.shutdown()
//
// peerToken == the Duke connectindex. The host assigns tokens during the join
// handshake (duke-net.ts); this class holds the token<->deviceId map so outbound
// sends resolve a token to a peer and inbound frames resolve a peer to its token.
//
// Ordering guarantee: peer-up/-down events and frames are delivered to the netcode
// in the exact order they were enqueued (one FIFO), so a NET_PEER_UP always
// precedes that peer's first frame and a NET_PEER_DOWN follows its last.
// ─────────────────────────────────────────────────────────────────────────────

/** net_channel_t (mirrors the frozen header). */
export const NET_CHAN_MOVE = 0;
export const NET_CHAN_REL = 1;
export const NET_CHAN_BULK = 2;

/** net_peerevent_t. */
export const NET_PEER_DOWN = 0;
export const NET_PEER_UP = 1;

/** An item drained to net_poll. kind 0 = frame, kind 1 = peer event. */
export type SeamItem =
  | { kind: 0; peer: number; channel: number; data: Uint8Array }
  | { kind: 1; peer: number; event: number };

/** The actual wire send, injected by duke-net (routes to the right peer + channel).
 *  Returns false if the peer/channel is not deliverable. */
export type SeamSender = (deviceId: string, channel: number, bytes: Uint8Array) => boolean;

export class Seam {
  private inbound: SeamItem[] = [];
  private tokenToDevice = new Map<number, string>();
  private deviceToToken = new Map<string, number>();
  private sender: SeamSender | null = null;
  private _active = false;

  /** True between net_transport_init() and net_transport_shutdown(). */
  get active(): boolean {
    return this._active;
  }

  setSender(fn: SeamSender | null): void {
    this.sender = fn;
  }

  // ── token <-> device mapping (driven by the join handshake) ────────────────

  registerPeer(deviceId: string, token: number): void {
    this.tokenToDevice.set(token, deviceId);
    this.deviceToToken.set(deviceId, token);
  }

  unregisterPeer(deviceId: string): void {
    const t = this.deviceToToken.get(deviceId);
    if (t !== undefined) this.tokenToDevice.delete(t);
    this.deviceToToken.delete(deviceId);
  }

  tokenOf(deviceId: string): number | undefined {
    return this.deviceToToken.get(deviceId);
  }

  deviceOf(token: number): string | undefined {
    return this.tokenToDevice.get(token);
  }

  connectedTokens(): number[] {
    return [...this.tokenToDevice.keys()];
  }

  // ── Inbound (WebRTC -> queue -> net_poll -> C) ─────────────────────────────

  /** A raw netcode frame arrived from a peer (post-attach). Enqueued for net_poll. */
  enqueueFrameByDevice(deviceId: string, channel: number, bytes: Uint8Array): void {
    const peer = this.deviceToToken.get(deviceId);
    if (peer === undefined) return; // not a registered game peer: drop
    // Copy: the caller's view may be backed by a buffer that is reused.
    this.inbound.push({ kind: 0, peer, channel, data: bytes.slice() });
  }

  /** A peer became a game participant (up) or left (down). Enqueued in order. */
  enqueuePeerEventByDevice(deviceId: string, up: boolean): void {
    const peer = this.deviceToToken.get(deviceId);
    if (peer === undefined) return;
    this.inbound.push({ kind: 1, peer, event: up ? NET_PEER_UP : NET_PEER_DOWN });
  }

  /** Drain all queued inbound items (frames + events) in arrival order. Called by
   *  the js-library's net_poll implementation, which then invokes the C entrypoints
   *  Net_ReceiveFrame / Net_PeerEvent for each. */
  drain(): SeamItem[] {
    if (this.inbound.length === 0) return [];
    const out = this.inbound;
    this.inbound = [];
    return out;
  }

  // ── Outbound (C -> here -> WebRTC) ─────────────────────────────────────────

  /** net_send: to a single peer by token. Bytes are already copied by the js-library
   *  out of the wasm heap. */
  send(peerToken: number, channel: number, _reliable: number, bytes: Uint8Array): void {
    const dev = this.tokenToDevice.get(peerToken);
    if (dev === undefined || !this.sender) return;
    this.sender(dev, channel, bytes);
  }

  /** net_broadcast: to every registered peer (never loops back to self — self is
   *  never in the token map). */
  broadcast(channel: number, _reliable: number, bytes: Uint8Array): void {
    if (!this.sender) return;
    for (const dev of this.tokenToDevice.values()) this.sender(dev, channel, bytes);
  }

  // ── Lifecycle ──────────────────────────────────────────────────────────────

  init(): void {
    this._active = true;
  }

  shutdown(): void {
    this._active = false;
    this.inbound = [];
    this.tokenToDevice.clear();
    this.deviceToToken.clear();
  }
}
