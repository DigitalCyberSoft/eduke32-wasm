// Fetch a NATIVE (dTag "m-") and a BROWSER (dTag "ms") lobby event and run the
// browser's EXACT decrypt+parse (nostr.ts) on each. If the native one fails to
// decrypt or parse, that is why the browser silently drops it (decrypt -> null).
import WebSocket from "ws";
import { getPublicKey } from "nostr-tools/pure";
import { createHash, webcrypto } from "node:crypto";
const crypto = webcrypto;

const KEY = "MqBAIxP3Lwawq+18BL1KSjAdlTxfVtoERfmgszaEKnc=";
const sk = new Uint8Array(createHash("sha256").update(Buffer.from(KEY, "base64")).digest());
const pub = getPublicKey(sk);

async function decrypt(b64ct) {
  const raw = Uint8Array.from(Buffer.from(KEY, "base64"));
  const key = await crypto.subtle.importKey("raw", raw, "AES-GCM", false, ["decrypt"]);
  const all = Uint8Array.from(Buffer.from(b64ct, "base64"));
  const pt = await crypto.subtle.decrypt({ name: "AES-GCM", iv: all.slice(0, 12) }, key, all.slice(12));
  return JSON.parse(new TextDecoder().decode(pt));
}

const events = new Map(); // dTag -> content
const ws = new WebSocket("wss://offchain.pub");
ws.on("open", () => ws.send(JSON.stringify(["REQ", "s", { kinds: [30078], authors: [pub], limit: 40 }])));
ws.on("message", (d) => {
  const m = JSON.parse(d.toString());
  if (m[0] === "EVENT") { const ev = m[2], dt = (ev.tags.find((t) => t[0] === "d") || [])[1]; if (dt) events.set(dt, ev.content); }
  else if (m[0] === "EOSE") ws.close();
});
ws.on("close", async () => {
  const nativeTag = [...events.keys()].find((d) => d.startsWith("m-"));
  const browserTag = [...events.keys()].find((d) => d.startsWith("ms"));
  for (const [label, tag] of [["NATIVE", nativeTag], ["BROWSER", browserTag]]) {
    if (!tag) { console.log(`${label}: no event found`); continue; }
    try {
      const info = await decrypt(events.get(tag));
      console.log(`\n${label} (dTag=${tag}) decrypt OK. MatchInfo:`);
      console.log(JSON.stringify(info, null, 1));
      console.log(`  -> status=${info.status} (needs "open"/"playing") ts=${info.ts} (needs ms) grp.mainGrp.crc=${info.grp?.mainGrp?.crc}`);
    } catch (e) { console.log(`\n${label} (dTag=${tag}) DECRYPT/PARSE FAILED: ${e.message}  <-- browser would drop this`); }
  }
  process.exit(0);
});
setTimeout(() => process.exit(0), 9000);
