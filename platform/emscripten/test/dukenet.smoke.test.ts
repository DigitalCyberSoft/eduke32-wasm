import { describe, it, expect } from "vitest";
import { allocateGuestSlot, canAdmitGuest, isGuestSlot } from "../net/duke-net";

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

describe("DukeNet guest slot bounds", () => {
  it("allocates only seats 1..15 while skipping humans and CPU reservations", () => {
    expect(allocateGuestSlot(new Set([1, 2, 4]), (1 << 3) | (1 << 5))).toBe(6);
    expect(isGuestSlot(1)).toBe(true);
    expect(isGuestSlot(15)).toBe(true);
    expect(isGuestSlot(0)).toBe(false);
    expect(isGuestSlot(16)).toBe(false);
  });

  it("returns no slot for a full 16-player roster instead of seat 16", () => {
    const full = new Set(Array.from({ length: 15 }, (_, i) => i + 1));
    expect(allocateGuestSlot(full, 0)).toBe(-1);
    expect(allocateGuestSlot(new Set(), 0xfffe)).toBe(-1);
    expect(canAdmitGuest(15, 16, allocateGuestSlot(full, 0))).toBe(false);
  });

  it("denies configured-full matches before slot mutation", () => {
    expect(canAdmitGuest(0, 2, 1)).toBe(true);
    expect(canAdmitGuest(1, 2, 2)).toBe(false);
    expect(canAdmitGuest(0, 16, -1)).toBe(false);
    expect(canAdmitGuest(0, 17, 1)).toBe(false);
  });
});
