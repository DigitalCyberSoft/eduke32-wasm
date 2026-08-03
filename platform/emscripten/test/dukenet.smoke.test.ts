import { describe, it, expect } from "vitest";

// Import the FULL facade graph (duke-net -> match -> nostr/nostr-tools, peer, grp,
// seam, ping, lobby, idb). In the Node test environment `window` is undefined, so
// this proves nothing touches the DOM at import time and the singleton constructs
// without a browser. (Live WebRTC/Nostr behavior is exercised in test/harness.html.)
describe("duke-net module graph loads headless", () => {
  it("constructs the DukeNet singleton with the seam surface + public API", async () => {
    const mod = await import("../net/duke-net");
    const DN = mod.default;
    expect(DN).toBeTruthy();
    // seam surface the js-library (seam_library.js) calls
    for (const m of ["_seamSend", "_seamBroadcast", "_seamDrain", "_seamInit", "_seamShutdown"]) {
      expect(typeof (DN as unknown as Record<string, unknown>)[m]).toBe("function");
    }
    // public API the in-engine menu drives
    for (const m of ["host", "join", "leave", "startLobby", "setPingFilter", "setAllowGrpDownload", "setLocalGrp", "getMyConnectIndex"]) {
      expect(typeof (DN as unknown as Record<string, unknown>)[m]).toBe("function");
    }
  });

  it("seam drain is empty and broadcast is safe before any peer/init", async () => {
    const DN = (await import("../net/duke-net")).default;
    expect(DN._seamDrain()).toEqual([]);
    // No registered peers => broadcast is a no-op, must not throw.
    expect(() => DN._seamBroadcast(1, 1, new Uint8Array([1, 2, 3]))).not.toThrow();
  });
});
