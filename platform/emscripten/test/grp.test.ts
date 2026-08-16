import { describe, it, expect } from "vitest";
import {
  crc32,
  crc32Update,
  crc32Final,
  sha256Hex,
  fingerprintFsFile,
  fingerprintBytes,
  setFingerprint,
  fingerprintsMatch,
  prepareGrpSend,
  GrpReceiver,
  buildSelector,
  scanFsGrps,
  GRP_CHUNK_SIZE,
  type EmscriptenFS,
  type GrpComponent,
  type GrpManifest,
  type GrpOffer,
} from "../net/grp";

// Build a self-consistent offer + chunker directly from arbitrary bytes (i.e.
// WITHOUT the sender firewall, which is exercised separately). This isolates the
// receiver's HASH-BEFORE-USE behavior from the shareware/paid policy.
async function offerFor(bytes: Uint8Array, chunkSize = GRP_CHUNK_SIZE): Promise<{ offer: GrpOffer; chunk: (i: number) => Uint8Array }> {
  const { crc, sha256, size } = await fingerprintBytes(bytes);
  const nchunks = Math.ceil(size / chunkSize) || 0;
  return {
    offer: { crc, sha256, size, name: "DUKE.GRP", nchunks, chunkSize },
    chunk: (i) => bytes.subarray(i * chunkSize, Math.min((i + 1) * chunkSize, size)),
  };
}

// ── a tiny in-memory Module.FS stand-in ─────────────────────────────────────
function fakeFs(files: Record<string, Uint8Array>): EmscriptenFS {
  const open = new Map<number, Uint8Array>();
  let fd = 3;
  return {
    readFile: (p) => files[p],
    stat: (p) => ({ size: files[p].length }),
    readdir: (dir) =>
      Object.keys(files)
        .filter((p) => p.startsWith(dir === "/" ? "/" : dir + "/"))
        .map((p) => p.slice((dir === "/" ? "/" : dir + "/").length))
        .filter((n) => !n.includes("/")),
    open: (p) => {
      const h = fd++;
      open.set(h, files[p]);
      return h;
    },
    read: (h, buffer, offset, length, position) => {
      const src = open.get(h)!;
      const n = Math.max(0, Math.min(length, src.length - position));
      buffer.set(src.subarray(position, position + n), offset);
      return n;
    },
    close: (h) => void open.delete(h),
  };
}

function pattern(n: number, seed = 1): Uint8Array {
  const b = new Uint8Array(n);
  let x = seed >>> 0;
  for (let i = 0; i < n; i++) {
    x = (x * 1664525 + 1013904223) >>> 0;
    b[i] = x & 0xff;
  }
  return b;
}

describe("CRC-32 (must equal the engine's Bcrc32)", () => {
  it("matches the canonical check value for '123456789'", () => {
    const bytes = new TextEncoder().encode("123456789");
    expect(crc32(bytes) >>> 0).toBe(0xcbf43926 >>> 0);
  });
  it("is incremental (chunked == whole)", () => {
    const b = pattern(5000);
    const whole = crc32(b);
    let run = 0xffffffff;
    for (let i = 0; i < b.length; i += 777) run = crc32Update(b.subarray(i, i + 777), run);
    expect(crc32Final(run)).toBe(whole);
  });
  it("crc of empty input is 0", () => {
    expect(crc32(new Uint8Array(0))).toBe(0);
  });
});

describe("fingerprintFsFile — chunked read over Module.FS", () => {
  it("produces the same crc + sha256 as a one-shot fingerprint of the bytes", async () => {
    const bytes = pattern(GRP_CHUNK_SIZE * 3 + 137); // spans several chunks + a tail
    const FS = fakeFs({ "/DUKE.GRP": bytes });
    const fp = await fingerprintFsFile(FS, "/DUKE.GRP", 4096);
    const ref = await fingerprintBytes(bytes);
    expect(fp.crc).toBe(ref.crc);
    expect(fp.sha256).toBe(ref.sha256);
    expect(fp.size).toBe(bytes.length);
    expect(fp.sha256).toBe(await sha256Hex(bytes));
  });
});

describe("setFingerprint — the join-gating set digest", () => {
  const mk = (name: string, crc: number, sha: string, size: number): GrpComponent => ({ name, crc, sha256: sha, size });

  it("is order-sensitive and content-sensitive", async () => {
    const a = mk("DUKE.GRP", 0x983ad923, "aa", 100);
    const b = mk("MOD.CON", 0x1111, "bb", 10);
    const f1 = await setFingerprint([a, b]);
    const f2 = await setFingerprint([a, b]);
    const f3 = await setFingerprint([b, a]); // reordered
    const f4 = await setFingerprint([a, mk("MOD.CON", 0x1111, "cc", 10)]); // changed mod
    expect(f1.setDigest).toBe(f2.setDigest);
    expect(f1.setDigest).not.toBe(f3.setDigest);
    expect(f1.setDigest).not.toBe(f4.setDigest);
    expect(fingerprintsMatch(f1, f2)).toBe(true);
    expect(fingerprintsMatch(f1, f3)).toBe(false);
  });

  it("derives shareable/officialPaid from the MAIN GRP classification", async () => {
    const sw = await setFingerprint([mk("DUKE.GRP", 0x983ad923, "aa", 11035779)]);
    expect(sw.shareable).toBe(true);
    expect(sw.officialPaid).toBe(false);
    const atomic = await setFingerprint([mk("DUKE.GRP", 0xfd3dcff1, "bb", 44356548)]);
    expect(atomic.shareable).toBe(false);
    expect(atomic.officialPaid).toBe(true);
  });

  it("sanitizes component labels (no '^', no control bytes)", async () => {
    const fp = await setFingerprint([mk("^10EVIL\tNAME", 0x983ad923, "aa", 1)]);
    expect(fp.labels[0]).toBe("10EVILNAME");
  });
});

