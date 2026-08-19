// Stage-3 seam end-to-end: two processes linking the real net_transport_native.cpp
// (host + guest) connect over the local relay, run the join handshake, and
// exchange a frame through the net_transport.h seam. Both must print SEAM OK,
// with a NET_PEER_UP and the guest's LOCALIDX assignment.
//
// Run:  node platform/native/test/seam_e2e.mjs /path/to/nn_seam_test
import { startRelay } from "../../emscripten/test/net/nip01-relay.mjs";
import { spawn } from "node:child_process";

const BIN = process.argv[2] || "/tmp/nn_seam_test";
const LD = { ...process.env, LD_LIBRARY_PATH: "/tmp/localdev/usr/lib64", NN_NO_UPNP: "1" };
const KEY = btoa(String.fromCharCode(...globalThis.crypto.getRandomValues(new Uint8Array(32))));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const { url, wss } = await startRelay(0);
console.log("relay:", url, "\n");

function launch(name, env, onLine) {
  let out = "";
  const p = spawn(BIN, [], { env: { ...LD, ...env } });
  p.stdout.on("data", (d) => {
    out += d;
    process.stdout.write(`[${name}] ${d}`);
    if (onLine) String(d).split("\n").forEach((l) => l && onLine(l));
  });
  p.stderr.on("data", (d) => process.stderr.write(`[${name}!] ${d}`));
  const done = new Promise((res) => p.on("close", (code) => res({ code, out })));
  return { p, done: () => done };
}

// Host first; capture its device id from "[nnet] HOSTID <id>".
let hostId = null;
// Reserve seat 1 as an engine CPU. Admission runs in net_poll on the game
// thread and must assign the joining transport human the next unique seat.
const host = launch("host", { NN_ROLE: "host", NN_KEY: KEY, NN_RELAY: url, NN_PLAYERS: "2", NN_MAXPLAYERS: "16", NN_TEST_BOTMASK: "0x2", NN_TEST_KICK_REJOIN: "1", NN_NAME: "Host" }, (line) => {
  const m = line.match(/HOSTID (\S+)/);
  if (m) hostId = m[1];
});
for (let t = 0; t < 5000 && !hostId; t += 50) await sleep(50);
if (!hostId) {
  console.log("FAIL host never printed HOSTID");
  wss.close();
  process.exit(1);
}

// Join with ONLY the room key (no NN_HOSTID) - the guest must discover the host via
// its host-flagged presence, exactly like the Multiplayer menu's Join by Code.
const guest1 = launch("guest1", { NN_ROLE: "guest", NN_KEY: KEY, NN_RELAY: url, NN_NAME: "Guest1" });
const g1 = await guest1.done();
// Host kicked Guest1 after its first frame. A fresh device joins the same match;
// mapping cleanup must make seat 2 reusable rather than leaking capacity.
const guest2 = launch("guest2", { NN_ROLE: "guest", NN_KEY: KEY, NN_RELAY: url, NN_NAME: "Guest2" });

const [h, g2] = await Promise.all([host.done(), guest2.done()]);
wss.close();

let fail = 0;
const ok = (c, name) => { console.log(`${c ? "ok  " : "FAIL"} ${name}`); if (!c) fail++; };
ok(h.code === 0 && h.out.includes("KICK-REJOIN OK"), "host: kick/rejoin and frames through seam");
ok(g1.code !== null && g1.out.includes("SEAM OK"), "first guest joined and exchanged a frame");
ok(g2.code === 0 && g2.out.includes("SEAM OK") && g2.out.includes("HELLO-AFTER-REJOIN"), "replacement guest exchanged a frame");
ok(h.out.includes("ALLOCATOR OK") && g1.out.includes("ALLOCATOR OK") && g2.out.includes("ALLOCATOR OK"), "native capacity/CPU-seat allocator bounds");
ok(g1.out.includes("LOCALIDX 2") && g2.out.includes("LOCALIDX 2"), "kick/rejoin reused connectindex 2");
ok((h.out.match(/joined as slot 2/g) ?? []).length === 2, "host skipped CPU seat 1 and reused human seat 2");

console.log(fail ? `\nSEAM E2E FAIL (${fail})` : "\nSEAM E2E OK");
process.exit(fail ? 1 : 0);
