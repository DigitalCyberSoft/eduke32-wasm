// Stage-2 WebRTC end-to-end: two native peers (NostrClient + PeerManager)
// discover each other via presence over the local relay and connect via WebRTC,
// then exchange a control message. Both must print CONNECTED + PEER OK.
//
// Run:  node platform/native/test/peer_e2e.mjs /path/to/nn_peer_test
import { startRelay } from "../../emscripten/test/net/nip01-relay.mjs";
import { spawn } from "node:child_process";

const BIN = process.argv[2] || "/tmp/nn_peer_test";
const LD = { ...process.env, LD_LIBRARY_PATH: "/tmp/localdev/usr/lib64" };
const KEY = btoa(String.fromCharCode(...globalThis.crypto.getRandomValues(new Uint8Array(32))));

function runPeer(name, url) {
  return new Promise((resolve) => {
    let out = "";
    const p = spawn(BIN, [url, KEY, name], { env: LD });
    p.stdout.on("data", (d) => {
      out += d;
      process.stdout.write(`[${name}] ${d}`);
    });
    p.stderr.on("data", () => {});
    p.on("close", (code) => resolve({ code, out }));
  });
}

const { url, wss } = await startRelay(0);
console.log("relay:", url, "\n");

const [a, b] = await Promise.all([runPeer("A", url), runPeer("B", url)]);
wss.close();

let fail = 0;
const ok = (c, name) => { console.log(`${c ? "ok  " : "FAIL"} ${name}`); if (!c) fail++; };
ok(a.code === 0 && a.out.includes("PEER OK"), "A connected + received peer control");
ok(b.code === 0 && b.out.includes("PEER OK"), "B connected + received peer control");
ok(a.out.includes("CONNECTED") && b.out.includes("CONNECTED"), "both opened all data channels");

console.log(fail ? `\nPEER E2E FAIL (${fail})` : "\nPEER E2E OK");
process.exit(fail ? 1 : 0);
