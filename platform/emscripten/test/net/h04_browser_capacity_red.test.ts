import { readFile } from "node:fs/promises";
import { resolve } from "node:path";
import { describe, expect, it } from "vitest";

// H-04 baseline characterization at the transport boundary. This source-level
// gate is intentionally narrow: a 16-seat engine cannot legally scan to 63 or
// synthesize slots.size+1 when exhausted. Set PHASE1_EXPECT_FIXED=1 to run it as
// the post-fix gate; default test runs record the expected semantic baseline bug.
describe("browser seat capacity RED (H-04)", () => {
  it("records the baseline seat-16/exhaustion defect", async () => {
    const source = await readFile(resolve(import.meta.dirname, "../../net/duke-net.ts"), "utf8");
    const fixed = /const\s+NET_MAX_SEATS\s*=\s*16|NET_MAX_SEATS\s+from/.test(source)
      && /for\s*\([^)]*<\s*NET_MAX_SEATS/.test(source)
      && !source.includes("for (let s = 1; s < 64; s++)")
      && !source.includes("return this.slots.size + 1");
    if (process.env.PHASE1_EXPECT_FIXED === "1") expect(fixed).toBe(true);
    else expect(fixed).toBe(false);
  });
});
