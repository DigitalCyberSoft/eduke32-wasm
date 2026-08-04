// Stage-1 relay END-TO-END interop: the C++ NostrClient and the browser
// transport's crypto (nostr-tools finalizeEvent + WebCrypto, exactly as
// nostr.ts) exchange encrypted ephemeral events over the local NIP-01 relay.
//
//   A) C++ self round-trip (publish -> receive own event)
//   B) node (browser stack) publish -> C++ receive + decrypt
//   C) C++ publish -> node (browser stack) receive + decrypt
//
// Run:  node platform/native/test/relay_e2e.mjs /path/to/nn_relay_test
import { startRelay } from "../../emscripten/test/net/nip01-relay.mjs";
import { spawn, execFileSync } from "node:child_process";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";

const here = path.dirname(fileURLToPath(import.meta.url));
const req = createRequire(path.join(path.resolve(here, "../../emscripten"), "package.json"));
const pure = await import(req.resolve("nostr-tools/pure"));
const getPublicKey = pure.getPublicKey ?? pure.default?.getPublicKey;
const finalizeEvent = pure.finalizeEvent ?? pure.default?.finalizeEvent;

const BIN = process.argv[2] || "/tmp/nn_relay_test";
const LD = { ...process.env, LD_LIBRARY_PATH: "/tmp/localdev/usr/lib64" };
const KIND = 20079;
const subtle = globalThis.crypto.subtle;

const b64ToBytes = (b) => Uint8Array.from(atob(b), (c) => c.charCodeAt(0));
const bytesToB64 = (u) => btoa(String.fromCharCode(...u));
const KEY = bytesToB64(globalThis.crypto.getRandomValues(new Uint8Array(32)));

// nostr.ts crypto (verbatim: JSON.stringify wrapper is part of the layer)
async function encrypt(data, b64Key) {
  const key = await subtle.importKey("raw", b64ToBytes(b64Key), "AES-GCM", false, ["encrypt"]);
  const iv = globalThis.crypto.getRandomValues(new Uint8Array(12));
  const ct = await subtle.encrypt({ name: "AES-GCM", iv }, key, new TextEncoder().encode(JSON.stringify(data)));
  const out = new Uint8Array(12 + ct.byteLength);
  out.set(iv);
  out.set(new Uint8Array(ct), 12);
  return bytesToB64(out);
}
async function decrypt(b64ct, b64Key) {
  const key = await subtle.importKey("raw", b64ToBytes(b64Key), "AES-GCM", false, ["decrypt"]);
  const all = b64ToBytes(b64ct);
  const pt = await subtle.decrypt({ name: "AES-GCM", iv: all.slice(0, 12) }, key, all.slice(12));
  return JSON.parse(new TextDecoder().decode(pt));
}
async function skFor(b64Key) {
  return new Uint8Array(await subtle.digest("SHA-256", b64ToBytes(b64Key)));
}

let fail = 0;
const ok = (b, name) => { console.log(`${b ? "ok  " : "FAIL"} ${name}`); if (!b) fail++; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function runCpp(args) {
  return new Promise((resolve) => {
    let out = "";
    const p = spawn(BIN, args, { env: LD });
    p.stdout.on("data", (d) => (out += d));
    p.stderr.on("data", () => {});
    p.on("close", (code) => resolve({ code, out }));
  });
}
// spawn C++ that prints READY then RECV lines; resolve when it exits
function runCppStreaming(args, onLine) {
  return new Promise((resolve) => {
    let out = "", buf = "";
    const p = spawn(BIN, args, { env: LD });
    p.stdout.on("data", (d) => {
      out += d;
      buf += d;
      let i;
      while ((i = buf.indexOf("\n")) >= 0) {
        onLine(buf.slice(0, i));
        buf = buf.slice(i + 1);
      }
    });
    p.on("close", (code) => resolve({ code, out }));
    resolve.proc = p;
  });
}

const { url, wss } = await startRelay(0);
console.log("relay:", url);

// ── A) C++ self round-trip ──────────────────────────────────────────────────
{
  const { code, out } = await runCpp(["selfrt", url, KEY]);
  ok(code === 0 && out.includes("SELFRT OK"), "A: C++ self round-trip over relay");
}

// ── B) node (browser stack) publish -> C++ receive ──────────────────────────
{
  let ready = false, got = null;
  const done = runCppStreaming(["sub", url, KEY, "1"], (line) => {
    if (line.startsWith("READY")) ready = true;
    if (line.startsWith("RECV ")) got = line.slice(5);
  });
  for (let t = 0; t < 4000 && !ready; t += 50) await sleep(50);
  await sleep(200);
  // publish exactly as nostr.ts publishEphemeral does
  const sk = await skFor(KEY);
  const payload = { type: "presence", from: "browser", name: "WebDuke", ts: 1712345678000 };
  const content = await encrypt(payload, KEY);
  const ev = finalizeEvent({ kind: KIND, created_at: Math.floor(Date.now() / 1000), tags: [], content }, sk);
  const ws = new WebSocket(url);
  await new Promise((r) => (ws.onopen = r));
  ws.send(JSON.stringify(["EVENT", ev]));
  const { code } = await done;
  ws.close();
  ok(code === 0 && got && JSON.parse(got).from === "browser", "B: browser(nostr-tools) publish -> C++ receive+decrypt");
}

// ── C) C++ publish -> node (browser stack) receive ──────────────────────────
{
  const sk = await skFor(KEY);
  const pub = getPublicKey(sk);
  const ws = new WebSocket(url);
  let received = null;
  ws.onmessage = async (e) => {
    const m = JSON.parse(e.data.toString());
    if (m[0] === "EVENT" && m[1] === "cs") received = await decrypt(m[2].content, KEY);
  };
  await new Promise((r) => (ws.onopen = r));
  ws.send(JSON.stringify(["REQ", "cs", { kinds: [KIND], authors: [pub], since: Math.floor(Date.now() / 1000) }]));
  await sleep(300);
  await runCpp(["pub", url, KEY, '{"type":"offer","from":"native","to":"browser","sdp":"v=0\\r\\n","ts":42}']);
  for (let t = 0; t < 3000 && !received; t += 50) await sleep(50);
  ws.close();
  ok(received && received.type === "offer" && received.from === "native", "C: C++ publish -> browser(WebCrypto) receive+decrypt");
}

wss.close();
console.log(fail ? `\nRELAY E2E FAIL (${fail})` : "\nRELAY E2E OK");
process.exit(fail ? 1 : 0);
