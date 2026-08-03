// ─────────────────────────────────────────────────────────────────────────────
// GRP TABLE — a faithful transcription of the engine's own internalgrpfiles[]
// (source/duke3d/src/grpscan.cpp), used to CLASSIFY a GRP by its CRC-32.
//
// This is the authority behind two policies:
//   1. The shareable classification: GAMEFLAG_SHAREWARE == free to redistribute.
//   2. The paid-retail firewall: every retail base game (GAMEFLAG_DUKE/NAM/WW2GI
//      with NO shareware flag) and every add-on (GAMEFLAG_ADDON) is PAID and must
//      NEVER be transferred, regardless of any host "allow download" toggle.
//
// KEY ON CRC, NOT SIZE. Atomic (0xFD3DCFF1) and Atomic-WT (0x982AFE4A) are BOTH
// 44356548 bytes with different CRCs and different classification lookups, so size
// alone is ambiguous — the whole point of hashing.
//
// CRCs are stored UNSIGNED (the engine's #defines are int32_t; a JS CRC-32 of the
// bytes yields the same value unsigned). Compare with crc >>> 0.
// ─────────────────────────────────────────────────────────────────────────────

/** Symbolic GAMEFLAG_* names as they appear on each internalgrpfiles[] row. We only
 *  need the names the classification cares about; the numeric bit values live in the
 *  engine and are never needed here. */
export type GameFlag =
  | "DUKE"
  | "NAM"
  | "WW2GI"
  | "NAPALM"
  | "SHAREWARE"
  | "DUKEBETA"
  | "ADDON";

export interface GrpTableEntry {
  name: string;
  crc: number; // unsigned 32-bit
  size: number;
  flags: GameFlag[];
  /** dependency CRC (the base GRP an add-on needs), unsigned, or 0 for a base game. */
  dependency: number;
}

// Helper to keep the transcription readable and unsigned.
const u = (n: number): number => n >>> 0;

