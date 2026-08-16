// ─────────────────────────────────────────────────────────────────────────────
// duke-net.ts — window.DukeNet: the public multiplayer facade AND the JS side of
// the net_transport.h seam. This is the bundle's entry point (esbuild builds it
// into eduke32-net.js).
//
// Responsibilities:
//   * Host a PUBLIC (listed) or PRIVATE (invite/QR) match; the host also plays.
//   * Join by invite code / URL / public-list row, GRP-gated at the join handshake.
//   * Serve the host's SINGLE in-play GRP to a joiner who lacks it, but ONLY when it
//     is shareable (firewall) AND the host has not opted out; the joiner verifies
//     the download against the advertised fingerprint before persisting + reloading.
//   * Drive the seam: outbound net_send/net_broadcast route to the right peer +
//     channel; inbound frames + peer up/down are queued for net_poll.
//
// The netcode (netduke32) is NOT wired in this branch, so the seam send/receive
// path cannot be exercised end-to-end here; it is written to the frozen header and
// unit-tested standalone. See docs/INTEGRATION.md and docs/INENGINE_MENU_SPEC.md.
// ─────────────────────────────────────────────────────────────────────────────

import { DEVICE_ID } from "./identity";
import { activeRelays } from "./netconfig";
import {
  Match,
  type MatchInfo,
  type RoomPlayer,
  listPublicMatches,
  warmPublicLobby,
  adoptPublicLobby,
  releasePublicLobby,
} from "./match";
import {
  type GrpFingerprint,
  type GrpComponent,
  type GrpOffer,
  GrpReceiver,
  prepareGrpSend,
  setFingerprint,
  fingerprintFsFile,
  scanFsGrps,
  type EmscriptenFS,
} from "./grp";
import { classifyByCrc } from "./grptable";
import { buildLobbyRows, type LobbyRow } from "./lobby";
import { Seam } from "./seam";
import { PingTracker, measureRelayRtt, PING_PRESETS } from "./ping";
import { sanitizeName, sanitizeText } from "./sanitize";
import { putGrp, getGrp } from "./idb";

// ── Control protocol (JSON over duke-rel, pre-attach only) ───────────────────

type Ctl =
  | { t: "join"; name: string; grp: GrpFingerprint }
  | { t: "join_ok"; yourSlot: number; hostSlot: number; name: string }
  | { t: "join_deny"; reason: "grpmismatch" | "full" | "closed" | "started"; hostGrp?: GrpFingerprint }
  | { t: "grp_req"; from?: number } // from = resume chunk index (optional; old hosts ignore it)
  | { t: "grp_begin"; offer: GrpOffer; from?: number } // from echoes the actual start (absent = 0)
  | { t: "grp_end" }
  | { t: "grp_deny"; reason: "paid" | "optout" | "notplaying" | "mismatch" }
  | { t: "sav_begin"; size: number; mask: number; plc?: number; join?: number; spawns?: string }
  | { t: "sav_chunk"; d: string }
  | { t: "sav_end" }
  | { t: "rtt_ping"; id: number }
  | { t: "rtt_pong"; id: number }
  | { t: "kick"; reason: string };

// Persistence keys:
//   SESSION_GRP_KEY (sessionStorage): the TRANSIENT boot-GRP override a join-driven
//     switch/download writes before reloading. Shapes (read by index.html preRun):
//       {crc, filename}             -> boot the verified GRP stored in eduke32-net IDB
//       {crc, filename, builtin:1}  -> boot a GRP already bundled in Module.FS
//       {main:1}                    -> boot the registered import (eduke32/mainGrp)
//     Session-scoped ON PURPOSE: joining a shareware match must not permanently
//     reset the player's chosen game — a later cold start boots their own choice.
//   GAMEGRP_KEY (localStorage): the player's DURABLE GRP choice. No longer written
//     by the join flow; still honored at boot (legacy installs / future selector)
//     and cleared whenever the Settings panel picks a main game.
//   REJOIN_KEY (sessionStorage): legacy; superseded by reloading into ?join=<blob>
//     (consumePendingJoin still reads it for older pages).
const SESSION_GRP_KEY = "eduke32-net-gamegrp-session";
const GAMEGRP_KEY = "eduke32-net-gamegrp";
const REJOIN_KEY = "eduke32-net-rejoin";
// One switch-and-reload attempt per host GRP crc per session: if we reload
// claiming to have the GRP and the host STILL denies us, the local copy is not
// what we thought — fall through to a real download instead of reload-looping.
const SWITCHTRY_KEY = "eduke32-net-switchtry";

// Local-only matches boot a guest whose real data-channel RTT exceeds this. LAN is
// <1 ms, same-region fiber <~20 ms; 30 ms keeps it "local/regional" without being so
// strict that a same-city LAN party fails. (Test hook: the headless harness may lower
// it via globalThis.__DUKE_LO_MAX_MS__; that global is never set in the browser build.)
const LOCAL_ONLY_MAX_MS = ((globalThis as Record<string, unknown>).__DUKE_LO_MAX_MS__ as number | undefined) ?? 30;

export interface HostConfig {
  name: string;
  isPublic: boolean;
  maxPlayers: number;
  localOnly?: boolean; // boot guests whose real (data-channel) RTT exceeds the threshold
}

export interface DukeNetEvents {
  onLobby?: (rows: LobbyRow[]) => void;
  onRoster?: (players: RoomPlayer[]) => void;
  onStatus?: (msg: string) => void;
  onError?: (msg: string) => void;
  onGrpProgress?: (fraction: number, label: string) => void;
  onJoined?: (info: { matchId: string; myConnectIndex: number }) => void;
  onLeft?: () => void;
}

class DukeNet {
  private match: Match | null = null;
  private myName = "Duke";
  private allowDownload = true; // "sharing is ON by default"
  private localGrp: GrpFingerprint | null = null;
  private inPlayGrpBytes: Uint8Array | null = null; // cached ONLY when shareable
  private myConnectIndex = 0; // host = 0; guest learns its slot at join_ok
  private readonly seam = new Seam();
  private readonly ping = new PingTracker();
  private myRelayRtt: number | null = null;
  private pingFilterMaxMs = Infinity;
  private trueRtt = new Map<string, number>(); // hostId -> measured RTT (lobby)
  private events: DukeNetEvents = {};

  // Host slot allocation (host = 0; guests take 1..maxPlayers-1).
  private slots = new Map<string, number>(); // deviceId -> slot
  private rttTimer: ReturnType<typeof setInterval> | null = null;

  // Late-join snapshot receive state (guest/veteran side).
  private savBuf: Uint8Array[] | null = null;
  private savSize = 0;
  private savMask = 0;
  private savPlc = 0; // sender's movefifoplc at save (catchup stream base)
  private savJoin = 0; // 1 = barrier-free join apply, 0 = legacy resync apply
  private savSpawns = ""; // host's premap spawn table (see Net_SetSpawnTable)

  // GRP receive state (guest side).
  private grpRecv: GrpReceiver | null = null;
  private grpRecvIndex = 0;
  private grpRecvFrom: string | null = null;
  private grpTailRetries = 0; // bounded grp_end-but-incomplete re-requests
  private pendingJoinInfo: MatchInfo | null = null; // the match we want after a download
  // GRP send state (host side): one live stream per peer. A newer grp_req bumps the
  // generation so the superseded loop stops — two loops interleaving chunks on the
  // same ordered channel would scramble the receiver's arrival-order indexing.
  private grpSendGen = new Map<string, number>();

  // The seam surface the js-library (seam_library.js) calls. Bound in ctor.
  readonly _seamSend = (peerToken: number, channel: number, reliable: number, bytes: Uint8Array) =>
    this.seam.send(peerToken, channel, reliable, bytes);
  readonly _seamBroadcast = (channel: number, reliable: number, bytes: Uint8Array) =>
    this.seam.broadcast(channel, reliable, bytes);
  readonly _seamDrain = () => this.seam.drain();
  readonly _seamInit = () => this.seam.init();
  readonly _seamShutdown = () => this.seam.shutdown();
  readonly _seamKick = (peerToken: number) => this.kickPeer(peerToken);