describe("GRP transfer — firewall on send, HASH-BEFORE-USE on receive", () => {
  it("prepareGrpSend blocks paid, honors opt-out, allows shareware", () => {
    const bytes = pattern(1000);
    // paid retail CRC -> blocked regardless of the toggle
    expect(prepareGrpSend(bytes, 0xfd3dcff1, "x", "Atomic", true).ok).toBe(false);
    // shareware but host opted out -> blocked
    const optout = prepareGrpSend(bytes, 0x983ad923, "x", "SW", false);
    expect(optout.ok).toBe(false);
    if (!optout.ok) expect(optout.reason).toBe("optout");
    // shareware + allowed -> ok
    const ok = prepareGrpSend(bytes, 0x983ad923, "x", "SW", true);
    expect(ok.ok).toBe(true);
  });

  it("a receiver yields bytes only when crc AND sha256 match the offer", async () => {
    const bytes = pattern(GRP_CHUNK_SIZE * 2 + 500);
    const { offer, chunk } = await offerFor(bytes);
    const recv = new GrpReceiver(offer);
    for (let i = 0; i < offer.nchunks; i++) recv.accept(i, chunk(i));
    expect(recv.complete).toBe(true);
    const out = await recv.verify();
    expect(out).not.toBeNull();
    expect(out!.length).toBe(bytes.length);
    expect(recv.state).toBe("verified");
  });

  it("a tampered transfer fails closed (no bytes yielded)", async () => {
    const bytes = pattern(GRP_CHUNK_SIZE + 10);
    const { offer, chunk } = await offerFor(bytes);
    const recv = new GrpReceiver(offer);
    for (let i = 0; i < offer.nchunks; i++) {
      const c = chunk(i).slice();
      if (i === 0) c[0] ^= 0xff; // flip a byte in flight
      recv.accept(i, c);
    }
    const out = await recv.verify();
    expect(out).toBeNull();
    expect(recv.state).toBe("failed");
  });

  it("rejects out-of-range / duplicate chunks", () => {
    const bytes = pattern(100);
    const ref0 = crc32(bytes);
    const recv = new GrpReceiver({ crc: ref0, sha256: "x", size: 100, name: "G", nchunks: 2, chunkSize: 64 });
    expect(recv.accept(5, new Uint8Array(10))).toBe(false); // out of range
    expect(recv.accept(0, new Uint8Array(64))).toBe(true);
    expect(recv.accept(0, new Uint8Array(64))).toBe(false); // duplicate
  });

  it("resume: contiguousCount is the exact restart point and the tail completes + verifies", async () => {
    // A partial receiver that lost its connection after N chunks (ordered channel
    // => the survivor holds exactly a contiguous prefix). A resumed stream from
    // contiguousCount() must complete to a verifying transfer.
    const bytes = pattern(GRP_CHUNK_SIZE * 5 + 123);
    const { offer, chunk } = await offerFor(bytes);
    const recv = new GrpReceiver(offer);
    expect(recv.contiguousCount()).toBe(0);
    for (let i = 0; i < 3; i++) recv.accept(i, chunk(i)); // then the channel died
    expect(recv.contiguousCount()).toBe(3);
    expect(recv.complete).toBe(false);
    for (let i = recv.contiguousCount(); i < offer.nchunks; i++) recv.accept(i, chunk(i)); // host resumes at 3
    expect(recv.contiguousCount()).toBe(offer.nchunks);
    expect(recv.complete).toBe(true);
    const out = await recv.verify();
    expect(out).not.toBeNull();
    expect(recv.state).toBe("verified");
  });

  it("resume: progress reflects the surviving prefix (no restart from 0%)", async () => {
    const bytes = pattern(GRP_CHUNK_SIZE * 4);
    const { offer, chunk } = await offerFor(bytes);
    const recv = new GrpReceiver(offer);
    recv.accept(0, chunk(0));
    recv.accept(1, chunk(1));
    expect(recv.progress).toBeCloseTo(0.5);
    expect(recv.contiguousCount()).toBe(2);
  });
});

describe("startup selector model", () => {
  const manifest: GrpManifest = {
    version: 1,
    grps: [
      { id: "sw", name: "Duke Nukem 3D Shareware 1.3D", crc: 0x983ad923 >>> 0, size: 11035779, filename: "DUKE.GRP", source: "bundled", shareware: true },
    ],
  };

  it("scans FS for .grp files", () => {
    const FS = fakeFs({ "/DUKE.GRP": new Uint8Array(4), "/notes.txt": new Uint8Array(1), "/data/EXTRA.GRP": new Uint8Array(2) });
    const found = scanFsGrps(FS);
    expect(found.map((f) => f.filename).sort()).toEqual(["DUKE.GRP", "EXTRA.GRP"]);
  });

  it("does NOT show the selector when shareware is the only launchable GRP", () => {
    const sel = buildSelector(manifest, [{ filename: "DUKE.GRP", size: 11035779 }]);
    expect(sel.shouldShow).toBe(false);
    expect(sel.defaultId).toBe("sw");
    expect(sel.entries.find((e) => e.id === "sw")!.present).toBe(true);
  });

  it("shows the selector when a retail GRP is also present, defaulting to shareware", () => {
    const sel = buildSelector(manifest, [
      { filename: "DUKE.GRP", size: 11035779 },
      { filename: "ATOMIC.GRP", size: 44356548 },
    ]);
    expect(sel.shouldShow).toBe(true);
    expect(sel.defaultId).toBe("sw"); // shareware stays the default
    expect(sel.entries.some((e) => e.filename === "ATOMIC.GRP")).toBe(true);
  });
});
