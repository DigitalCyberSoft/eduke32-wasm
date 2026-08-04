// ─────────────────────────────────────────────────────────────────────────────
// LOBBY — assemble browsable rows from raw public MatchInfo + our own state.
//
// Pure, dependency-light logic (easy to unit test) that bakes in the user rules:
//   * matches whose GRP set we ALREADY HAVE sort to the TOP;
//   * a match we lack AND whose GRP is PAID (not shareable) is MARKED needsPaidGrp
//     (we cannot join it holding only shareware, and cannot legally download it);
//   * a match we lack whose GRP is SHAREABLE is MARKED canDownload;
//   * per-row ping is the true data-channel RTT if we have one, else the relay-RTT
//     proxy estimate, else null ("?");
//   * the high-ping filter EXCLUDES rows over the threshold but never hides an
//     unknown ("?") ping.
// ─────────────────────────────────────────────────────────────────────────────

import type { MatchInfo } from "./match";
import type { GrpFingerprint } from "./grp";
import { fingerprintsMatch, classifyFingerprint } from "./grp";
import { estimatePing, passesPingFilter } from "./ping";
import { sanitizeText } from "./sanitize";

export interface LobbyRow {
  matchId: string;
  name: string; // sanitized for display
  hostId: string;
  roomKey: string;
  players: number;
  maxPlayers: number;
  status: MatchInfo["status"];
  grp: GrpFingerprint;
  grpLabels: string[]; // sanitized component labels
  haveGrp: boolean; // our GRP set matches the host's
  canDownload: boolean; // host GRP is shareable (firewall lets it be transferred)
  needsPaidGrp: boolean; // we lack it and it is paid -> unjoinable without buying it
  ping: number | null; // ms, or null for unknown ("?")
  ts: number;
  raw: MatchInfo;
}

export interface LobbyBuildOpts {
  localGrp: GrpFingerprint | null; // our current GRP set fingerprint (null before boot)
  myRelayRttMs: number | null; // our median relay RTT (list-wide proxy leg)
  trueRtt?: Map<string, number>; // hostId -> measured data-channel RTT (overrides proxy)
  pingFilterMaxMs?: number; // Infinity / undefined -> no exclusion
}

/** Build the sorted, filtered, marked lobby rows. */
export function buildLobbyRows(matches: MatchInfo[], opts: LobbyBuildOpts): LobbyRow[] {
  const trueRtt = opts.trueRtt ?? new Map();
  const maxMs = opts.pingFilterMaxMs ?? Infinity;

  const rows: LobbyRow[] = matches.map((m) => {
    const haveGrp = fingerprintsMatch(opts.localGrp ?? undefined, m.grp);
    const cls = classifyFingerprint(m.grp);
    const ping = trueRtt.get(m.hostId) ?? estimatePing(opts.myRelayRttMs, m.pingHint);
    return {
      matchId: m.matchId,
      name: sanitizeText(m.name) || "Duke Match",
      hostId: m.hostId,
      roomKey: m.roomKey,
      players: m.players | 0,
      maxPlayers: m.maxPlayers | 0,
      status: m.status,
      grp: m.grp,
      grpLabels: (m.grp.labels ?? []).map((l) => sanitizeText(l) || "GRP"),
      haveGrp,
      canDownload: !haveGrp && cls.shareable,
      needsPaidGrp: !haveGrp && !cls.shareable,
      ping: ping ?? null,
      ts: m.ts,
      raw: m,
    };
  });

  const visible = rows.filter((r) => passesPingFilter(r.ping, maxMs));

  // Sort order (mirrored by the in-engine BROWSE list, which draws rows as given):
  //   (1) rooms whose GRP we HAVE, to the top — joinable without a download;
  //   (2) joinable-now: rooms with an OPEN slot before FULL ones (availability);
  //   (3) lower ping first; unknown ("?") sorts last but is never filtered out;
  //   (4) among equal-ping rooms, more-populated first (likelier to get a game going);
  //   (5) freshest last-seen first.
  visible.sort((a, b) => {
    if (a.haveGrp !== b.haveGrp) return a.haveGrp ? -1 : 1;
    const aOpen = a.players < a.maxPlayers;
    const bOpen = b.players < b.maxPlayers;
    if (aOpen !== bOpen) return aOpen ? -1 : 1;
    const pa = a.ping ?? Number.POSITIVE_INFINITY;
    const pb = b.ping ?? Number.POSITIVE_INFINITY;
    if (pa !== pb) return pa - pb;
    if (a.players !== b.players) return b.players - a.players;
    return b.ts - a.ts;
  });

  return visible;
}