  /** HOST: tear down one seated peer's pair. Called by the engine (net_kick)
   *  after it has deterministically excised that player (timeout/disconnect
   *  drop). A courtesy {t:'kick'} rides ahead so a live-but-lagging client shows
   *  a reason instead of a bare connection loss. */
  kickPeer(peerToken: number): void {
    const m = this.match;
    if (!m || m.role !== "host") return;
    const dev = this.seam.deviceOf(peerToken);
    if (!dev) return;
    console.log(`[dnet] kick: dropping peer token=${peerToken} (${dev.slice(0, 8)})`);
    m.peers.sendControl(dev, { t: "kick", reason: "dropped from the match (connection)" } as Ctl);
    this.seam.unregisterPeer(dev);
    this.slots.delete(dev);
    setTimeout(() => this.match?.peers.close(dev), 300); // let the notice flush, then drop
  }

  constructor() {
    // The seam's actual wire send: resolve deviceId -> the peer's data channel.
    this.seam.setSender((deviceId, channel, bytes) => (this.match ? this.match.peers.sendNet(deviceId, channel, bytes) : false));
    const nm = safeLocalGet("eduke32-net-name");
    if (nm) this.myName = sanitizeName(nm);
    const ad = safeLocalGet("eduke32-net-allow-dl");
    if (ad === "0") this.allowDownload = false;

    // Backgrounded GUEST tab: the wasm main loop throttles to a crawl, which
    // would drag the whole lockstep down (or zombie at ~1 tic/s under the
    // host's rate monitor). After a short grace, leave the match cleanly: close
    // the host pair (host drops the seat within a tic) and queue the host-down
    // event locally so the engine exits to the menu the moment the tab resumes.
    // The HOST is exempt: hiding the host ends the match for everyone anyway,
    // and each guest's own host-silence watchdog already covers that.
    if (typeof document !== "undefined") {
      let hiddenTimer: ReturnType<typeof setTimeout> | null = null;
      document.addEventListener("visibilitychange", () => {
        if (!document.hidden) {
          if (hiddenTimer) { clearTimeout(hiddenTimer); hiddenTimer = null; }
          return;
        }
        if (hiddenTimer || !this.match || this.match.role !== "guest") return;
        const inGame = () => ((window as any).__e32menu?.game | 0) === 1;
        if (!inGame()) return;
        hiddenTimer = setTimeout(() => {
          hiddenTimer = null;
          const m = this.match;
          if (!document.hidden || !m || m.role !== "guest" || !inGame()) return;
          console.log("[dnet] tab hidden >5s in-game: leaving the match");
          const hostId = m.hostId;
          if (hostId) {
            this.seam.enqueuePeerEventByDevice(hostId, false); // engine sees host-down on resume
            m.peers.close(hostId);
          }
        }, 5000);
      });
    }
  }

  // ── Config ─────────────────────────────────────────────────────────────────

  setPlayerName(name: string): void {
    this.myName = sanitizeName(name);
    safeLocalSet("eduke32-net-name", this.myName);
  }
  getPlayerName(): string {
    return this.myName;
  }

  /** Host opt-out toggle: "Don't allow people to download my GRP files." Sharing is
   *  ON by default; the paid-retail firewall still blocks paid GRPs regardless. */
  setAllowGrpDownload(allow: boolean): void {
    this.allowDownload = !!allow;
    safeLocalSet("eduke32-net-allow-dl", allow ? "1" : "0");
  }
  getAllowGrpDownload(): boolean {
    return this.allowDownload;
  }

  on(events: DukeNetEvents): void {
    this.events = { ...this.events, ...events };
  }

  pingPresets() {
    return PING_PRESETS;
  }
  setPingFilter(maxMs: number): void {
    this.pingFilterMaxMs = maxMs;
    this._refreshLobby();
  }

  /** Measured data-channel RTT (ms) to a peer by device id, or null if unmeasured
   *  (or our own row). The lobby roster shows this real ping, never the relay proxy. */
  rttFor(deviceId: string): number | null {
    return this.trueRtt.get(deviceId) ?? null;
  }

  // ── Local GRP fingerprint ────────────────────────────────────────────────

  /** Set the fingerprint of the GRP set THIS client is running. Pass the ordered
   *  component list (index 0 = main GRP) and, if the main GRP is shareable, its
   *  bytes so the host can serve them. Non-shareable bytes are never cached. */
  async setLocalGrp(components: GrpComponent[], mainGrpBytes?: Uint8Array): Promise<GrpFingerprint> {
    const fp = await setFingerprint(components);
    this.localGrp = fp;
    this.inPlayGrpBytes = fp.shareable && mainGrpBytes ? mainGrpBytes : null;
    if (this.match) this.match.setGrp(fp);
    this._refreshLobby();
    return fp;
  }

  /** Compute + set the local fingerprint from Module.FS, given the ordered file
   *  paths of the loaded set (main GRP first). Caches the main GRP bytes iff
   *  shareable. */
  async setLocalGrpFromFs(FS: EmscriptenFS, paths: string[]): Promise<GrpFingerprint> {
    if (paths.length === 0) throw new Error("setLocalGrpFromFs: no paths");
    const comps: GrpComponent[] = [];
    let mainBytes: Uint8Array | undefined;
    for (let i = 0; i < paths.length; i++) {
      const p = paths[i];
      const { crc, sha256, size, bytes } = await fingerprintFsFile(FS, p);
      comps.push({ name: baseName(p), crc, sha256, size });
      if (i === 0) mainBytes = bytes;
    }
    return this.setLocalGrp(comps, mainBytes);
  }

  getLocalGrp(): GrpFingerprint | null {
    return this.localGrp;
  }
  getMyConnectIndex(): number {
    return this.myConnectIndex;
  }

  /** One compact state line for the periodic console heartbeat (null = no match).
   *  Reading it never touches the engine, so it stays truthful while wasm is wedged. */
  debugLine(): string | null {
    const m = this.match;
    if (!m) return null;
    const up = m.players().filter((p) => p.connected).length;
    return `role=${m.role} status=${m.status()} idx=${this.myConnectIndex} roster=${m.players().length} connected=${up} attached=${this.slots.size}`;
  }

  /** HOST: stream the snapshot (written by the engine to /latejoin.esv) as
   *  base64 control slices on duke-rel. targetSlot < 0 broadcasts to every
   *  ATTACHED peer (legacy resync: everyone reloads); otherwise ONLY that
   *  slot's peer receives it (barrier-free join / targeted resync -- the match
   *  never pauses and veterans reload nothing). plc = engine movefifoplc at
   *  save (the catchup stream base); isJoin selects the engine's apply mode. */
  sendSnapshot(seatMask: number, targetSlot = -1, plc = 0, isJoin = 0): void {
    const m = this.match;
    if (!m || m.role !== "host") return;
    let bytes: Uint8Array;
    try {
      const FS = (globalThis as unknown as { Module?: { FS?: { readFile: (p: string) => Uint8Array } } }).Module?.FS;
      if (!FS) return;
      bytes = FS.readFile("/latejoin.esv");
    } catch (e) {
      console.log("[dnet] sendSnapshot: read failed: " + String(e));
      return;
    }
    const CHUNK = 48 * 1024;
    let targets = [...this.slots.keys()].filter((id) => m.peers.isAttached(id));
    if (targetSlot >= 0)
      targets = targets.filter((id) => this.slots.get(id) === targetSlot);
    console.log(
      `[dnet] -> snapshot ${bytes.length} bytes to ${targets.length} peer(s), mask 0x${seatMask.toString(16)}, slot ${targetSlot}, plc ${plc}, join ${isJoin}`,
    );
    // Premap's spawn table rides along: the snapshot is a savegame and does not
    // carry it, and a fresh-process joiner has none (its Net_InsertLatePlayer
    // would silently skip -- a permanent fork; see Net_SetSpawnTable).
    let spawns = "";
    try {
      const mod = (globalThis as unknown as { Module?: { ccall?: (...a: unknown[]) => unknown } }).Module;
      spawns = (mod?.ccall?.("Net_GetSpawnTable", "string", [], []) as string) || "";
    } catch { spawns = ""; }
    for (const peerId of targets) {
      m.peers.sendControl(peerId, { t: "sav_begin", size: bytes.length, mask: seatMask, plc, join: isJoin, spawns } as Ctl);
      for (let off = 0; off < bytes.length; off += CHUNK) {
        const slice = bytes.subarray(off, Math.min(off + CHUNK, bytes.length));
        let bin = "";
        for (let i = 0; i < slice.length; i += 0x8000)
          bin += String.fromCharCode.apply(null, Array.from(slice.subarray(i, i + 0x8000)));
        m.peers.sendControl(peerId, { t: "sav_chunk", d: btoa(bin) } as Ctl);
      }
      m.peers.sendControl(peerId, { t: "sav_end" } as Ctl);
    }
  }

