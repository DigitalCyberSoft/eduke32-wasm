// ─────────────────────────────────────────────────────────────────────────────
// GRP — fingerprinting, gating, transfer, and the startup-selector data model.
//
// A GRP "fingerprint" identifies the exact content a host is playing so the lobby
// can (a) gate joins to an identical GRP set and (b) decide whether the host's main
// GRP may be legally transferred to a joiner who lacks it.
//
//   crc     : standard IEEE CRC-32 (matches the engine's Bcrc32 — poly 0xEDB88320,
//             init/final 0xFFFFFFFF) so it lines up with grpscan.cpp's table.
//   sha256  : Web Crypto digest, computed by reading the Module.FS bytes in chunks
//             (yielding between chunks so a 44 MB hash never janks a frame).
//   setDigest: a hash over the ORDERED set (main GRP + each mod/CON/DEF in load
//             order), so two players match only if their whole content stack agrees.
//
// GRP TRANSFER is HASH-BEFORE-USE: a received GRP is fully reassembled and must
// hash to the fingerprint the host advertised BEFORE a single byte is written to
// IndexedDB or handed to the engine. The paid-retail firewall (see grptable.ts) is
// enforced on the SENDER, so paid content is never even chunked for the wire.
// ─────────────────────────────────────────────────────────────────────────────

import { classifyByCrc, mayShareCrc, type GrpClassification } from "./grptable";
import { sanitizeText } from "./sanitize";

// ── CRC-32 (standard IEEE / zlib; matches source/build/src/crc32.cpp Bcrc32) ──

const CRC_TABLE: Uint32Array = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    t[n] = c >>> 0;
  }
  return t;
})();

/** Feed bytes into a running CRC-32. Start with crc32Update(bytes) or chain chunks
 *  with the returned value; finalize with crc32Final(). Lets us CRC a GRP as its
 *  transfer chunks arrive, without buffering the whole file first. */
export function crc32Update(bytes: Uint8Array, running = 0xffffffff): number {
  let c = running >>> 0;
  for (let i = 0; i < bytes.length; i++) c = (c >>> 8) ^ CRC_TABLE[(c ^ bytes[i]) & 0xff];
  return c >>> 0;
}
export function crc32Final(running: number): number {
  return (running ^ 0xffffffff) >>> 0;
}
/** One-shot CRC-32 of a whole buffer (unsigned). */
export function crc32(bytes: Uint8Array): number {
  return crc32Final(crc32Update(bytes));
}

// ── SHA-256 (Web Crypto) ─────────────────────────────────────────────────────

