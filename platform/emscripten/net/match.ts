// ─────────────────────────────────────────────────────────────────────────────
// MATCH (Duke fork) — a room: presence-driven WebRTC STAR + private invites /
// public listing, carrying the GRP fingerprint that gates joins.
//
// Forked from the scorchedearth-multi Match. Duke changes:
//   * MatchInfo carries a match NAME, a GRP fingerprint (join gating + transfer
//     eligibility), and a ping hint (list-wide relay-RTT proxy).
//   * STAR topology: a guest connects ONLY to the host; the host connects to every
//     guest. No guest-guest links. The host also plays (listen server).
//   * Transport callbacks (control / bulk / netframe / channelsReady) are forwarded
//     up to duke-net.ts, which owns the join handshake, GRP transfer, and the seam.
//   * The lockstep/turn-broadcast half of the source Match is intentionally dropped
//     — Duke has its own netcode above the seam.
// ─────────────────────────────────────────────────────────────────────────────
import {
  activeRelays,
  PUBLIC_LOBBY_KEY,
  LOBBY_KIND,
  PRESENCE_INTERVAL_MS,
  ANNOUNCE_INTERVAL_MS,
  ANNOUNCE_TTL_MS,
  PROTOCOL_VERSION,
} from "./netconfig";
import { DEVICE_ID, uid } from "./identity";
import { ensureRelays, generateRoomKey, publishReplaceable, subscribeReplaceable } from "./nostr";
import { subscribeSignaling, sendPresence } from "./signaling";
import { PeerManager, type ConnState } from "./peer";
import type { GrpFingerprint } from "./grp";
import { sanitizeText, sanitizeName } from "./sanitize";

export type Role = "host" | "guest";
export type MatchStatus = "open" | "starting" | "playing";

/** Capacity predicate (pure, unit-tested): does an OPEN match with `rosterSize`
 *  members (self included) have room for one more under `maxPlayers`? A match that
 *  has started (status !== "open") has room for nobody new. */
export function matchHasOpenSlot(rosterSize: number, maxPlayers: number, status: MatchStatus): boolean {
  return status === "open" && rosterSize < maxPlayers;
}

export interface MatchInfo {
  v: number;
  matchId: string;
  name: string; // sanitized match name
  hostId: string; // host device id (STAR center)
  roomKey: string; // AES room key (present in public announcements + invite codes)
  maxPlayers: number;
  players: number;
  status: MatchStatus;
  grp: GrpFingerprint; // GRP set fingerprint — join gating + transfer eligibility
  pingHint: number | null; // host median relay RTT (ms) for the list-wide proxy
  localOnly?: boolean; // host boots guests whose real (data-channel) RTT exceeds the local threshold
  ts: number;
}

export interface RoomPlayer {
  deviceId: string;
  name: string;
  connected: boolean;
  lastSeen: number;
  /** When this peer first appeared in the roster. A peer still not connected
   *  CONNECT_TIMEOUT_MS after this is dropped (host side) so a failed WebRTC pair
   *  can't squat a lobby row as "(CONNECTING)" forever. */
  firstSeen: number;
}

/** How long the host tolerates a roster member whose WebRTC pair never completes. */
export const CONNECT_TIMEOUT_MS = 60_000;

export interface MatchHandlers {
  onRoster?: (players: RoomPlayer[]) => void;
  onControl?: (peerId: string, msg: unknown) => void;
  onBulkChunk?: (peerId: string, bytes: Uint8Array) => void;
  onNetFrame?: (peerId: string, channel: number, bytes: Uint8Array) => void;
  onChannelsReady?: (peerId: string) => void;
  onConnection?: (peerId: string, state: ConnState) => void;
}

interface MatchInit {
  role: Role;
  roomKey: string;
  matchId: string;
  name: string;
  hostId: string;
  maxPlayers: number;
  isPublic: boolean;
  myName: string;
  grp: GrpFingerprint;
  pingHint: number | null;
  localOnly: boolean;
  relays: readonly string[];
}

export class Match {
  readonly role: Role;
  readonly roomKey: string;
  readonly matchId: string;
  readonly name: string;
  readonly hostId: string;
  readonly maxPlayers: number;
  readonly isPublic: boolean;
  readonly localOnly: boolean;
  readonly relays: readonly string[];
  readonly peers = new PeerManager();
  private readonly myName: string;
  private grp: GrpFingerprint;
  private pingHint: number | null;
  private readonly roster = new Map<string, RoomPlayer>();
  handlers: MatchHandlers = {};

  private _presenceTimer: ReturnType<typeof setInterval> | null = null;
  private _announceTimer: ReturnType<typeof setInterval> | null = null;
  private _pruneTimer: ReturnType<typeof setInterval> | null = null;
  private _unsubSignaling: (() => void) | null = null;
  private _status: MatchStatus = "open";
  private _announceScheduled = false;