  /** The engine calls this when the local match actually starts (true) or ends
   *  (false). On a public HOST it flips the advertised status to "playing", which
   *  both removes the match from the public list and closes the host's accept gate
   *  (Match._acceptsPeer keys on status === "open"), so late joiners cannot land in a
   *  lockstep game already in progress. No-op for guests and outside a match. */
  setInGame(inGame: boolean): void {
    this.match?.setStatus(inGame ? "playing" : "open");
  }

  // ── Hosting ────────────────────────────────────────────────────────────────

  async host(cfg: HostConfig): Promise<{ matchId: string; inviteCode: string; inviteUrl: string }> {
    if (!this.localGrp) throw new Error("host: set the local GRP fingerprint first");
    if (this.match) this.leave();
    this.myRelayRtt = await measureRelayRtt(activeRelays()).catch(() => null);
    const name = sanitizeText(cfg.name) || "Duke Match";
    const max = clampInt(cfg.maxPlayers, 2, 16);
    this.match = cfg.isPublic
      ? await Match.createPublic(name, max, this.myName, this.localGrp, this.myRelayRtt, cfg.localOnly ?? false)
      : await Match.createPrivate(name, max, this.myName, this.localGrp, this.myRelayRtt, cfg.localOnly ?? false);
    this.myConnectIndex = 0;
    this.slots.clear();
    // Mark our OWN slot 0 connected in the netcode. Without this the host's
    // connecthead != myconnectindex and Net_SendNewGame silently DROPS the launch
    // broadcast: the host enters the level alone while every guest keeps waiting in
    // the lobby. (The native transport fixed the identical bug in hostMatch; guests
    // get theirs via the join_ok handler.)
    if (typeof window !== "undefined") {
      const mod = (window as unknown as { Module?: { ccall?: (...args: unknown[]) => unknown } }).Module;
      mod?.ccall?.("Net_SetLocalIndex", "void", ["number"], [0]);
      console.log("[dnet] host: slot 0 attached (connect chain rooted)");
    }
    this._wireMatch();
    this._startRttLoop();
    const info = this.match.info();
    const inviteCode = this.match.inviteCode();
    this.events.onStatus?.(cfg.isPublic ? "Hosting (public)" : "Hosting (private)");
    return { matchId: info.matchId, inviteCode, inviteUrl: inviteUrlFor(inviteCode) };
  }

  /** QR data URL for an invite code (loads `qrcode` lazily to keep the hot path light). */
  async inviteQr(inviteCode: string): Promise<string | null> {
    try {
      const { default: QR } = await import("qrcode");
      return await QR.toDataURL(inviteUrlFor(inviteCode), { margin: 1, width: 240 });
    } catch {
      return null;
    }
  }

  // ── Joining ──────────────────────────────────────────────────────────────

  static parseInvite(codeOrUrl: string): MatchInfo | null {
    return Match.parseInvite(extractInviteCode(codeOrUrl));
  }

  async join(target: MatchInfo | string): Promise<void> {
    if (!this.localGrp) throw new Error("join: set the local GRP fingerprint first");
    let info: MatchInfo | null;
    if (typeof target === "string") {
      const code = extractInviteCode(target);
      info = Match.parseInvite(code);            // legacy full-record blob
      if (!info && Match.looksLikeShortCode(code)) {
        this.events.onStatus?.("Looking up match…");
        info = await Match.resolveShortCode(code); // 12-char relay ticket
      }
    } else {
      info = target;
    }
    if (!info) {
      this.events.onError?.("Match not found — the code may have expired with its host");
      return;
    }
    if (this.match) this.leave();
    this.pendingJoinInfo = info;
    this.match = await Match.join(info, this.myName, this.localGrp);
    this.myConnectIndex = -1; // unknown until join_ok
    this._wireMatch();
    this._startRttLoop();
    this.events.onStatus?.("Connecting to host…");
  }

  // ── Public discovery / lobby ─────────────────────────────────────────────

  startLobby(onLobby?: (rows: LobbyRow[]) => void): void {
    if (onLobby) this.events.onLobby = onLobby;
    void (async () => {
      this.myRelayRtt = await measureRelayRtt(activeRelays()).catch(() => null);
      this._refreshLobby();
    })();
    this._lobbyMatches = adoptPublicLobby((ms) => {
      this._lobbyMatches = ms;
      this._refreshLobby();
    });
  }
  warmLobby(): void {
    warmPublicLobby();
  }
  stopLobby(): void {
    releasePublicLobby();
    this._lobbyMatches = [];
  }
  private _lobbyMatches: MatchInfo[] = [];

  /** One-shot public match query (for a manual "refresh"). */
  async listMatches(): Promise<LobbyRow[]> {
    return new Promise((resolve) => {
      let settled = false;
      void listPublicMatches((ms) => {
        if (settled) return;
        settled = true;
        resolve(this._rows(ms));
      }).then((unsub) => setTimeout(() => unsub(), 3000));
    });
  }

  private _rows(matches: MatchInfo[]): LobbyRow[] {
    return buildLobbyRows(matches, {
      localGrp: this.localGrp,
      myRelayRttMs: this.myRelayRtt,
      trueRtt: this.trueRtt,
      pingFilterMaxMs: this.pingFilterMaxMs,
    });
  }
  private _refreshLobby(): void {
    this.events.onLobby?.(this._rows(this._lobbyMatches));
  }

  // ── Leaving ────────────────────────────────────────────────────────────────

  leave(): void {
    if (this.rttTimer) {
      clearInterval(this.rttTimer);
      this.rttTimer = null;
    }
    this.match?.leave();
    this.match = null;
    this.slots.clear();
    this.grpRecv = null;
    this.grpRecvFrom = null;
    this.seam.shutdown();
    this.events.onLeft?.();
  }

  // ── Match wiring ───────────────────────────────────────────────────────────

  private _wireMatch(): void {
    const m = this.match;
    if (!m) return;
    m.handlers = {
      onRoster: (players) => this.events.onRoster?.(players),
      onChannelsReady: (peerId) => this._onChannelsReady(peerId),
      onControl: (peerId, msg) => this._onControl(peerId, msg as Ctl),
      onBulkChunk: (peerId, bytes) => this._onBulkChunk(peerId, bytes),
      onNetFrame: (peerId, channel, bytes) => this.seam.enqueueFrameByDevice(peerId, channel, bytes),
      onConnection: (peerId, state) => this._onConnection(peerId, state),
    };
  }