// ── The table (transcribed 1:1 from grpscan.cpp internalgrpfiles[]) ──────────
export const GRP_TABLE: readonly GrpTableEntry[] = [
  { name: "Duke Nukem 3D", crc: u(0xbbc9ce44), size: 26524524, flags: ["DUKE"], dependency: 0 },
  { name: "Duke Nukem 3D (South Korean Censored)", crc: u(0xaa4f6a40), size: 26385383, flags: ["DUKE"], dependency: 0 },
  { name: "Duke Nukem 3D: Atomic Edition", crc: u(0xfd3dcff1), size: 44356548, flags: ["DUKE"], dependency: 0 },
  { name: "Duke Nukem 3D: Atomic Edition (WT)", crc: u(0x982afe4a), size: 44356548, flags: ["DUKE"], dependency: 0 },
  { name: "Duke Nukem 3D: Plutonium Pak", crc: u(0xf514a6ac), size: 44348015, flags: ["DUKE"], dependency: 0 },
  { name: "Duke Nukem 3D Shareware 0.99", crc: u(0x02f18900), size: 9690241, flags: ["DUKE", "DUKEBETA"], dependency: 0 },
  { name: "Duke Nukem 3D Shareware 1.0", crc: u(0xa28aa589), size: 10429258, flags: ["DUKE", "SHAREWARE"], dependency: 0 },
  { name: "Duke Nukem 3D Shareware 1.1", crc: u(0x912e1e8d), size: 10442980, flags: ["DUKE", "SHAREWARE"], dependency: 0 },
  { name: "Duke Nukem 3D Shareware 1.3D", crc: u(0x983ad923), size: 11035779, flags: ["DUKE", "SHAREWARE"], dependency: 0 },
  { name: "Duke Nukem 3D Mac Demo", crc: u(0xc5f71561), size: 10444391, flags: ["DUKE", "SHAREWARE"], dependency: 0 },
  { name: "Duke Nukem 3D MacUser Demo", crc: u(0x73a15ee7), size: 10628573, flags: ["DUKE", "SHAREWARE"], dependency: 0 },
  { name: "Duke it out in D.C. (1.3D)", crc: u(0xa9242158), size: 7926624, flags: ["DUKE", "ADDON"], dependency: u(0xbbc9ce44) },
  { name: "Duke it out in D.C.", crc: u(0xb79d997f), size: 8225517, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke it out in D.C.", crc: u(0xa8cf80da), size: 8410183, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke it out in D.C.", crc: u(0x39a692bf), size: 8410187, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke it out in D.C.", crc: u(0xc63b6a8b), size: 8410149, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke Caribbean: Life's a Beach (1.3D)", crc: u(0x4a2dbb62), size: 23559381, flags: ["DUKE", "ADDON"], dependency: u(0xbbc9ce44) },
  { name: "Duke Caribbean: Life's a Beach (PPak)", crc: u(0x2f4fccee), size: 22551333, flags: ["DUKE", "ADDON"], dependency: u(0xf514a6ac) },
  { name: "Duke Caribbean: Life's a Beach", crc: u(0xb62b42fd), size: 22521880, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke Caribbean: Life's a Beach", crc: u(0x18f01c5b), size: 22213819, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke Caribbean: Life's a Beach", crc: u(0x65b5f690), size: 22397273, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke Caribbean: Life's a Beach", crc: u(0x64cf2351), size: 22213795, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke: Nuclear Winter", crc: u(0xf1cae8e4), size: 16169365, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke: Nuclear Winter Demo", crc: u(0xc7efbfa9), size: 10965909, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke!ZONE II (1.3D)", crc: u(0x82c1b47f), size: 26135388, flags: ["DUKE", "ADDON"], dependency: u(0xbbc9ce44) },
  { name: "Duke!ZONE II", crc: u(0x7fb6117c), size: 44100411, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke!ZONE II", crc: u(0x1e9516f1), size: 3186656, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke Nukem's Penthouse Paradise", crc: u(0x7cd82a3b), size: 2112419, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "Duke Nukem's Penthouse Paradise", crc: u(0xcf928a58), size: 4247491, flags: ["DUKE", "ADDON"], dependency: u(0xfd3dcff1) },
  { name: "NAM", crc: u(0x75c1f07b), size: 43448927, flags: ["NAM"], dependency: 0 },
  { name: "NAPALM", crc: u(0x3de1589a), size: 44365728, flags: ["NAM", "NAPALM"], dependency: 0 },
  { name: "WWII GI", crc: u(0x907b82bf), size: 77939508, flags: ["WW2GI"], dependency: 0 },
  { name: "Platoon Leader", crc: u(0xd1ed8c0c), size: 37852572, flags: ["WW2GI", "ADDON"], dependency: u(0x907b82bf) },
];

const _byCrc = new Map<number, GrpTableEntry>(GRP_TABLE.map((e) => [e.crc, e]));

export type GrpCategory = "shareware" | "retail" | "addon" | "beta" | "unknown";

export interface GrpClassification {
  known: boolean;
  name: string | null;
  size: number | null;
  flags: GameFlag[];
  /** GAMEFLAG_SHAREWARE — free to redistribute. The ONLY class the GRP firewall
   *  ever lets a host serve. */
  shareable: boolean;
  /** GAMEFLAG_ADDON — a paid add-on (needs, and is sold on top of, a base game). */
  addon: boolean;
  /** Paid/official content that is NOT freely shareable (retail base OR add-on OR
   *  the 0.99 beta). True for everything known that is not shareware. */
  officialPaid: boolean;
  category: GrpCategory;
}

/** Classify a GRP by its (unsigned) CRC-32 against the engine's table. */
export function classifyByCrc(crc: number): GrpClassification {
  const e = _byCrc.get(crc >>> 0);
  if (!e) {
    return { known: false, name: null, size: null, flags: [], shareable: false, addon: false, officialPaid: false, category: "unknown" };
  }
  const shareable = e.flags.includes("SHAREWARE");
  const addon = e.flags.includes("ADDON");
  const beta = e.flags.includes("DUKEBETA");
  const category: GrpCategory = shareable ? "shareware" : addon ? "addon" : beta ? "beta" : "retail";
  return {
    known: true,
    name: e.name,
    size: e.size,
    flags: e.flags,
    shareable,
    addon,
    officialPaid: !shareable, // retail base, add-on, or beta — none freely shareable
    category,
  };
}

/**
 * THE PAID-RETAIL FIREWALL (sender-side). True iff a GRP with this CRC may be sent
 * to another player. ONLY shareware GRPs are ever transferable; everything else —
 * every retail base game, every paid add-on, the 0.99 beta, AND any unknown CRC —
 * is blocked. This is enforced on the SENDER, so a host can never serve paid content
 * even if it wanted to, and it is independent of the host's "allow download" toggle
 * (the toggle can only make a shareable GRP MORE restricted, never less).
 */
export function mayShareCrc(crc: number): boolean {
  return classifyByCrc(crc).shareable;
}

/** The explicit blocklist: every known non-shareware CRC (unsigned). Handy for tests
 *  and for a UI "why can't I download this?" explanation. */
export const PAID_RETAIL_CRCS: readonly number[] = GRP_TABLE.filter(
  (e) => !e.flags.includes("SHAREWARE"),
).map((e) => e.crc);

/** The shareable set: every known shareware CRC (unsigned). */
export const SHAREWARE_CRCS: readonly number[] = GRP_TABLE.filter((e) =>
  e.flags.includes("SHAREWARE"),
).map((e) => e.crc);
