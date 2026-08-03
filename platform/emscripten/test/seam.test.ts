import { describe, it, expect } from "vitest";
import { Seam, NET_PEER_UP, NET_PEER_DOWN, NET_CHAN_MOVE, NET_CHAN_REL } from "../net/seam";

describe("Seam — token<->device mapping and routing", () => {
  it("maps tokens to devices both ways", () => {
    const s = new Seam();
    s.registerPeer("devA", 1);
    s.registerPeer("devB", 2);
    expect(s.tokenOf("devA")).toBe(1);
    expect(s.deviceOf(2)).toBe("devB");
    expect(s.connectedTokens().sort()).toEqual([1, 2]);
    s.unregisterPeer("devA");
    expect(s.tokenOf("devA")).toBeUndefined();
    expect(s.deviceOf(1)).toBeUndefined();
  });

  it("net_send routes to the right peer + channel; unknown token is a no-op", () => {
    const s = new Seam();
    const sent: Array<{ dev: string; ch: number; bytes: number[] }> = [];
    s.setSender((dev, ch, bytes) => {
      sent.push({ dev, ch, bytes: [...bytes] });
      return true;
    });
    s.registerPeer("devA", 7);
    s.send(7, NET_CHAN_MOVE, 0, new Uint8Array([1, 2, 3]));
    s.send(99, NET_CHAN_REL, 1, new Uint8Array([9])); // unknown token
    expect(sent).toEqual([{ dev: "devA", ch: NET_CHAN_MOVE, bytes: [1, 2, 3] }]);
  });

  it("net_broadcast reaches every registered peer (never self)", () => {
    const s = new Seam();
    const got: string[] = [];
    s.setSender((dev) => {
      got.push(dev);
      return true;
    });
    s.registerPeer("devA", 1);
    s.registerPeer("devB", 2);
    s.broadcast(NET_CHAN_REL, 1, new Uint8Array([0]));
    expect(got.sort()).toEqual(["devA", "devB"]);
  });
});

describe("Seam — inbound queue ordering and copy semantics", () => {
  it("drains frames and peer events in enqueue order", () => {
    const s = new Seam();
    s.registerPeer("devA", 3);
    s.enqueuePeerEventByDevice("devA", true);
    s.enqueueFrameByDevice("devA", NET_CHAN_REL, new Uint8Array([10, 11]));
    s.enqueuePeerEventByDevice("devA", false);
    const items = s.drain();
    expect(items).toHaveLength(3);
    expect(items[0]).toEqual({ kind: 1, peer: 3, event: NET_PEER_UP });
    expect(items[1]).toMatchObject({ kind: 0, peer: 3, channel: NET_CHAN_REL });
    expect(items[2]).toEqual({ kind: 1, peer: 3, event: NET_PEER_DOWN });
    expect(s.drain()).toHaveLength(0); // queue cleared
  });

  it("copies frame bytes (mutating the source view after enqueue is safe)", () => {
    const s = new Seam();
    s.registerPeer("devA", 1);
    const view = new Uint8Array([1, 2, 3]);
    s.enqueueFrameByDevice("devA", NET_CHAN_MOVE, view);
    view[0] = 0xff; // mutate after enqueue
    const [item] = s.drain();
    expect(item.kind).toBe(0);
    if (item.kind === 0) expect([...item.data]).toEqual([1, 2, 3]);
  });

  it("drops frames/events from unregistered devices", () => {
    const s = new Seam();
    s.enqueueFrameByDevice("ghost", NET_CHAN_REL, new Uint8Array([1]));
    s.enqueuePeerEventByDevice("ghost", true);
    expect(s.drain()).toHaveLength(0);
  });

  it("shutdown clears the active flag, maps, and queue", () => {
    const s = new Seam();
    s.init();
    expect(s.active).toBe(true);
    s.registerPeer("devA", 1);
    s.enqueueFrameByDevice("devA", NET_CHAN_REL, new Uint8Array([1]));
    s.shutdown();
    expect(s.active).toBe(false);
    expect(s.connectedTokens()).toHaveLength(0);
    expect(s.drain()).toHaveLength(0);
  });
});