  private _onChannelsReady(peerId: string): void {
    const m = this.match;
    if (!m) return;
    console.log(`[dnet] channels ready with ${peerId.slice(0, 8)} (role=${m.role})`);
    if (m.role === "guest" && peerId === m.hostId) {
      // Guest -> host: request to join (GRP-gated on the host side).
      console.log(`[dnet] -> sending join to host`);
      this.events.onStatus?.("Connecting to host… (joining)");
      m.peers.sendControl(peerId, { t: "join", name: this.myName, grp: this.localGrp } as Ctl);
    }
    // Host waits for each guest's {t:'join'}.
  }

  private _onControl(peerId: string, msg: Ctl): void {
    const m = this.match;
    if (!m || !msg || typeof msg.t !== "string") return;
    switch (msg.t) {
      case "join":
        this._hostHandleJoin(peerId, msg);
        break;
      case "join_ok":
        this._guestHandleJoinOk(peerId, msg);
        break;
      case "join_deny":
        this._guestHandleJoinDeny(peerId, msg);
        break;
      case "kick":
        this.events.onError?.(`Kicked: ${msg.reason}`);
        this.leave();
        break;
      case "grp_req":
        this._hostHandleGrpReq(peerId, msg);
        break;
      case "grp_begin":
        this._guestGrpBegin(peerId, msg.offer, msg.from ?? 0);
        break;
      case "grp_end":
        void this._guestGrpEnd(peerId);
        break;
      case "grp_deny":
        this.events.onError?.("GRP download refused: " + msg.reason);
        break;
      case "sav_begin":
        // Late-join snapshot incoming (host only). Buffered as base64 control
        // slices on duke-rel: strings are ALWAYS control, even post-attach, so
        // this coexists with live netcode frames without touching the seam.
        if (m.role === "guest" && peerId === m.hostId) {
          this.savBuf = [];
          this.savSize = msg.size | 0;
          this.savMask = msg.mask | 0;
          this.savPlc = (msg.plc ?? 0) | 0;
          this.savJoin = (msg.join ?? 0) | 0;
          this.savSpawns = typeof msg.spawns === "string" ? msg.spawns : "";
          console.log(
            `[dnet] <- snapshot begin (${msg.size} bytes, mask 0x${(msg.mask | 0).toString(16)}, plc ${this.savPlc}, join ${this.savJoin})`,
          );
          this.events.onStatus?.("Syncing game state…");
        }
        break;
      case "sav_chunk":
        if (this.savBuf && m.role === "guest" && peerId === m.hostId) {
          const bin = atob(msg.d);
          const arr = new Uint8Array(bin.length);
          for (let i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i);
          this.savBuf.push(arr);
        }
        break;
      case "sav_end": {
        if (!this.savBuf || m.role !== "guest" || peerId !== m.hostId) break;
        const total = this.savBuf.reduce((n, a) => n + a.length, 0);
        const bytes = new Uint8Array(total);
        let off = 0;
        for (const a of this.savBuf) { bytes.set(a, off); off += a.length; }
        this.savBuf = null;
        if (total !== this.savSize) {
          console.log(`[dnet] snapshot SIZE MISMATCH (${total} != ${this.savSize}) -- dropped`);
          break;
        }
        try {
          const FS = (globalThis as unknown as { Module?: { FS?: { writeFile: (p: string, d: Uint8Array) => void } } }).Module?.FS;
          FS?.writeFile("/latejoin.esv", bytes);
          const mod = (globalThis as unknown as { Module?: { ccall?: (...a: unknown[]) => unknown } }).Module;
          if (this.savSpawns)
            mod?.ccall?.("Net_SetSpawnTable", null, ["string"], [this.savSpawns]);
          mod?.ccall?.("Net_SnapshotReady", null, ["number", "number", "number"], [this.savMask, this.savPlc, this.savJoin]);
          console.log(`[dnet] snapshot ready (${total} bytes, plc ${this.savPlc}, join ${this.savJoin}) -> engine notified`);
        } catch (e) {
          console.log("[dnet] snapshot write failed: " + String(e));
        }
        break;
      }
      case "rtt_ping":
        m.peers.sendControl(peerId, { t: "rtt_pong", id: msg.id } as Ctl);
        break;
      case "rtt_pong": {
        const rtt = this.ping.onPong(peerId, msg.id);
        // Record RTT for ANY peer (host measures each guest, guest measures the
        // host) so the lobby roster shows a real per-player ping. Keyed by device
        // id, which is what the roster rows join on.
        if (rtt != null) this.trueRtt.set(peerId, rtt);
        // Local-only enforcement (post-connect: a real RTT must exist first). The host
        // boots an admitted guest whose measured RTT exceeds the local threshold.
        if (rtt != null && m.role === "host" && m.localOnly && rtt > LOCAL_ONLY_MAX_MS && this.slots.has(peerId)) {
          this.events.onStatus?.(`Dropped a player: ${Math.round(rtt)} ms > ${LOCAL_ONLY_MAX_MS} ms (local-only)`);
          m.peers.sendControl(peerId, { t: "kick", reason: `local-only match (your ping ${Math.round(rtt)} ms)` } as Ctl);
          setTimeout(() => m.peers.close(peerId), 300); // let the kick flush, then drop
        }
        break;
      }
    }
  }

  // ── Host: join handshake (authoritative slot assignment + GRP gate) ─────────

  private _hostHandleJoin(peerId: string, msg: { grp: GrpFingerprint; name: string }): void {
    const m = this.match;
    if (!m || m.role !== "host") return;
    const st = m.status();
    console.log(`[dnet] <- join request from ${peerId.slice(0, 8)} name=${msg.name} (match=${st} guests=${this.slots.size}/${m.maxPlayers - 1})`);
    // LATE JOIN is allowed: an in-game host accepts the joiner like any other; the
    // ENGINE defers the seat to a safe frame point (oldnet queues the peer-up, the
    // host relaunches the current map so everyone re-enters tic 0 together). The
    // old behavior -- attaching straight into the running tic loop -- hard-froze
    // the host right after "<- join request" (live-reported). "starting" is the
    // only refused window (mid-handoff).
    if (st === "starting") {
      console.log(`[dnet] join denied: match is mid-launch`);
      m.peers.sendControl(peerId, { t: "join_deny", reason: "started" } as Ctl);
      setTimeout(() => m.peers.close(peerId), 300); // deny flushed -> drop the pair
      return;
    }
    const hostFp = m.grpFingerprint();
    if (!msg.grp || msg.grp.setDigest !== hostFp.setDigest) {
      // GRP GATING: only identical-GRP-set players may join. Offer the fingerprint so
      // the joiner can decide whether to download (if the host's main GRP is shareable).
      console.log(`[dnet] join denied: grp mismatch (theirs=${msg.grp?.setDigest?.slice(0, 12) ?? "none"} ours=${hostFp.setDigest.slice(0, 12)})`);
      m.peers.sendControl(peerId, { t: "join_deny", reason: "grpmismatch", hostGrp: hostFp } as Ctl);
      return;
    }
    if (this.slots.size + 1 >= m.maxPlayers) {
      console.log(`[dnet] join denied: full (${this.slots.size + 1}/${m.maxPlayers})`);
      m.peers.sendControl(peerId, { t: "join_deny", reason: "full" } as Ctl);
      return;
    }
    const slot = this._allocSlot();
    this.slots.set(peerId, slot);
    // Attach: from now on this peer's channels carry raw netcode frames, and the
    // netcode sees a NET_PEER_UP at connectindex==slot. Log each step so a freeze
    // in this path pinpoints itself (the last line printed = the step that hung).
    console.log(`[dnet] join accept: slot=${slot} registering peer`);
    this.seam.registerPeer(peerId, slot);
    m.peers.setAttached(peerId, true);
    this.seam.enqueuePeerEventByDevice(peerId, true);
    console.log(`[dnet] join accept: peer-up queued, sending join_ok slot=${slot}`);
    m.peers.sendControl(peerId, { t: "join_ok", yourSlot: slot, hostSlot: 0, name: this.myName } as Ctl);
    console.log(`[dnet] -> join_ok sent to ${peerId.slice(0, 8)} (slot ${slot})`);
    this.events.onStatus?.(`Player joined (slot ${slot})`);
  }

