import { createHash } from "node:crypto";
import { readFile, stat } from "node:fs/promises";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";
import { crc32 } from "../net/grp";
import { classifyByCrc } from "../net/grptable";

const files = {
  "DUKE3D.GRP": {
    bytes: 11_035_779,
    sha256: "f943d0c2e2a0803a644a2107c81ea897dec87596d9dd1a6a432131ad6f5818d6",
  },
  "LICENSE.TXT": {
    bytes: 9_108,
    sha256: "38cd7edf73ba672db785a2c30ae0f2cceded7f94a6297397548ee8ce5312547d",
  },
} as const;

const repoRoot = resolve(import.meta.dirname, "../../..");

describe("bundled shareware gameplay fixture", () => {
  for (const [name, expected] of Object.entries(files)) {
    it(`${name} has the pinned size and SHA-256`, async () => {
      const path = resolve(repoRoot, "assets/shareware", name);
      const [info, bytes] = await Promise.all([stat(path), readFile(path)]);
      expect(info.isFile()).toBe(true);
      expect(info.size).toBe(expected.bytes);
      expect(createHash("sha256").update(bytes).digest("hex")).toBe(expected.sha256);
    });
  }

  it("the bundled GRP is the engine-known redistributable 1.3D shareware image", async () => {
    const bytes = await readFile(resolve(repoRoot, "assets/shareware/DUKE3D.GRP"));
    const crc = crc32(bytes);
    const classification = classifyByCrc(crc);
    expect(crc).toBe(0x983ad923);
    expect(classification).toMatchObject({
      known: true,
      name: "Duke Nukem 3D Shareware 1.3D",
      shareable: true,
      officialPaid: false,
      category: "shareware",
    });
  });
});
