import { describe, it, expect } from "vitest";
import { matchHasOpenSlot } from "../net/match";

// The host's capacity/started gate (Match._acceptsPeer / _onPresence) is built on this
// pure predicate. roster.size counts the host itself, so size < maxPlayers == room for
// one more joiner, and a started match (status !== "open") takes nobody new.
describe("matchHasOpenSlot — host capacity + started gate", () => {
  it("open match with a free slot has room", () => {
    expect(matchHasOpenSlot(1, 4, "open")).toBe(true); // host alone, cap 4
    expect(matchHasOpenSlot(3, 4, "open")).toBe(true); // 3/4 -> one more fits
  });

  it("open match at capacity has no room (size counts self)", () => {
    expect(matchHasOpenSlot(4, 4, "open")).toBe(false); // 4/4 full
    expect(matchHasOpenSlot(5, 4, "open")).toBe(false); // never over-fill
  });

  it("a started match takes nobody new regardless of free slots", () => {
    expect(matchHasOpenSlot(1, 8, "playing")).toBe(false);
    expect(matchHasOpenSlot(2, 8, "starting")).toBe(false);
  });

  it("boundary: two-player match", () => {
    expect(matchHasOpenSlot(1, 2, "open")).toBe(true); // host waiting for one
    expect(matchHasOpenSlot(2, 2, "open")).toBe(false); // both slots taken
  });
});