  private _guestHandleJoinOk(peerId: string, msg: { yourSlot: number; hostSlot: number }): void {
    const m = this.match;
    if (!m || m.role !== "guest" || peerId !== m.hostId) return;
    console.log(`[dnet] <- join_ok slot=${msg.yourSlot} -> entering lobby`);
    this.myConnectIndex = msg.yourSlot | 0;
    // Tell the netcode our authoritative slot (host is 0; we are a guest at
    // yourSlot). Reaches the page's Emscripten Module — ccall is exported via
    // EXPORTED_RUNTIME_METHODS, Net_SetLocalIndex via EXPORTED_FUNCTIONS. Guarded
    // so it is a strict no-op when the wasm runtime is absent (Node unit tests) or
    // not yet ready, mirroring seam_library.js / index.html's window guards.
    if (typeof window !== "undefined") {
      const mod = (window as unknown as { Module?: { ccall?: (...args: unknown[]) => unknown } }).Module;
      mod?.ccall?.("Net_SetLocalIndex", "void", ["number"], [this.myConnectIndex]);
    }
    // The host is our single peer (star). Register it and attach.
    this.seam.registerPeer(peerId, msg.hostSlot | 0);
    m.peers.setAttached(peerId, true);
    this.seam.enqueuePeerEventByDevice(peerId, true);
    this.pendingJoinInfo = null;
    clearRejoin();
    // Seated: a future GRP switch (e.g. a different host later this session) must
    // be allowed to try again, so the one-shot switch guard resets on success.
    try {
      sessionStorage.removeItem(SWITCHTRY_KEY);
    } catch {
      /* ignore */
    }
    this.events.onJoined?.({ matchId: m.matchId, myConnectIndex: this.myConnectIndex });
    this.events.onStatus?.("Joined");
  }

  private _guestHandleJoinDeny(peerId: string, msg: { reason: string; hostGrp?: GrpFingerprint }): void {
    const m = this.match;
    if (!m || peerId !== m.hostId) return;
    console.log(`[dnet] <- join_deny reason=${msg.reason}`);
    if (msg.reason === "grpmismatch" && msg.hostGrp) {
      void this._resolveGrpMismatch(peerId, msg.hostGrp);
    } else if (msg.reason === "started") {
      // The match launched before our join completed. There is no mid-game join in
      // lockstep, so land back in Browse with an honest message instead of the old
      // forever-"CONNECTING TO HOST [JOINING]" / lobby limbo.
      this.events.onError?.("Match already in progress - pick another game");
      this.leave();
    } else {
      this.events.onError?.("Join refused: " + msg.reason);
    }
  }

  // ── Guest: turn a grpmismatch deny into the cheapest path to the host's GRP ──

  /** Order matters: (1) resume a surviving partial download; (2) find the host's
   *  exact GRP ALREADY ON THIS MACHINE — the registered import, a previously
   *  downloaded copy, or a GRP bundled in the page (every build ships shareware!)
   *  — and switch to it with a reload instead of re-downloading megabytes we
   *  have; (3) only then a real download (shareable GRPs only). Live-reported:
   *  a guest owning the host's shareware GRP was forced through an 11 MB
   *  download that its own connection then choked on. */
  private async _resolveGrpMismatch(peerId: string, hostFp: GrpFingerprint): Promise<void> {
    const m = this.match;
    if (!m || peerId !== m.hostId) return;
    const crc = hostFp.mainGrp.crc >>> 0;
    const cls = classifyByCrc(crc);

    const recv = this.grpRecv;
    if (recv && this.grpRecvFrom === peerId && recv.state === "receiving" && (recv.offer.crc >>> 0) === crc) {
      const from = recv.contiguousCount();
      console.log(`[dnet] resuming GRP download at chunk ${from}/${recv.offer.nchunks}`);
      this.events.onStatus?.("Resuming the GRP download…");
      m.peers.sendControl(peerId, { t: "grp_req", from } as Ctl);
      return;
    }

    // Local switch: single-GRP host sets only (a set with mods stacked cannot be
    // reproduced by swapping the main GRP), once per crc per session (a second
    // deny after a switch-reload means our local copy was NOT equivalent — fall
    // through to the download instead of reload-looping). ?noswitch=1 is the
    // test-harness override that forces the transfer path.
    const single = (hostFp.labels?.length ?? 1) <= 1;
    const tried = safeSessionGet(SWITCHTRY_KEY) === String(crc);
    if (single && !tried && !noSwitchRequested()) {
      const sw = await this._findLocalGrp(hostFp);
      if (sw) {
        safeSessionSet(SWITCHTRY_KEY, String(crc));
        setSessionGrpChoice(sw);
        console.log(`[dnet] host GRP found locally (${describeChoice(sw)}) — switching and rejoining`);
        this.events.onStatus?.("You already have this game — switching and rejoining…");
        reloadIntoMatch(this.pendingJoinInfo);
        return;
      }
    }

    if (cls.shareable) {
      // We lack the host's GRP but it is shareable — ask for it (the host still
      // enforces its own opt-out + the firewall before sending).
      this.events.onStatus?.("Downloading the host's GRP…");
      m.peers.sendControl(peerId, { t: "grp_req" } as Ctl);
      return;
    }
    this.events.onError?.(
      cls.known ? "This match needs a paid GRP you do not have." : "This match runs a GRP you do not have (not shareable).",
    );
  }

  /** Search this machine for a GRP byte-identical to the host's main GRP. Returns
   *  the session boot-choice that would launch it, or null. */
  private async _findLocalGrp(hostFp: GrpFingerprint): Promise<SessionGrpChoice | null> {
    const crc = hostFp.mainGrp.crc >>> 0;
    const want = hostFp.mainGrp;
    // The registered import (e.g. Atomic): its marker carries the crc, no IDB read.
    try {
      const marker = JSON.parse(safeLocalGet("eduke32/mainGrp") || "null") as { crc?: number } | null;
      if (marker && typeof marker.crc === "number" && (marker.crc >>> 0) === crc) return { main: 1 };
    } catch {
      /* no marker */
    }
    // A previously downloaded copy (verified when stored; sha re-checked anyway).
    const stored = await getGrp(crc);
    if (stored && stored.sha256 === want.sha256 && stored.size === want.size) return { crc, filename: stored.filename };
    // A GRP present in Module.FS (the bundled shareware, always shipped). Size
    // prefilter first — hashing is 11 MB of work we only spend on a plausible hit.
    const FS = (globalThis as unknown as { Module?: { FS?: EmscriptenFS } }).Module?.FS;
    if (FS) {
      for (const f of scanFsGrps(FS)) {
        if (f.size !== want.size) continue;
        if (/^_e32_main\.grp$/i.test(f.filename)) continue; // the import's slot — covered by the marker case
        for (const p of ["/" + f.filename, "/data/" + f.filename]) {
          try {
            const fp = await fingerprintFsFile(FS, p);
            if ((fp.crc >>> 0) === crc && fp.sha256 === want.sha256) return { crc, filename: f.filename, builtin: 1 };
          } catch {
            /* not at this path */
          }
        }
      }
    }
    return null;
  }

  // ── Host: serve the single in-play GRP (firewall + opt-out enforced here) ───

