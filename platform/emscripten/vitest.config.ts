import { defineConfig } from "vitest/config";

// Standalone transport tests. These run in Node against the pure logic (GRP
// classification + fingerprint, sanitizer, ping filter, lobby assembly, the seam
// queue/routing). The live WebRTC + Nostr 2-peer connect is a BROWSER concern and
// is exercised by test/harness.html (see docs/INTEGRATION.md), not here.
export default defineConfig({
  test: {
    environment: "node",
    include: ["test/**/*.test.ts"],
    pool: "forks",
    poolOptions: { forks: { minForks: 1, maxForks: 4 } },
  },
});
