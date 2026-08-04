// Stage-1 Nostr EVENT interop check: a C++-built event must be accepted by
// nostr-tools verifyEvent (proves id + serialization + schnorr sig all match),
// its recomputed id must equal the C++ id, and its encrypted content must
// decrypt back to the plaintext with WebCrypto.
//
// Run:  node platform/native/test/nostr_check.mjs /path/to/nn_nostr_test
import { execFileSync } from "node:child_process";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";

const here = path.dirname(fileURLToPath(import.meta.url));
const req = createRequire(path.join(path.resolve(here, "../../emscripten"), "package.json"));
const pure = await import(req.resolve("nostr-tools/pure"));
const verifyEvent = pure.verifyEvent ?? pure.default?.verifyEvent;
const getEventHash = pure.getEventHash ?? pure.default?.getEventHash;
if (typeof verifyEvent !== "function" || typeof getEventHash !== "function")
  throw new Error("nostr-tools verifyEvent/getEventHash not found");

const BIN = process.argv[2] || "/tmp/nn_nostr_test";
const LD = { ...process.env, LD_LIBRARY_PATH: "/tmp/localdev/usr/lib64" };
const subtle = globalThis.crypto.subtle;
const cpp = (...a) => execFileSync(BIN, a, { env: LD }).toString().trim();

function b64ToBytes(b64) { return Uint8Array.from(atob(b64), (c) => c.charCodeAt(0)); }
function bytesToB64(u8) { return btoa(String.fromCharCode(...u8)); }
async function decrypt(b64ct, b64Key) {
  const key = await subtle.importKey("raw", b64ToBytes(b64Key), "AES-GCM", false, ["decrypt"]);
  const all = b64ToBytes(b64ct);
  const pt = await subtle.decrypt({ name: "AES-GCM", iv: all.slice(0, 12) }, key, all.slice(12));
  return new TextDecoder().decode(pt);
}

let fail = 0;
const ok = (b, name) => { console.log(`${b ? "ok  " : "FAIL"} ${name}`); if (!b) fail++; };

// SIGNALING_KIND=20079 (ephemeral, empty tags) and LOBBY_KIND=30078 (replaceable, d-tag)
const cases = [
  { kind: 20079, payload: JSON.stringify({ type: "offer", from: "a", to: "b", sdp: "v=0\r\no=- 1 2 IN IP4 0.0.0.0\r\n", ts: 1712345678000 }) },
  { kind: 20079, payload: JSON.stringify({ type: "presence", from: "node-xyz", name: "Duke", ts: 1712345679000 }) },
  { kind: 30078, dtag: "match-abc123", payload: JSON.stringify({ v: 1, matchId: "match-abc123", name: "Duke Match", players: 1 }) },
];

for (const c of cases) {
  const KEY = bytesToB64(globalThis.crypto.getRandomValues(new Uint8Array(32)));
  const out = cpp("buildevent", KEY, String(c.kind), c.payload, c.dtag ?? "", "1712345678");
  let ev;
  try { ev = JSON.parse(out); } catch { ok(false, `kind ${c.kind}: C++ emitted valid JSON`); continue; }
  ok(verifyEvent(ev) === true, `kind ${c.kind}: nostr-tools verifyEvent accepts C++ event`);
  ok(getEventHash(ev) === ev.id, `kind ${c.kind}: nostr-tools id == C++ id`);
  ok(ev.kind === c.kind, `kind ${c.kind}: kind field`);
  const expectTags = c.dtag ? JSON.stringify([["d", c.dtag]]) : "[]";
  ok(JSON.stringify(ev.tags) === expectTags, `kind ${c.kind}: tags`);
  ok((await decrypt(ev.content, KEY)) === c.payload, `kind ${c.kind}: content decrypts to payload`);
}

console.log(fail ? `\nNOSTR EVENT INTEROP FAIL (${fail})` : "\nNOSTR EVENT INTEROP OK");
process.exit(fail ? 1 : 0);