  private _hostHandleGrpReq(peerId: string, req?: { from?: number }): void {
    const m = this.match;
    if (!m || m.role !== "host") return;
    const fp = m.grpFingerprint();
    const bytes = this.inPlayGrpBytes;
    if (!bytes) {
      m.peers.sendControl(peerId, { t: "grp_deny", reason: "notplaying" } as Ctl);
      return;
    }
    const prep = prepareGrpSend(bytes, fp.mainGrp.crc, fp.mainGrp.sha256, fp.labels[0] ?? "GRP", this.allowDownload);
    if (!prep.ok) {
      m.peers.sendControl(peerId, { t: "grp_deny", reason: prep.reason } as Ctl);
      return;
    }
    const from = Math.max(0, Math.min(prep.offer.nchunks, (req?.from ?? 0) | 0));
    const gen = (this.grpSendGen.get(peerId) ?? 0) + 1;
    this.grpSendGen.set(peerId, gen);
    // grp_begin rides DUKE-BULK, in front of its own chunks: on a separate
    // channel it can lose (or win) a cross-stream race against the chunk
    // backlog. Same stream => begin < chunk 0 < ... < grp_end, always.
    m.peers.sendControl(peerId, { t: "grp_begin", offer: prep.offer, from } as Ctl, "duke-bulk");
    void this._streamGrp(peerId, prep.offer, prep.chunk, from, gen);
  }

  /** Stream chunks over duke-bulk with backpressure so a big transfer never wedges
   *  the channel. Ordered+reliable duke-bulk means the receiver can index by arrival
   *  order (no per-chunk header). `from` resumes a partial transfer (the guest kept
   *  its contiguous prefix across a reconnect). */
  private async _streamGrp(peerId: string, offer: GrpOffer, chunk: (i: number) => Uint8Array, from = 0, gen = 0): Promise<void> {
    const m = this.match;
    if (!m) return;
    // Keep the standing queue SHALLOW: everything buffered here adds seconds of
    // bufferbloat on a slow uplink, which delays ICE consent checks and flaps the
    // pc into "disconnected" mid-transfer. 512 KB keeps the pipe full at any
    // realistic rate (refilled every 20 ms) without drowning the STUN traffic.
    const HIGH = 512 * 1024;
    const superseded = () => gen !== 0 && this.grpSendGen.get(peerId) !== gen;
    for (let i = from; i < offer.nchunks; i++) {
      while (m.peers.bulkBufferedAmount(peerId) > HIGH) {
        await sleep(20);
        if (!this.match || superseded()) return; // left / newer request took over
      }
      if (superseded()) return;
      if (!m.peers.sendBulkChunk(peerId, chunk(i))) return; // channel gone
      if ((i & 63) === 0) this.events.onGrpProgress?.(i / offer.nchunks, "Uploading GRP");
    }
    if (superseded()) return;
    m.peers.sendControl(peerId, { t: "grp_end" } as Ctl, "duke-bulk"); // ordered AFTER the last chunk
    this.events.onGrpProgress?.(1, "Upload complete");
  }

  // ── Guest: receive + verify (HASH-BEFORE-USE) + persist + reload ────────────

  private _guestGrpBegin(peerId: string, offer: GrpOffer, from = 0): void {
    // Only accept a GRP whose CRC is shareable — a second firewall on the RECEIVER,
    // so a malicious host cannot push paid/unknown content onto us.
    if (!classifyByCrc(offer.crc).shareable) {
      this.events.onError?.("Refused a non-shareable GRP offer.");
      return;
    }
    // Resume: the host echoes the start chunk we asked for. Accept only if our
    // surviving partial is EXACTLY the same offer and our contiguous prefix ends
    // where the host will start — anything else gets a clean from-0 restart.
    const prev = this.grpRecv;
    if (from > 0) {
      const resumable =
        !!prev &&
        this.grpRecvFrom === peerId &&
        prev.state === "receiving" &&
        (prev.offer.crc >>> 0) === (offer.crc >>> 0) &&
        prev.offer.sha256 === offer.sha256 &&
        prev.offer.size === offer.size &&
        prev.offer.chunkSize === offer.chunkSize &&
        from === prev.contiguousCount();
      if (resumable) {
        this.grpRecvIndex = from;
        console.log(`[dnet] GRP download resumed at chunk ${from}/${offer.nchunks}`);
        return;
      }
      console.log(`[dnet] cannot resume GRP at chunk ${from} — requesting a fresh transfer`);
      this.grpRecv = null;
      this.grpRecvFrom = null;
      this.match?.peers.sendControl(peerId, { t: "grp_req" } as Ctl); // from absent = 0
      return;
    }
    this.grpRecv = new GrpReceiver(offer);
    this.grpRecvIndex = 0;
    this.grpRecvFrom = peerId;
    this.grpTailRetries = 0;
  }

  private _onBulkChunk(peerId: string, bytes: Uint8Array): void {
    if (!this.grpRecv || this.grpRecvFrom !== peerId) return;
    this.grpRecv.accept(this.grpRecvIndex++, bytes);
    this.events.onGrpProgress?.(this.grpRecv.progress, "Downloading GRP");
  }

  private async _guestGrpEnd(peerId: string): Promise<void> {
    const recv = this.grpRecv;
    if (!recv || this.grpRecvFrom !== peerId) return;
    if (!recv.complete) {
      // Ended short (a reconnect ate part of the stream, or the end marker of a
      // superseded send). The prefix is intact (ordered channel) — ask for the
      // tail instead of discarding megabytes. Bounded so a hostile/broken host
      // can't loop us forever.
      const from = recv.contiguousCount();
      if (++this.grpTailRetries <= 3) {
        console.log(`[dnet] grp_end at ${from}/${recv.offer.nchunks} chunks — requesting the tail (try ${this.grpTailRetries})`);
        this.events.onStatus?.("Resuming the GRP download…");
        this.match?.peers.sendControl(peerId, { t: "grp_req", from } as Ctl);
        return;
      }
      this.grpRecv = null;
      this.grpRecvFrom = null;
      this.events.onError?.("GRP download kept ending short — giving up.");
      return;
    }
    this.grpRecv = null;
    this.grpRecvFrom = null;
    const bytes = await recv.verify(); // HASH-BEFORE-USE: crc + sha256 must match
    if (!bytes) {
      this.events.onError?.("Downloaded GRP failed verification — discarded.");
      return;
    }
    // Persist the verified bytes, set the TRANSIENT boot choice, and reload
    // straight into ?join=<invite> so the page auto-rejoins this exact match
    // (with its banner + retry plumbing). The bytes are durable in IDB; the boot
    // choice is session-only so a later cold start returns to the player's own
    // game. (The old path wrote a sessionStorage rejoin key NOTHING consumed —
    // a completed download landed the player at the main menu, joined nothing.)
    try {
      const filename = sanitizeFilename(recv.offer.name);
      await putGrp({
        crc: recv.offer.crc >>> 0,
        sha256: recv.offer.sha256,
        size: recv.offer.size,
        filename,
        name: sanitizeText(recv.offer.name) || "GRP",
        bytes,
        savedAt: Date.now(),
      });
      setSessionGrpChoice({ crc: recv.offer.crc >>> 0, filename });
      this.events.onStatus?.("GRP downloaded — reloading to apply…");
      reloadIntoMatch(this.pendingJoinInfo);
    } catch (e) {
      this.events.onError?.("Could not persist the GRP: " + String(e));
    }
  }

  // ── Connection down -> netcode peer-down + slot free ────────────────────────

  private _onConnection(peerId: string, state: RTCPeerConnectionState): void {
    const m = this.match;
    if (!m) return;
    console.log(`[dnet] conn ${peerId.slice(0, 8)} -> ${state} (role=${m.role})`);
    // While a guest is still dialing its host (not yet attached == no join_ok yet),
    // surface the handshake stage on-screen so a stall is diagnosable from the status.
    if (m.role === "guest" && peerId === m.hostId && !m.peers.isAttached(peerId))
      this.events.onStatus?.(`Connecting to host… (${state})`);
    // "disconnected" never arrives here anymore: peer.ts holds transient flaps
    // back for a grace window and escalates a stuck one to "failed" itself.
    // Acting on the raw flap tore down seats (and mid-download transfers) that
    // ICE would have recovered on its own.
    if (state === "failed" || state === "closed") {
      if (m.peers.isAttached(peerId)) {
        this.seam.enqueuePeerEventByDevice(peerId, false);
        this.seam.unregisterPeer(peerId);
        m.peers.setAttached(peerId, false);
      }
      const slot = this.slots.get(peerId);
      if (slot !== undefined) this.slots.delete(peerId);
      this.ping.forget(peerId);
      this.trueRtt.delete(peerId);
    }
  }