  private constructor(init: MatchInit) {
    this.role = init.role;
    this.roomKey = init.roomKey;
    this.matchId = init.matchId;
    this.name = init.name;
    this.hostId = init.hostId;
    this.maxPlayers = init.maxPlayers;
    this.isPublic = init.isPublic;
    this.localOnly = init.localOnly;
    this.peers.localOnly = init.localOnly; // ICE policy: LAN-only pairs when set
    this.relays = init.relays;
    this.myName = init.myName;
    this.grp = init.grp;
    this.pingHint = init.pingHint;
    this.roster.set(DEVICE_ID, { deviceId: DEVICE_ID, name: this.myName, connected: true, lastSeen: Date.now(), firstSeen: Date.now() });
  }

  // ── Factories ────────────────────────────────────────────────────────────

  static async createPrivate(name: string, maxPlayers: number, myName: string, grp: GrpFingerprint, pingHint: number | null, localOnly = false, relays = activeRelays()): Promise<Match> {
    const m = new Match({
      role: "host",
      roomKey: await generateRoomKey(),
      matchId: uid(),
      name: sanitizeText(name) || "Duke Match",
      hostId: DEVICE_ID,
      maxPlayers,
      isPublic: false,
      myName: sanitizeName(myName),
      grp,
      pingHint,
      localOnly,
      relays,
    });
    await m._open();
    return m;
  }

  static async createPublic(name: string, maxPlayers: number, myName: string, grp: GrpFingerprint, pingHint: number | null, localOnly = false, relays = activeRelays()): Promise<Match> {
    const m = new Match({
      role: "host",
      roomKey: await generateRoomKey(),
      matchId: uid(),
      name: sanitizeText(name) || "Duke Match",
      hostId: DEVICE_ID,
      maxPlayers,
      isPublic: true,
      myName: sanitizeName(myName),
      grp,
      pingHint,
      localOnly,
      relays,
    });
    await m._open();
    await m._announce();
    m._announceTimer = setInterval(() => void m._announce(), ANNOUNCE_INTERVAL_MS);
    return m;
  }

  static async join(info: MatchInfo, myName: string, grp: GrpFingerprint, relays = activeRelays()): Promise<Match> {
    const m = new Match({
      role: "guest",
      roomKey: info.roomKey,
      matchId: info.matchId,
      name: sanitizeText(info.name) || "Duke Match",
      hostId: info.hostId,
      maxPlayers: info.maxPlayers,
      isPublic: false,
      myName: sanitizeName(myName),
      grp,
      pingHint: null,
      localOnly: info.localOnly ?? false,
      relays,
    });
    await m._open();
    // Star: proactively dial the host (we already know its id from the invite/list).
    m.peers.connect(info.hostId, m.roomKey, m.relays);
    return m;
  }

  // ── Invite codes (private match) ───────────────────────────────────────────

  info(): MatchInfo {
    return {
      v: PROTOCOL_VERSION,
      matchId: this.matchId,
      name: this.name,
      hostId: this.hostId,
      roomKey: this.roomKey,
      maxPlayers: this.maxPlayers,
      players: this.roster.size,
      status: this._status,
      grp: this.grp,
      pingHint: this.pingHint,
      localOnly: this.localOnly,
      ts: Date.now(),
    };
  }

  inviteCode(): string {
    return btoa(JSON.stringify(this.info())).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
  }

  static parseInvite(code: string): MatchInfo | null {
    try {
      const json = atob(code.trim().replace(/-/g, "+").replace(/_/g, "/"));
      const info = JSON.parse(json) as MatchInfo;
      if (!info.roomKey || !info.matchId || !info.hostId || !info.grp) return null;
      return info;
    } catch {
      return null;
    }
  }

  // ── Lifecycle ──────────────────────────────────────────────────────────────

