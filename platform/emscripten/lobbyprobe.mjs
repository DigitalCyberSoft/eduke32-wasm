// Replicate the browser's public-lobby discovery: derive the lobby author from
// PUBLIC_LOBBY_KEY, query relays for kind-30078 events under it, print d-tags.
// If the native host's matchId shows up, the native publishes correctly.
import WebSocket from "ws";
import { getPublicKey } from "nostr-tools/pure";
import { createHash } from "node:crypto";

const KEY = "MqBAIxP3Lwawq+18BL1KSjAdlTxfVtoERfmgszaEKnc="; // PUBLIC_LOBBY_KEY
const sk = new Uint8Array(createHash("sha256").update(Buffer.from(KEY, "base64")).digest());
const pub = getPublicKey(sk);
console.log("lobby author pubkey:", pub);

const relays = [
  "wss://relay.primal.net", "wss://offchain.pub", "wss://purplepag.es",
  "wss://nostr-pub.wellorder.net", "wss://nos.lol", "wss://nostr.mom",
];
let total = 0, anyKind = 0;
for (const url of relays) {
  try {
    const ws = new WebSocket(url);
    ws.on("open", () => {
      ws.send(JSON.stringify(["REQ", "byauthor", { kinds: [30078], authors: [pub] }]));
      ws.send(JSON.stringify(["REQ", "anykind", { kinds: [30078], limit: 5 }])); // any 30078 at all?
    });
    ws.on("message", (d) => {
      let m; try { m = JSON.parse(d.toString()); } catch { return; }
      if (m[0] === "EVENT") {
        const ev = m[2], dt = (ev.tags.find((t) => t[0] === "d") || [])[1];
        if (m[1] === "byauthor") { total++; console.log(`[${url}] LOBBY-AUTHOR EVENT dTag=${dt} created_at=${ev.created_at} clen=${ev.content.length} pub=${ev.pubkey.slice(0,12)}`); }
        else { anyKind++; console.log(`[${url}] any-30078 dTag=${dt} pub=${ev.pubkey.slice(0,12)} (author ${ev.pubkey===pub?"==LOBBY":"!=lobby"})`); }
      } else if (m[0] === "EOSE") { /* keep open a bit */ }
        else if (m[0] === "NOTICE" || m[0] === "OK") console.log(`[${url}] ${m[0]}: ${JSON.stringify(m.slice(1))}`);
    });
    ws.on("error", (e) => console.log(`[${url}] ERR ${e.message}`));
  } catch (e) { console.log(`[${url}] THROW ${e.message}`); }
}
setTimeout(() => { console.log(`\n== lobby-author kind-30078 events: ${total} ; any-30078 seen: ${anyKind} =='`); process.exit(0); }, 9000);
