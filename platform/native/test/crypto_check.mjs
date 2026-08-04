// Stage-1 crypto INTEROP check: proves the C++ nn_crypto primitives are
// byte-compatible with the browser transport (nostr.ts + nostr-tools).
//
//   * pubkey derivation:  C++ derive(key)  ==  nostr-tools getPublicKey(sha256(rawKey))
//   * AES-256-GCM node->C++: WebCrypto encrypt (nostr.ts layout) -> C++ decrypt
//   * AES-256-GCM C++->node: C++ encrypt -> WebCrypto decrypt
//
// Run:  node platform/native/test/crypto_check.mjs /path/to/nn_crypto_test
import { execFileSync } from "node:child_process";
import { createRequire } from "node:module";
import { fileURLToPath } from "node:url";
import path from "node:path";

// Resolve nostr-tools from the browser transport's node_modules regardless of cwd
// (ESM resolves bare specifiers from the importing file's location, and these test
// files intentionally live outside platform/emscripten).
const here = path.dirname(fileURLToPath(import.meta.url));
const req = createRequire(path.join(path.resolve(here, "../../emscripten"), "package.json"));
const _nostr = await import(req.resolve("nostr-tools/pure"));
const getPublicKey = _nostr.getPublicKey ?? _nostr.default?.getPublicKey;
if (typeof getPublicKey !== "function") throw new Error("nostr-tools getPublicKey not found; keys=" + Object.keys(_nostr));

const BIN = process.argv[2] || "/tmp/nn_crypto_test";
const LD = { ...process.env, LD_LIBRARY_PATH: "/tmp/localdev/usr/lib64" };
const subtle = globalThis.crypto.subtle;

function cpp(...args) {
  return execFileSync(BIN, args, { env: LD }).toString().trim();
}

// nostr.ts crypto, replicated verbatim (raw byte layer; the JSON wrapper is the
// Nostr LAYER, tested separately in the event stage).
function b64ToBytes(b64) { return Uint8Array.from(atob(b64), (c) => c.charCodeAt(0)); }
function bytesToB64(u8) { return btoa(String.fromCharCode(...u8)); }
async function importKey(b64) {
  return subtle.importKey("raw", b64ToBytes(b64), "AES-GCM", false, ["encrypt", "decrypt"]);
}
async function encryptRaw(str, b64Key) {
  const key = await importKey(b64Key);
  const iv = globalThis.crypto.getRandomValues(new Uint8Array(12));
  const ct = await subtle.encrypt({ name: "AES-GCM", iv }, key, new TextEncoder().encode(str));
  const out = new Uint8Array(12 + ct.byteLength);
  out.set(iv);
  out.set(new Uint8Array(ct), 12);
  return bytesToB64(out);
}
async function decryptRaw(b64ct, b64Key) {
  const key = await importKey(b64Key);
  const all = b64ToBytes(b64ct);
  const pt = await subtle.decrypt({ name: "AES-GCM", iv: all.slice(0, 12) }, key, all.slice(12));
  return new TextDecoder().decode(pt);
}

let fail = 0;
const ok = (b, name) => {
  console.log(`${b ? "ok  " : "FAIL"} ${name}`);
  if (!b) fail++;
};

const KEY = bytesToB64(globalThis.crypto.getRandomValues(new Uint8Array(32)));
const rawKey = b64ToBytes(KEY);

// 1) pubkey derivation matches nostr-tools
const skBytes = new Uint8Array(await subtle.digest("SHA-256", rawKey));
const expectedPub = getPublicKey(skBytes);
const cppPub = cpp("derive", KEY);
ok(cppPub === expectedPub, `pubkey derive matches nostr-tools (${cppPub.slice(0, 16)}...)`);

// 2) node (WebCrypto) -> C++
const SDP = "v=0\r\no=- 42 2 IN IP4 0.0.0.0\r\ns=-\r\na=ice-ufrag:x9Zq\r\n\"quoted\"\\slash";
const nodeBlob = await encryptRaw(SDP, KEY);
const cppDecrypted = cpp("decrypt", KEY, nodeBlob);
ok(cppDecrypted === SDP, "WebCrypto encrypt -> C++ decrypt (SDP-like)");

// 3) C++ -> node (WebCrypto)
const cppBlob = cpp("encrypt", KEY, SDP);
const nodeDecrypted = await decryptRaw(cppBlob, KEY);
ok(nodeDecrypted === SDP, "C++ encrypt -> WebCrypto decrypt (SDP-like)");

// 4) a few more keys/messages for robustness
for (let i = 0; i < 5; i++) {
  const k = bytesToB64(globalThis.crypto.getRandomValues(new Uint8Array(32)));
  const raw = b64ToBytes(k);
  const sk = new Uint8Array(await subtle.digest("SHA-256", raw));
  ok(cpp("derive", k) === getPublicKey(sk), `derive #${i} matches`);
  const msg = `msg-${i}-${Math.random().toString(36)}`;
  ok((await decryptRaw(cpp("encrypt", k, msg), k)) === msg, `C++->node roundtrip #${i}`);
  ok(cpp("decrypt", k, await encryptRaw(msg, k)) === msg, `node->C++ roundtrip #${i}`);
}

console.log(fail ? `\nCRYPTO INTEROP FAIL (${fail})` : "\nCRYPTO INTEROP OK");
process.exit(fail ? 1 : 0);