  private async _open(): Promise<void> {
    this.peers.onControl = (peerId, msg) => this.handlers.onControl?.(peerId, msg);
    this.peers.onBulkChunk = (peerId, bytes) => this.handlers.onBulkChunk?.(peerId, bytes);
    this.peers.onNetFrame = (peerId, channel, bytes) => this.handlers.onNetFrame?.(peerId, channel, bytes);
    this.peers.onChannelsReady = (peerId) => {
      const p = this.roster.get(peerId);
      if (p) p.connected = true;
      this.handlers.onChannelsReady?.(peerId);
      this._emitRoster();
    };
    this.peers.onConnectionChange = (peerId, state) => {
      const p = this.roster.get(peerId);
      if (p) p.connected = this.peers.isConnected(peerId);
      this.handlers.onConnection?.(peerId, state);
      this._emitRoster();
    };

    this._unsubSignaling = await subscribeSignaling(
      this.roomKey,
      {
        onOffer: (from, sdp) => {
          // The HOST answers every room-keyed offer, even once playing: the guest
          // needs open channels to RECEIVE the authoritative join_deny ("started"/
          // "full") from _hostHandleJoin. Refusing at the signal layer left late
          // joiners stuck on "CONNECTING TO HOST" with no feedback (live-reported).
          // Guests still answer only the host (STAR).
          if (this.role === "host" || this._acceptsPeer(from)) void this.peers.handleOffer(from, sdp, this.roomKey, this.relays);
        },
        onAnswer: (from, sdp) => void this.peers.handleAnswer(from, sdp),
        onIce: (from, c) => void this.peers.addIceCandidate(from, c),
        onPresence: (from, name) => this._onPresence(from, name),
      },
      this.relays,
    );

    void sendPresence(this.roomKey, this.myName, this.relays);
    // Presence burst: re-announce a few times in the first seconds so a peer is discovered
    // quickly even if the first presence is lost on a flaky relay (the steady interval
    // below is only every 5 s, and the host only offers once it has heard the guest).
    for (const t of [900, 2200, 4200]) setTimeout(() => void sendPresence(this.roomKey, this.myName, this.relays), t);
    this._presenceTimer = setInterval(() => void sendPresence(this.roomKey, this.myName, this.relays), PRESENCE_INTERVAL_MS);
    this._pruneTimer = setInterval(() => this._prune(), PRESENCE_INTERVAL_MS);
    this._emitRoster();
  }

  /** STAR + capacity gate: a guest accepts ONLY the host. The host accepts a peer it
   *  already knows (reconnect) and, for a NEW peer, only while the match is open and
   *  has a free slot — so a full or in-progress game stops taking joiners. */
  private _acceptsPeer(peerId: string): boolean {
    if (this.role !== "host") return peerId === this.hostId;
    return this.roster.has(peerId) || matchHasOpenSlot(this.roster.size, this.maxPlayers, this._status);
  }

  private _onPresence(from: string, name: string): void {
    const nm = sanitizeName(name);
    const existing = this.roster.get(from);
    if (existing) {
      existing.name = nm;
      existing.lastSeen = Date.now();
      existing.connected = this.peers.isConnected(from);
    } else {
      // Host capacity/started gate: admit a NEW joiner only while open with a free
      // slot. Guests keep tracking every room member for the roster display; they
      // only ever *connect* to the host (see _acceptsPeer).
      if (this.role === "host" && !matchHasOpenSlot(this.roster.size, this.maxPlayers, this._status)) {
        // No seat -- but still DIAL (don't add to the roster): open channels are the
        // only path for the authoritative join_deny ("started"/"full"), and glare
        // avoidance may put the offer on OUR side. Without this a late knocker sat
        // on "CONNECTING TO HOST" forever (live-reported). The deny closes the pair.
        this.peers.connect(from, this.roomKey, this.relays);
        return;
      }
      this.roster.set(from, { deviceId: from, name: nm, connected: this.peers.isConnected(from), lastSeen: Date.now(), firstSeen: Date.now() });
      if (this.isPublic) this._announceSoon(); // player count changed -> refresh the advert now
    }
    // STAR: host dials every guest; a guest dials only the host. PeerManager.connect
    // only offers when our id is smaller, so exactly one side of the pair initiates.
    if (this._acceptsPeer(from)) this.peers.connect(from, this.roomKey, this.relays);
    this._emitRoster();
  }

  private _prune(): void {
    const cutoff = Date.now() - PRESENCE_INTERVAL_MS * 3;
    const connectCutoff = Date.now() - CONNECT_TIMEOUT_MS;
    let changed = false;
    for (const [id, p] of this.roster) {
      if (id === DEVICE_ID) continue;
      if (p.lastSeen < cutoff && !this.peers.isConnected(id)) {
        this.roster.delete(id);
        changed = true;
      }
      // Presence keeps refreshing lastSeen, so a peer whose WebRTC pair never
      // completes would otherwise sit in the lobby as "(CONNECTING)" forever
      // (live-reported "1. DUKE (CONNECTING)"). The host evicts it after
      // CONNECT_TIMEOUT_MS; if the peer is really there its presence re-adds it
      // and the connect gets a fresh start.
      else if (this.role === "host" && !this.peers.isConnected(id) && p.firstSeen < connectCutoff) {
        console.log(`[dnet] dropping ${id.slice(0, 8)}: no WebRTC connection after ${Math.round(CONNECT_TIMEOUT_MS / 1000)}s`);
        this.peers.close(id);
        this.roster.delete(id);
        changed = true;
      }
    }
    if (changed) {
      this._emitRoster();
      if (this.isPublic) this._announceSoon(); // a player dropped -> refresh the advert
    }
  }