  // ── RTT loop (lobby, pre-attach) ────────────────────────────────────────────

  private _startRttLoop(): void {
    if (this.rttTimer) clearInterval(this.rttTimer);
    this.rttTimer = setInterval(() => {
      const m = this.match;
      if (!m) return;
      // Ping EVERY connected peer, attached or not. Peers attach at JOIN time (in
      // the lobby), so the old `isAttached` skip meant the lobby never measured a
      // real RTT and the roster/rows fell back to the worthless relay proxy. The
      // netcode surfaces no RTT of its own, so this loop is the only source; a 2 s
      // control-channel ping does not perturb the lockstep game frames.
      for (const id of m.peers.connectedPeers()) {
        const pid = this.ping.startPing(id);
        m.peers.sendControl(id, { t: "rtt_ping", id: pid } as Ctl);
      }
    }, 2000);
  }

  // ── Host slot allocation ─────────────────────────────────────────────────

  private _allocSlot(): number {
    // The engine seats CPU players on slots the transport never sees. Handing a
    // joiner one of those strands them: the C join driver discards seat requests
    // for occupied slots, so the guest waits in the lobby forever (live-reported
    // via ?join= into a bot match). Ask the engine which seats bots hold and skip
    // them; the engine yields a bot seat separately once the human is in.
    let botMask = 0;
    if (typeof window !== "undefined") {
      const mod = (window as unknown as { Module?: { ccall?: (...a: unknown[]) => unknown } }).Module;
      try { botMask = ((mod?.ccall?.("Net_GetBotMask", "number", [], []) as number) ?? 0) | 0; } catch { botMask = 0; }
    }
    const used = new Set(this.slots.values());
    for (let s = 1; s < 64; s++) if (!used.has(s) && !(botMask & (1 << s))) return s;
    return this.slots.size + 1;
  }

  // ── Boot-time GRP application (called from index.html preRun) ───────────────

  /** Apply the player's persisted GRP CHOICE (a downloaded GRP) into Module.FS and
   *  return the -gamegrp file name to launch with. Called from index.html preRun,
   *  which gates the engine with add/removeRunDependency around it. Returns null when
   *  the default (bundled shareware) should be used. */
  async applyPendingGrpToFS(FS: { writeFile: (p: string, d: Uint8Array) => void }): Promise<string | null> {
    const choice = loadGrpChoice();
    if (!choice) return null;
    const stored = await getGrp(choice.crc);
    if (!stored) return null;
    try {
      FS.writeFile("/" + stored.filename, stored.bytes);
      return stored.filename;
    } catch {
      return null;
    }
  }

  /** The match to auto-rejoin after a download+reload (consumed once, per session).
   *  The in-engine menu layer calls this post-boot to resume the join. */
  consumePendingJoin(): MatchInfo | null {
    const info = loadRejoin();
    clearRejoin();
    return info;
  }
}

// ── module-local helpers ─────────────────────────────────────────────────────

function baseName(p: string): string {
  const i = Math.max(p.lastIndexOf("/"), p.lastIndexOf("\\"));
  return i >= 0 ? p.slice(i + 1) : p;
}
function sanitizeFilename(name: string): string {
  const b = baseName(String(name)).replace(/[^A-Za-z0-9._-]/g, "");
  return /\.grp$/i.test(b) ? b : (b || "DOWNLOAD") + ".GRP";
}
function clampInt(n: number, lo: number, hi: number): number {
  n = Math.floor(Number(n) || 0);
  return Math.max(lo, Math.min(hi, n));
}
function sleep(ms: number): Promise<void> {
  return new Promise((r) => setTimeout(r, ms));
}
function inviteUrlFor(code: string): string {
  const base = typeof location !== "undefined" ? location.origin + location.pathname : "";
  return base + "#join=" + code;
}
function extractInviteCode(codeOrUrl: string): string {
  const s = String(codeOrUrl).trim();
  const h = s.indexOf("#join=");
  if (h >= 0) return s.slice(h + 6);
  const q = s.indexOf("join=");
  if (q >= 0) return s.slice(q + 5);
  return s;
}
function safeLocalGet(k: string): string | null {
  try {
    return localStorage.getItem(k);
  } catch {
    return null;
  }
}
function safeLocalSet(k: string, v: string): void {
  try {
    localStorage.setItem(k, v);
  } catch {
    /* ignore */
  }
}
/** The transient (session-scoped) boot-GRP override a switch/download writes.
 *  Read by index.html's preRun blocks; see the SESSION_GRP_KEY comment above. */
type SessionGrpChoice = { crc: number; filename: string; builtin?: 1 } | { main: 1 };
function setSessionGrpChoice(c: SessionGrpChoice): void {
  safeSessionSet(SESSION_GRP_KEY, JSON.stringify(c));
}
function describeChoice(c: SessionGrpChoice): string {
  return "main" in c ? "registered import" : (c.builtin ? "bundled " : "downloaded ") + c.filename;
}
/** ?noswitch=1: test-harness override that forces the transfer path even when the
 *  host's GRP is available locally (never set by product UI). */
function noSwitchRequested(): boolean {
  try {
    return typeof location !== "undefined" && new URLSearchParams(location.search).has("noswitch");
  } catch {
    return false;
  }
}
function loadGrpChoice(): { crc: number; filename: string } | null {
  const raw = safeSessionGet(SESSION_GRP_KEY) ?? safeLocalGet(GAMEGRP_KEY);
  if (!raw) return null;
  try {
    return JSON.parse(raw) as { crc: number; filename: string };
  } catch {
    return null;
  }
}
function loadRejoin(): MatchInfo | null {
  try {
    const raw = sessionStorage.getItem(REJOIN_KEY);
    return raw ? (JSON.parse(raw) as MatchInfo) : null;
  } catch {
    return null;
  }
}
function clearRejoin(): void {
  try {
    sessionStorage.removeItem(REJOIN_KEY);
  } catch {
    /* ignore */
  }
}
/** Same encoding as Match.inviteBlob(), for a raw MatchInfo we hold (the match we
 *  were trying to join when the GRP switch/download forced a reload). */
