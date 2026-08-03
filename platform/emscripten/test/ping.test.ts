import { describe, it, expect } from "vitest";
import { passesPingFilter, estimatePing, formatPing, PingTracker, PING_PRESETS } from "../net/ping";

describe("high-ping exclude filter", () => {
  it("includes unknown pings ('?') regardless of threshold", () => {
    expect(passesPingFilter(null, 100)).toBe(true);
    expect(passesPingFilter(undefined, 100)).toBe(true);
    expect(passesPingFilter(NaN, 100)).toBe(true);
  });
  it("passes everything when the threshold is 'Any'", () => {
    expect(passesPingFilter(9999, Infinity)).toBe(true);
  });
  it("excludes rows strictly over the threshold, keeps rows at/under", () => {
    expect(passesPingFilter(100, 100)).toBe(true);
    expect(passesPingFilter(101, 100)).toBe(false);
    expect(passesPingFilter(40, 100)).toBe(true);
  });
  it("formats unknown as '?' and known as 'NN ms'", () => {
    expect(formatPing(null)).toBe("?");
    expect(formatPing(NaN)).toBe("?");
    expect(formatPing(42.6)).toBe("43 ms");
  });
  it("ships a presets list starting with 'Any ping'", () => {
    expect(PING_PRESETS[0].maxMs).toBe(Infinity);
    expect(PING_PRESETS.length).toBeGreaterThan(1);
  });
});

describe("relay-RTT proxy estimate", () => {
  it("sums both legs, null if either is unknown", () => {
    expect(estimatePing(30, 40)).toBe(70);
    expect(estimatePing(null, 40)).toBeNull();
    expect(estimatePing(30, null)).toBeNull();
    expect(estimatePing(30, undefined)).toBeNull();
    expect(estimatePing(NaN, 40)).toBeNull();
  });
});

describe("PingTracker (true data-channel RTT)", () => {
  it("measures RTT from ping/pong and smooths it", () => {
    const t = new PingTracker();
    expect(t.rtt("host")).toBeNull();
    const id = t.startPing("host", 1000);
    const sample = t.onPong("host", id, 1050);
    expect(sample).toBe(50);
    expect(t.rtt("host")).toBe(50);
    // a second sample blends via EWMA (0.7 old + 0.3 new)
    const id2 = t.startPing("host", 2000);
    t.onPong("host", id2, 2150); // 150ms sample
    expect(t.rtt("host")).toBeCloseTo(50 * 0.7 + 150 * 0.3, 6);
  });
  it("ignores unknown/stale pong ids", () => {
    const t = new PingTracker();
    expect(t.onPong("host", 999)).toBeNull();
  });
  it("forgets a peer", () => {
    const t = new PingTracker();
    const id = t.startPing("p", 0);
    t.onPong("p", id, 10);
    t.forget("p");
    expect(t.rtt("p")).toBeNull();
  });
});
