import { describe, it, expect } from "vitest";
import { classifyByCrc, mayShareCrc, PAID_RETAIL_CRCS, SHAREWARE_CRCS, GRP_TABLE } from "../net/grptable";

// CRCs from grpscan.cpp (unsigned).
const SW_13D = 0x983ad923 >>> 0; // Duke Nukem 3D Shareware 1.3D  (GAMEFLAG_SHAREWARE)
const ATOMIC = 0xfd3dcff1 >>> 0; // Atomic Edition                (retail)
const ATOMIC_WT = 0x982afe4a >>> 0; // Atomic Edition (WT)        (retail, SAME SIZE)
const DUKEDC = 0xa8cf80da >>> 0; // Duke it out in D.C.           (GAMEFLAG_ADDON)
const BETA_099 = 0x02f18900 >>> 0; // Shareware 0.99 beta         (GAMEFLAG_DUKEBETA)

describe("GRP classification (data-driven from the engine table)", () => {
  it("shareware 1.3D is shareable / transferable", () => {
    const c = classifyByCrc(SW_13D);
    expect(c.known).toBe(true);
    expect(c.shareable).toBe(true);
    expect(c.officialPaid).toBe(false);
    expect(c.category).toBe("shareware");
    expect(mayShareCrc(SW_13D)).toBe(true);
  });

  it("Atomic (retail) is BLOCKED from transfer", () => {
    const c = classifyByCrc(ATOMIC);
    expect(c.known).toBe(true);
    expect(c.shareable).toBe(false);
    expect(c.officialPaid).toBe(true);
    expect(c.category).toBe("retail");
    expect(mayShareCrc(ATOMIC)).toBe(false);
  });

  it("keys on CRC not size: Atomic and Atomic-WT share a size but classify separately", () => {
    const a = classifyByCrc(ATOMIC);
    const wt = classifyByCrc(ATOMIC_WT);
    expect(a.size).toBe(44356548);
    expect(wt.size).toBe(44356548); // identical byte count …
    expect(a.name).not.toBe(wt.name); // … but distinct entries
    expect(mayShareCrc(ATOMIC)).toBe(false);
    expect(mayShareCrc(ATOMIC_WT)).toBe(false);
  });

  it("paid add-ons (GAMEFLAG_ADDON) are blocked", () => {
    const c = classifyByCrc(DUKEDC);
    expect(c.addon).toBe(true);
    expect(c.shareable).toBe(false);
    expect(c.category).toBe("addon");
    expect(mayShareCrc(DUKEDC)).toBe(false);
  });

  it("the 0.99 beta (DUKEBETA, not SHAREWARE) is conservatively blocked", () => {
    const c = classifyByCrc(BETA_099);
    expect(c.shareable).toBe(false);
    expect(c.category).toBe("beta");
    expect(mayShareCrc(BETA_099)).toBe(false);
  });

  it("unknown CRCs fail closed (firewall blocks anything not explicitly shareware)", () => {
    const c = classifyByCrc(0xdeadbeef);
    expect(c.known).toBe(false);
    expect(c.shareable).toBe(false);
    expect(mayShareCrc(0xdeadbeef)).toBe(false);
  });

  it("the blocklist and shareware set partition the whole known table", () => {
    expect(PAID_RETAIL_CRCS.length + SHAREWARE_CRCS.length).toBe(GRP_TABLE.length);
    for (const crc of SHAREWARE_CRCS) expect(mayShareCrc(crc)).toBe(true);
    for (const crc of PAID_RETAIL_CRCS) expect(mayShareCrc(crc)).toBe(false);
    // Exactly the five shareware releases are transferable.
    expect(SHAREWARE_CRCS.length).toBe(5);
  });

  it("negative int32 #defines are stored unsigned and still match", () => {
    // 0xFD3DCFF1 as a signed int32 is negative; classifying it either way must hit
    // the same row (we normalize with >>> 0).
    expect(classifyByCrc((0xfd3dcff1 | 0) >>> 0).name).toBe("Duke Nukem 3D: Atomic Edition");
  });
});