function inviteBlobFor(info: MatchInfo): string {
  return btoa(JSON.stringify(info)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
/** Reload the page INTO a match: sets ?join=<blob> so the boot flow (banner,
 *  engine gating, retries — index.html's ?join= handler) drives the rejoin.
 *  Falls back to a plain reload when there is no match to return to. */
function reloadIntoMatch(info: MatchInfo | null): void {
  if (typeof location === "undefined") return;
  try {
    if (info) {
      const qs = new URLSearchParams(location.search);
      qs.delete("join");
      qs.delete("bot"); // never re-arm a bot flag across a GRP switch
      qs.set("join", inviteBlobFor(info));
      location.href = location.pathname + "?" + qs.toString();
      return;
    }
  } catch {
    /* fall through to a plain reload */
  }
  try {
    location.reload();
  } catch {
    /* ignore */
  }
}
function safeSessionGet(k: string): string | null {
  try {
    return sessionStorage.getItem(k);
  } catch {
    return null;
  }
}
function safeSessionSet(k: string, v: string): void {
  try {
    sessionStorage.setItem(k, v);
  } catch {
    /* ignore */
  }
}

// ── install the singleton ─────────────────────────────────────────────────────

const dukeNet = new DukeNet();

declare global {
  interface Window {
    DukeNet: DukeNet;
  }
}

// ── in-engine multiplayer menu bridge (eduke32-wasm) ──────────────────────────
// Forwards every DukeNet event into the C menu's NetMenu_* setters, and exposes
// window.NetMenu for the C action handlers to call back. EVERY ccall is guarded so
// the flag-OFF deploy build (no NetMenu_* exports, no ccall yet) is a silent no-op
// and its boot is unaffected.
function wireInEngineMenu(): void {
  const call = (fn: string, sig: string[], args: unknown[]): void => {
    try {
      const mod = (globalThis as unknown as { Module?: { ccall?: (...a: unknown[]) => unknown } }).Module;
      if (mod && typeof mod.ccall === "function") mod.ccall(fn, null, sig, args);
    } catch {
      /* export absent (flag-off) or runtime not ready yet -> no-op */
    }
  };
  const setStatus = (s: string): void => call("NetMenu_SetStatus", ["string"], [s]);
  // Mirror every status to the page (index.html's ?join= banner listens): the
  // in-menu status line is invisible from the main menu, which made every join
  // failure look like "the link does nothing".
  const emitPageStatus = (s: string): void => {
    try {
      if (typeof window !== "undefined") window.dispatchEvent(new CustomEvent("duke-net-status", { detail: s }));
    } catch { /* page without CustomEvent: nothing to show */ }
  };
  const fail = (e: unknown, what: string): void => setStatus("!" + ((e as Error)?.message || what));

  // Fingerprint the loaded GRP the first time we host/join/browse. Nothing else
  // initializes it, and dukeNet.host()/join() THROW without it; the engine has the
  // GRP preloaded in Module.FS, so find the main one and hand its bytes to the
  // transport. Idempotent (no-op once set), so it is cheap to await on every path.
  const ensureLocalGrp = async (): Promise<void> => {
    if (dukeNet.getLocalGrp()) return;
    const M = (globalThis as unknown as { Module?: { FS?: EmscriptenFS; arguments?: string[] } }).Module;
    const FS = M?.FS;
    if (!FS) return;
    // THE ENGINE'S main GRP, not "first *.grp in readdir": the registered-GRP
    // restore boots the import as the reserved /_e32_main.grp (bundled shareware
    // /DUKE3D.GRP stays in MEMFS beside it) and selects it with -gamegrp. Scanning
    // the directory fingerprinted the SHAREWARE while the engine played Atomic --
    // every registered-GRP match then rendered "paid"/unjoinable. Honor the boot
    // arguments first; fall back to the scan only when no explicit choice exists.
    let mainPath: string | null = null;
    let label: string | null = null;
    try {
      const args: string[] = M?.arguments ?? [];
      const gi = args.lastIndexOf("-gamegrp");
      const chosen = gi >= 0 ? args[gi + 1] : null;
      if (chosen) {
        for (const cand of [chosen.startsWith("/") ? chosen : "/" + chosen, chosen]) {
          try { FS.stat(cand); mainPath = cand; break; } catch { /* next form */ }
        }
        if (mainPath && baseName(mainPath) === "_e32_main.grp") {
          // Reserved-name import: advertise the ORIGINAL filename, not the slot.
          try {
            const marker = JSON.parse(localStorage.getItem("eduke32/mainGrp") || "null") as { name?: string } | null;
            if (marker?.name) label = marker.name;
          } catch { /* keep the raw name */ }
        }
      }
    } catch { /* fall through to the scan */ }
    if (!mainPath) {
      const grps = scanFsGrps(FS);
      if (!grps.length) return;
      for (const cand of ["/" + grps[0].filename, "/data/" + grps[0].filename]) {
        try { FS.stat(cand); mainPath = cand; break; } catch { /* try the next dir */ }
      }
    }
    if (!mainPath) return;
    const fp = await dukeNet.setLocalGrpFromFs(FS, [mainPath]);
    if (label && fp) fp.labels[0] = label;
  };

  const rowForMenu = (r: LobbyRow) => ({
    matchId: r.matchId,
    name: r.name,
    players: r.players,
    maxPlayers: r.maxPlayers,
    ping: r.rttMs == null ? -1 : Math.round(r.rttMs), // browse shows real RTT only ("?" until connected), not the relay proxy
    grpState: r.haveGrp ? "have" : r.needsPaidGrp ? "paid" : "download",
  });

  let lastRows: LobbyRow[] = [];
  dukeNet.on({
    onStatus: (s) => { setStatus(s); emitPageStatus(s); },
    onError: (s) => { setStatus("!" + s); emitPageStatus("!" + s); },
    onLobby: (rows) => {
      lastRows = rows;
      call("NetMenu_SetLobby", ["string"], [JSON.stringify(rows.map(rowForMenu))]);
    },
    onRoster: (ps) =>
      call("NetMenu_SetRoster", ["string"], [JSON.stringify(ps.map((p) => ({
        name: p.name,
        connected: p.connected,
        ping: dukeNet.rttFor(p.deviceId) ?? -1, // real data-channel RTT; -1 = unmeasured / self
      })))]),
    onGrpProgress: (f, l) => call("NetMenu_SetProgress", ["number", "string"], [Math.round(f * 100), l]),
    onJoined: (i) => call("NetMenu_OnJoined", ["number"], [i.myConnectIndex]),
  });

  const NetMenu = {
    host(isPublic: number, name: string, maxPlayers: number, player: string, localOnly: number): void {
      try { if (player) dukeNet.setPlayerName(player); } catch { /* keep last name */ }
      ensureLocalGrp()
        .then(() => dukeNet.host({ name, isPublic: !!isPublic, maxPlayers, localOnly: !!localOnly }))
        .then((r) => { if (!isPublic && r?.inviteCode) setStatus("Invite code: " + r.inviteCode); })
        .catch((e) => fail(e, "Host failed"));
    },
    joinRow(idx: number, player: string): void {
      try { if (player) dukeNet.setPlayerName(player); } catch { /* keep last name */ }
      const row = lastRows[idx];
      if (!row) { setStatus("!That match is no longer listed"); return; }
      ensureLocalGrp().then(() => dukeNet.join(row.raw)).catch((e) => fail(e, "Join failed"));
    },
    joinCode(code: string, player: string): void {
      try { if (player) dukeNet.setPlayerName(player); } catch { /* keep last name */ }
      ensureLocalGrp().then(() => dukeNet.join(code)).catch((e) => fail(e, "Join failed"));
    },
    setPing(idx: number): void {
      const presets = dukeNet.pingPresets();
      const p = presets[idx] ?? presets[0];
      if (p) dukeNet.setPingFilter(p.maxMs);
    },
    setAllowDl(dontAllow: number): void { dukeNet.setAllowGrpDownload(!dontAllow); },
    startBrowse(): void { void ensureLocalGrp().then(() => dukeNet.startLobby()).catch((e) => fail(e, "Lobby unavailable")); },
    stopBrowse(): void { try { dukeNet.stopLobby(); } catch { /* nothing to stop */ } },
    leave(): void { try { dukeNet.leave(); } catch { /* already gone */ } },
  };
  (window as unknown as { NetMenu: typeof NetMenu }).NetMenu = NetMenu;

  // Freeze-visible heartbeat: one line every 15s WHILE IN A MATCH. Reading it:
  //   lines keep coming + canvas frozen  -> the engine wedged (JS event loop alive);
  //   lines stop while the tab is open   -> a wasm spin is blocking the event loop.
  // Either way the LAST [dnet] line before silence names the step that hung.
  setInterval(() => {
    const l = dukeNet.debugLine();
    if (l) console.log("[dnet] hb " + l);
  }, 15_000);
}

if (typeof window !== "undefined") {
  window.DukeNet = dukeNet;
  // Surface the local device id for diagnostics / the in-engine menu.
  (window as unknown as { DUKE_DEVICE_ID: string }).DUKE_DEVICE_ID = DEVICE_ID;
  wireInEngineMenu();
}

export { DukeNet };
export default dukeNet;