function toHex(buf: ArrayBuffer): string {
  return [...new Uint8Array(buf)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

/** SHA-256 hex of a whole buffer (bytes already in memory — one native call). The
 *  Uint8Array view is passed directly (digest honors its byteOffset/byteLength). */
export async function sha256Hex(bytes: Uint8Array): Promise<string> {
  return toHex(await crypto.subtle.digest("SHA-256", bytes as unknown as BufferSource));
}

/** Minimal Emscripten FS surface we touch (typed so tests can pass a fake). */
export interface EmscriptenFS {
  readFile(path: string, opts?: { encoding?: "binary" }): Uint8Array;
  stat(path: string): { size: number };
  readdir(dir: string): string[];
  open(path: string, flags: string): number;
  read(stream: number, buffer: Uint8Array, offset: number, length: number, position: number): number;
  close(stream: number): void;
}

export const GRP_CHUNK_SIZE = 16 * 1024; // safe SCTP message size across browsers

/**
 * Read a Module.FS file in chunks, computing CRC-32 incrementally and SHA-256 over
 * the assembled bytes, yielding to the event loop between chunks. Returns the crc,
 * sha256, size, and the bytes (so a caller that needs them, e.g. to send, avoids a
 * second read). For files that are already a Uint8Array, use fingerprintBytes().
 */
export async function fingerprintFsFile(
  FS: EmscriptenFS,
  path: string,
  chunkSize = GRP_CHUNK_SIZE,
): Promise<{ crc: number; sha256: string; size: number; bytes: Uint8Array }> {
  const size = FS.stat(path).size;
  const bytes = new Uint8Array(size);
  const fd = FS.open(path, "r");
  try {
    let crc = 0xffffffff;
    let pos = 0;
    while (pos < size) {
      const n = Math.min(chunkSize, size - pos);
      FS.read(fd, bytes, pos, n, pos);
      crc = crc32Update(bytes.subarray(pos, pos + n), crc);
      pos += n;
      if ((pos / chunkSize) % 64 === 0) await Promise.resolve(); // periodic yield
    }
    const sha256 = await sha256Hex(bytes);
    return { crc: crc32Final(crc), sha256, size, bytes };
  } finally {
    FS.close(fd);
  }
}

/** Fingerprint a buffer already in memory. */
export async function fingerprintBytes(bytes: Uint8Array): Promise<{ crc: number; sha256: string; size: number }> {
  return { crc: crc32(bytes), sha256: await sha256Hex(bytes), size: bytes.length };
}

// ── GRP set fingerprint (what MatchInfo advertises) ──────────────────────────

/** One component of a loaded content stack, in load order (index 0 = main GRP). */
export interface GrpComponent {
  name: string; // file name / label
  crc: number; // unsigned
  sha256: string; // hex
  size: number;
}

export interface GrpFingerprint {
  /** SHA-256 over the ordered component digests — the join-gating key. */
  setDigest: string;
  /** The single GRP the host is "currently playing" (the only transfer candidate). */
  mainGrp: { crc: number; sha256: string; size: number };
  /** Sanitized human labels of each component in load order (main GRP first). */
  labels: string[];
  /** Main GRP is paid/official (retail base, add-on, or beta) — informational. */
  officialPaid: boolean;
  /** Main GRP is shareware AND therefore a firewall-eligible transfer candidate.
   *  (The host's runtime "allow download" toggle is applied separately, at offer.) */
  shareable: boolean;
}

/** Build the fingerprint from an ordered component list (main GRP at index 0). */
export async function setFingerprint(components: GrpComponent[]): Promise<GrpFingerprint> {
  if (components.length === 0) throw new Error("setFingerprint: empty component list");
  const main = components[0];
  // setDigest = SHA-256( concat over components of (sha256 bytes) ), order-sensitive.
  const enc = new TextEncoder();
  const parts = components.map((c) => `${c.sha256}:${c.crc >>> 0}`).join("|");
  const setDigest = await sha256Hex(enc.encode(parts));
  const cls = classifyByCrc(main.crc);
  return {
    setDigest,
    mainGrp: { crc: main.crc >>> 0, sha256: main.sha256, size: main.size },
    labels: components.map((c) => sanitizeText(c.name) || "GRP"),
    officialPaid: cls.officialPaid,
    shareable: cls.shareable,
  };
}

/** Two hosts' content stacks match iff their setDigests are equal. */
export function fingerprintsMatch(a: GrpFingerprint | undefined, b: GrpFingerprint | undefined): boolean {
  return !!a && !!b && a.setDigest === b.setDigest;
}

/** Convenience: classify a fingerprint's main GRP. */
export function classifyFingerprint(fp: GrpFingerprint): GrpClassification {
  return classifyByCrc(fp.mainGrp.crc);
}

// ── GRP transfer (chunked over duke-bulk, HASH-BEFORE-USE) ───────────────────

/** Metadata a host sends before streaming a GRP (validated on receive). */
export interface GrpOffer {
  crc: number;
  sha256: string;
  size: number;
  name: string;
  nchunks: number;
  chunkSize: number;
}

/** Reasons a host declines to serve its GRP (surfaced to the joiner's UI). */
export type GrpDenyReason = "paid" | "optout" | "notplaying" | "mismatch";

/**
 * Prepare a shareable GRP for sending. Returns null (with a reason) when the
 * firewall or the host's opt-out blocks it — the caller then sends a `grp_deny`.
 * This is the SENDER-side enforcement point: paid content never becomes chunks.
 */
export function prepareGrpSend(
  bytes: Uint8Array,
  crc: number,
  sha256: string,
  name: string,
  hostAllowsDownload: boolean,
  chunkSize = GRP_CHUNK_SIZE,
): { ok: true; offer: GrpOffer; chunk: (i: number) => Uint8Array } | { ok: false; reason: GrpDenyReason } {
  if (!mayShareCrc(crc)) return { ok: false, reason: "paid" }; // firewall: retail/add-on/unknown
  if (!hostAllowsDownload) return { ok: false, reason: "optout" }; // host toggle (default ON)
  const nchunks = Math.ceil(bytes.length / chunkSize) || 0;
  const offer: GrpOffer = { crc: crc >>> 0, sha256, size: bytes.length, name: sanitizeText(name) || "GRP", nchunks, chunkSize };
  const chunk = (i: number): Uint8Array => bytes.subarray(i * chunkSize, Math.min((i + 1) * chunkSize, bytes.length));
  return { ok: true, offer, chunk };
}

export type GrpReceiverState = "idle" | "receiving" | "verifying" | "verified" | "failed";

/**
 * Reassembles an incoming GRP and refuses to yield its bytes until they hash to the
 * ADVERTISED fingerprint. Chunk indices are validated; a completed transfer whose
 * crc OR sha256 disagrees fails closed (nothing is written / used).
 */
export class GrpReceiver {
  readonly offer: GrpOffer;
  private buf: Uint8Array;
  private got: boolean[];
  private received = 0;
  state: GrpReceiverState = "idle";

  constructor(offer: GrpOffer) {
    this.offer = offer;
    this.buf = new Uint8Array(offer.size);
    this.got = new Array(offer.nchunks).fill(false);
    this.state = "receiving";
  }

  /** Bytes received so far / total, in [0,1]. */
  get progress(): number {
    return this.offer.size === 0 ? 1 : Math.min(1, this.received / this.offer.size);
  }

  /** Accept one chunk. Returns true when it was a valid, not-yet-seen chunk. */
  accept(index: number, data: Uint8Array): boolean {
    if (this.state !== "receiving") return false;
    if (index < 0 || index >= this.offer.nchunks || this.got[index]) return false;
    const at = index * this.offer.chunkSize;
    if (at + data.length > this.offer.size) return false; // overlong: reject
    this.buf.set(data, at);
    this.got[index] = true;
    this.received += data.length;
    return true;
  }

  get complete(): boolean {
    return this.got.every(Boolean);
  }

  /** Number of LEADING chunks held (the resume point). duke-bulk is ordered, so
   *  after a connection loss the receiver holds exactly a contiguous prefix; a
   *  re-request asks the host to stream from here instead of restarting at 0. */
  contiguousCount(): number {
    let n = 0;
    while (n < this.offer.nchunks && this.got[n]) n++;
    return n;
  }

  /**
   * Verify the assembled bytes against the offer's crc AND sha256. Only on success
   * are the bytes returned; otherwise state -> "failed" and null is returned. HASH
   * BEFORE USE: callers MUST route the return value, never `this.buf` directly.
   */
  async verify(): Promise<Uint8Array | null> {
    if (!this.complete) return null;
    this.state = "verifying";
    const crc = crc32(this.buf);
    const sha = await sha256Hex(this.buf);
    if ((crc >>> 0) !== (this.offer.crc >>> 0) || sha !== this.offer.sha256) {
      this.state = "failed";
      return null;
    }
    this.state = "verified";
    return this.buf;
  }
}

// ── Startup GRP selector + manifest ──────────────────────────────────────────

/** A bundled free-GRP descriptor from grp-manifest.json. */
export interface ManifestGrp {
  id: string;
  name: string;
  crc: number; // unsigned
  size: number;
  /** Path the engine will find it at once present (for -gamegrp), e.g. "DUKE.GRP". */
  filename: string;
  /** How the page obtains the bytes: bundled with the .data image, or user-provided. */
  source: "bundled" | "user";
  shareware: boolean;
}

export interface GrpManifest {
  version: number;
  note?: string;
  grps: ManifestGrp[];
}

/** A launchable GRP the startup selector can offer. */
export interface SelectorEntry {
  id: string;
  name: string; // sanitized
  crc: number;
  size: number;
  filename: string;
  present: boolean; // found in Module.FS right now
  shareware: boolean;
  isDefault: boolean;
  source: ManifestGrp["source"] | "scanned";
}

/** Scan Module.FS for .grp files actually present (best-effort; mirrors index.html's
 *  settings-panel scan). Returns { filename, size } for each. */
export function scanFsGrps(FS: Pick<EmscriptenFS, "readdir" | "stat">, dirs: string[] = ["/", "/data"]): { filename: string; size: number }[] {
  const out: { filename: string; size: number }[] = [];
  for (const dir of dirs) {
    let names: string[];
    try {
      names = FS.readdir(dir);
    } catch {
      continue;
    }
    for (const n of names) {
      if (!/\.grp$/i.test(n)) continue;
      const p = (dir === "/" ? "" : dir) + "/" + n;
      let size = 0;
      try {
        size = FS.stat(p).size;
      } catch {
        /* ignore */
      }
      out.push({ filename: n, size });
    }
  }
  return out;
}

/**
 * Build the startup selector model from the bundled manifest plus whatever GRPs are
 * present in Module.FS. The default is the shareware GRP. Per the zero-friction rule,
 * `shouldShow` is false when shareware is the ONLY launchable GRP — the page then
 * boots straight into shareware with no prompt (today's one-click behavior).
 */
export function buildSelector(manifest: GrpManifest, fsGrps: { filename: string; size: number }[]): { entries: SelectorEntry[]; shouldShow: boolean; defaultId: string | null } {
  const entries: SelectorEntry[] = [];
  const seen = new Set<string>();

  // Manifest-declared GRPs (shareware today), marked present if the file is in FS.
  for (const g of manifest.grps) {
    const present = fsGrps.some((f) => f.filename.toLowerCase() === g.filename.toLowerCase());
    entries.push({
      id: g.id,
      name: sanitizeText(g.name) || g.filename,
      crc: g.crc >>> 0,
      size: g.size,
      filename: g.filename,
      present,
      shareware: g.shareware,
      isDefault: false,
      source: g.source,
    });
    seen.add(g.filename.toLowerCase());
  }

  // Any other GRP present in FS but not in the manifest (a user-provided retail GRP).
  // A size-only scan cannot know the CRC, so shareware defaults false (conservative);
  // the real classification happens by CRC at load time.
  for (const f of fsGrps) {
    if (seen.has(f.filename.toLowerCase())) continue;
    entries.push({
      id: "fs:" + f.filename,
      name: f.filename,
      crc: 0,
      size: f.size,
      filename: f.filename,
      present: true,
      shareware: false,
      isDefault: false,
      source: "scanned",
    });
  }

  // Default = the first present shareware entry (fallback: first shareware, then first).
  const launchable = entries.filter((e) => e.present);
  const def =
    launchable.find((e) => e.shareware) ??
    entries.find((e) => e.shareware) ??
    launchable[0] ??
    entries[0] ??
    null;
  if (def) def.isDefault = true;

  // Zero-friction: only show the selector when there is a REAL choice. If the sole
  // launchable GRP is the shareware one (today's default), boot straight in with no
  // prompt. Show it once there is more than one launchable GRP, or any launchable
  // non-shareware (retail) GRP the player might prefer.
  const hasNonShareware = launchable.some((e) => !e.shareware);
  const shouldShow = launchable.length > 1 || hasNonShareware;

  return { entries, shouldShow, defaultId: def ? def.id : null };
}
