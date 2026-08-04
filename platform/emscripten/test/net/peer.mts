// Headless Duke peer for the integration harness. Polyfills the browser globals the
// transport needs (WebRTC via werift; storage stubs) so the REAL transport runs in
// Node, points it at a local relay, and hosts or joins. One process = one module graph
// = one node-<uid> DEVICE_ID (identity.ts:42), mirroring a single browser tab.
//
//   node_modules/.bin/tsx peer.mts host  <relayUrl>
//   node_modules/.bin/tsx peer.mts guest <relayUrl> <inviteCode>
import { RTCPeerConnection as WeriftPC } from "werift";

// Force host-candidate-only ICE (no STUN) so two localhost peers connect fast offline.
class LocalPC extends WeriftPC {
  constructor(cfg?: unknown) { super({ ...(cfg as object), iceServers: [] }); }
}
const g = globalThis as Record<string, unknown>;
g.RTCPeerConnection = LocalPC;
// werift's setRemoteDescription/addIceCandidate accept the plain {type,sdp} / candidate-init
// the transport already serializes, but REJECT `new WeriftSDP({type,sdp})`. So make these
// pass-throughs: `new RTCSessionDescription(init)` returns `init` unchanged.
g.RTCSessionDescription = class { constructor(x: unknown) { return x as object; } };
g.RTCIceCandidate = class { constructor(x: unknown) { return x as object; } };

class Mem {
  private m = new Map<string, string>();
  getItem(k: string) { return this.m.get(k) ?? null; }
  setItem(k: string, v: string) { this.m.set(k, String(v)); }
  removeItem(k: string) { this.m.delete(k); }
  clear() { this.m.clear(); }
  key(i: number) { return [...this.m.keys()][i] ?? null; }
  get length() { return this.m.size; }
}
g.sessionStorage = new Mem();
g.localStorage = new Mem();
// GRP cache is never exercised here (both peers pre-share the fingerprint); a stub that
// resolves nothing keeps idb.ts from throwing if it is touched.
g.indexedDB = { open() { const r: Record<string, unknown> = {}; queueMicrotask(() => (r.onerror as ((e: unknown) => void))?.({ target: r })); return r; } };

const [role, relayUrl, inviteCode] = process.argv.slice(2);
const emit = (o: unknown) => process.stdout.write(JSON.stringify(o) + "\n");
// test hooks: force a low local-only threshold so a localhost peer trips the RTT gate.
if (process.env.LO_MAX) g.__DUKE_LO_MAX_MS__ = Number(process.env.LO_MAX);

const dukeNet = (await import("../../net/duke-net.ts")).default;
const { setRelayOverride } = await import("../../net/netconfig.ts");
const { fingerprintBytes } = await import("../../net/grp.ts");

setRelayOverride([relayUrl]);

// identical fake GRP on both peers -> matching fingerprint -> the host's GRP gate passes.
const bytes = new Uint8Array([0x44, 0x55, 0x4b, 0x45, 1, 2, 3, 4]);
const fp = await fingerprintBytes(bytes);
await dukeNet.setLocalGrp([{ name: "DUKE3D.GRP", crc: fp.crc, sha256: fp.sha256, size: fp.size }], bytes);

dukeNet.on({
  onStatus: (s: string) => emit({ ev: "status", role, s }),
  onError: (s: string) => emit({ ev: "error", role, s }),
  onConnection: (peer: string, state: string) => emit({ ev: "conn", role, peer: peer.slice(0, 8), state }),
  onRoster: (players: { name: string }[]) => emit({ ev: "roster", role, count: players.length }),
  onJoined: (i: { myConnectIndex: number }) => emit({ ev: "joined", role, slot: i.myConnectIndex }),
});

if (role === "host") {
  const r = await dukeNet.host({ name: "HeadlessDM", isPublic: false, maxPlayers: 8, localOnly: process.env.LOCALONLY === "1" });
  emit({ ev: "hosted", role, inviteCode: r.inviteCode, matchId: r.matchId });
} else {
  await dukeNet.join(inviteCode);
  emit({ ev: "join-called", role });
}

setTimeout(() => { emit({ ev: "timeout", role }); process.exit(0); }, 25000);