  private async _announce(): Promise<void> {
    if (!this.isPublic) return;
    await publishReplaceable(LOBBY_KIND, PUBLIC_LOBBY_KEY, this.matchId, this.info(), this.relays);
  }

  /** Coalesced re-announce: a roster/state change refreshes the public advert within
   *  ~500ms instead of waiting for the 15s tick, so the listed player count and the
   *  open/full state track reality. No-op for private matches. */
  private _announceSoon(): void {
    if (!this.isPublic || this._announceScheduled) return;
    this._announceScheduled = true;
    setTimeout(() => {
      this._announceScheduled = false;
      void this._announce();
    }, 500);
  }

  private _emitRoster(): void {
    this.handlers.onRoster?.(this.players());
  }

  // ── Public API for duke-net / UI ───────────────────────────────────────────

  players(): RoomPlayer[] {
    return [...this.roster.values()].sort((a, b) => a.deviceId.localeCompare(b.deviceId));
  }

  guestIds(): string[] {
    return this.players().map((p) => p.deviceId).filter((id) => id !== DEVICE_ID);
  }

  status(): MatchStatus {
    return this._status;
  }

  setStatus(status: MatchStatus): void {
    this._status = status;
    if (this.isPublic) void this._announce();
  }

  /** Update the advertised GRP fingerprint (e.g. the host changed its in-play GRP). */
  setGrp(grp: GrpFingerprint): void {
    this.grp = grp;
    if (this.isPublic) void this._announce();
  }

  setPingHint(ms: number | null): void {
    this.pingHint = ms;
  }

  grpFingerprint(): GrpFingerprint {
    return this.grp;
  }

  leave(): void {
    if (this._presenceTimer) clearInterval(this._presenceTimer);
    if (this._announceTimer) clearInterval(this._announceTimer);
    if (this._pruneTimer) clearInterval(this._pruneTimer);
    this._unsubSignaling?.();
    this.peers.closeAll();
    if (this.isPublic) {
      this._status = "playing"; // not "open" -> filtered out of the public list
      void this._announce();    // publish the final non-open record NOW so consumers drop it immediately instead of waiting the 60s TTL
    }
  }
}

// ── Public match discovery ─────────────────────────────────────────────────────

/** Live list of open public matches (fresher than ANNOUNCE_TTL). Returns an unsub. */
export async function listPublicMatches(onUpdate: (matches: MatchInfo[]) => void): Promise<() => void> {
  const seen = new Map<string, MatchInfo>();
  const emit = () => {
    const now = Date.now();
    for (const [id, m] of seen) if (now - m.ts >= ANNOUNCE_TTL_MS) seen.delete(id);
    const live = [...seen.values()].filter((m) => m.status === "open");
    live.sort((a, b) => b.ts - a.ts);
    onUpdate(live);
  };
  const unsub = await subscribeReplaceable<MatchInfo>(LOBBY_KIND, PUBLIC_LOBBY_KEY, (rec) => {
    // Defensive validation: a public announcement is world-writable. Require the
    // fields duke-net needs; sanitize the name at the presentation layer.
    if (rec.data?.matchId && rec.data.roomKey && rec.data.hostId && rec.data.grp) {
      seen.set(rec.data.matchId, rec.data);
      emit();
    }
  });
  const tick = setInterval(emit, ANNOUNCE_INTERVAL_MS);
  return () => {
    clearInterval(tick);
    unsub();
  };
}

// ── Eager lobby warmup ─────────────────────────────────────────────────────────

type WarmLobby = { unsub: () => void; matches: MatchInfo[] };
let _warm: WarmLobby | null = null;
let _warmHook: ((ms: MatchInfo[]) => void) | null = null;

/** Connect the relay pool and start public-match discovery NOW. Idempotent. */
export function warmPublicLobby(): void {
  if (_warm) return;
  const w: WarmLobby = { unsub: () => {}, matches: [] };
  _warm = w;
  ensureRelays(activeRelays());
  void listPublicMatches((ms) => {
    w.matches = ms;
    if (_warm === w) _warmHook?.(ms);
  }).then((unsub) => {
    if (_warm === w) w.unsub = unsub;
    else unsub();
  });
}

/** Adopt the warm discovery: re-point the update stream and return matches seen. */
export function adoptPublicLobby(onUpdate: (ms: MatchInfo[]) => void): MatchInfo[] {
  warmPublicLobby();
  _warmHook = onUpdate;
  return _warm ? _warm.matches.slice() : [];
}

export function releasePublicLobby(): void {
  _warmHook = null;
  _warm?.unsub();
  _warm = null;
}
