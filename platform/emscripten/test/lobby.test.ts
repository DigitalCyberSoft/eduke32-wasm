import { describe, it, expect } from "vitest";
import { buildLobbyRows } from "../net/lobby";
import type { MatchInfo } from "../net/match";
import type { GrpFingerprint } from "../net/grp";

const swFp = (setDigest: string): GrpFingerprint => ({
  setDigest,
  mainGrp: { crc: 0x983ad923 >>> 0, sha256: "sw", size: 11035779 }, // shareware -> shareable
  labels: ["Duke Nukem 3D Shareware 1.3D"],
  officialPaid: false,
  shareable: true,
});
const atomicFp = (setDigest: string): GrpFingerprint => ({
  setDigest,
  mainGrp: { crc: 0xfd3dcff1 >>> 0, sha256: "at", size: 44356548 }, // retail -> paid
  labels: ["Duke Nukem 3D: Atomic Edition"],
  officialPaid: true,
  shareable: false,
});

const mk = (id: string, grp: GrpFingerprint, pingHint: number | null): MatchInfo => ({
  v: 1,
  matchId: id,
  name: id,
  hostId: "host-" + id,
  roomKey: "k",
  maxPlayers: 8,
  players: 1,
  status: "open",
  grp,
  pingHint,
  ts: Date.now(),
});

describe("buildLobbyRows — the user's lobby rules", () => {
  const local = swFp("SW"); // we hold the shareware set "SW"

  const matches = [
    mk("mine", swFp("SW"), 20), // same set -> haveGrp
    mk("paid", atomicFp("ATOMIC"), 30), // paid, we lack it -> needsPaidGrp
    mk("free", swFp("SW2"), null), // other shareware set, unknown ping -> downloadable
  ];

  it("sorts matches whose GRP we HAVE to the top", () => {
    const rows = buildLobbyRows(matches, { localGrp: local, myRelayRttMs: 10 });
    expect(rows[0].matchId).toBe("mine");
    expect(rows[0].haveGrp).toBe(true);
    expect(rows.slice(1).every((r) => !r.haveGrp)).toBe(true);
  });

  it("marks a paid GRP we lack as needsPaidGrp, and a shareable one as canDownload", () => {
    const rows = buildLobbyRows(matches, { localGrp: local, myRelayRttMs: 10 });
    const paid = rows.find((r) => r.matchId === "paid")!;
    const free = rows.find((r) => r.matchId === "free")!;
    expect(paid.needsPaidGrp).toBe(true);
    expect(paid.canDownload).toBe(false);
    expect(free.needsPaidGrp).toBe(false);
    expect(free.canDownload).toBe(true);
  });

  it("estimates ping from both relay legs, unknown stays null", () => {
    const rows = buildLobbyRows(matches, { localGrp: local, myRelayRttMs: 10 });
    expect(rows.find((r) => r.matchId === "mine")!.ping).toBe(30); // 10 + 20
    expect(rows.find((r) => r.matchId === "paid")!.ping).toBe(40); // 10 + 30
    expect(rows.find((r) => r.matchId === "free")!.ping).toBeNull(); // hint unknown
  });

  it("true data-channel RTT overrides the proxy", () => {
    const trueRtt = new Map([["host-mine", 5]]);
    const rows = buildLobbyRows(matches, { localGrp: local, myRelayRttMs: 10, trueRtt });
    expect(rows.find((r) => r.matchId === "mine")!.ping).toBe(5);
  });

  it("the high-ping filter excludes slow rows but keeps unknown ('?')", () => {
    const rows = buildLobbyRows(matches, { localGrp: local, myRelayRttMs: 10, pingFilterMaxMs: 35 });
    const ids = rows.map((r) => r.matchId);
    expect(ids).toContain("mine"); // 30 <= 35
    expect(ids).not.toContain("paid"); // 40 > 35 -> excluded
    expect(ids).toContain("free"); // unknown -> always shown
  });

  it("sanitizes display names", () => {
    const evil = mk("evil", swFp("SW"), 10);
    evil.name = "^10ha\tx";
    const rows = buildLobbyRows([evil], { localGrp: local, myRelayRttMs: 0 });
    expect(rows[0].name).toBe("10hax");
  });
});
