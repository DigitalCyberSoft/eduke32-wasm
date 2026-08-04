// Stage-3 seam end-to-end: two processes linking the real net_transport_native.cpp
// (host + guest) connect over the local relay, run the join handshake, and
// exchange a frame through the net_transport.h seam. Both must print SEAM OK,
// with a NET_PEER_UP and the guest's LOCALIDX assignment.
//
// Run:  node platform/native/test/seam_e2e.mjs /path/to/nn_seam_test
import { startRelay } from "../../emscripten/test/net/nip01-relay.mjs";
import { spawn } from "node:child_process";

const BIN = process.argv[2] || "/tmp/nn_seam_test";
const LD = { ...process.env, LD_LIBRARY_PATH: "/tmp/localdev/usr/lib64" };
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
const host = launch("host", { NN_ROLE: "host", NN_KEY: KEY, NN_RELAY: url, NN_PLAYERS: "2", NN_NAME: "Host" }, (line) => {
  const m = line.match(/HOSTID (\S+)/);
  if (m) hostId = m[1];
});
for (let t = 0; t < 5000 && !hostId; t += 50) await sleep(50);
if (!hostId) {
  console.log("FAIL host never printed HOSTID");
  wss.close();
  process.exit(1);
}

const guest = launch("guest", { NN_ROLE: "guest", NN_KEY: KEY, NN_HOSTID: hostId, NN_RELAY: url, NN_NAME: "Guest" });

const [h, g] = await Promise.all([host.done(), guest.done()]);
wss.close();

let fail = 0;
const ok = (c, name) => { console.log(`${c ? "ok  " : "FAIL"} ${name}`); if (!c) fail++; };
ok(h.code === 0 && h.out.includes("SEAM OK"), "host: peer-up + frame through seam");
ok(g.code === 0 && g.out.includes("SEAM OK"), "guest: peer-up + frame through seam");
ok(g.out.includes("LOCALIDX 1"), "guest assigned connectindex 1 via Net_SetLocalIndex");
ok(h.out.includes("joined as slot 1"), "host assigned the guest slot 1");

console.log(fail ? `\nSEAM E2E FAIL (${fail})` : "\nSEAM E2E OK");
process.exit(fail ? 1 : 0);
