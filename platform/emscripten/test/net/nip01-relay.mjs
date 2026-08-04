// Minimal NIP-01 relay for local/LAN Duke play + headless tests. Handles what the
// transport uses: ephemeral (20000-29999, e.g. 20079 signaling) forwarded but NOT
// stored; parameterized-replaceable (30000-39999, e.g. 30078 lobby) stored+replaced
// by (kind,pubkey,d-tag); regular events stored by id. No signature verification
// (trusted local relay) - fine for LAN/test, NOT for a public relay.
import { WebSocketServer } from "ws";

const isEphemeral = (k) => k >= 20000 && k < 30000;
const isParamReplaceable = (k) => k >= 30000 && k < 40000;
const isReplaceable = (k) => (k >= 10000 && k < 20000) || k === 0 || k === 3;
const dTag = (e) => (e.tags.find((t) => t[0] === "d") || [])[1] || "";
const storeKey = (e) =>
  isParamReplaceable(e.kind) ? `${e.kind}:${e.pubkey}:${dTag(e)}`
  : isReplaceable(e.kind) ? `${e.kind}:${e.pubkey}`
  : `id:${e.id}`;

function matchFilter(e, f) {
  if (f.ids && !f.ids.includes(e.id)) return false;
  if (f.kinds && !f.kinds.includes(e.kind)) return false;
  if (f.authors && !f.authors.includes(e.pubkey)) return false;
  if (f.since && e.created_at < f.since) return false;
  if (f.until && e.created_at > f.until) return false;
  for (const key of Object.keys(f)) {
    if (key[0] !== "#") continue;
    const name = key.slice(1);
    if (!e.tags.some((t) => t[0] === name && f[key].includes(t[1]))) return false;
  }
  return true;
}

export function startRelay(port = 0, opts = {}) {
  const lossRate = opts.lossRate || 0; // fraction of EPHEMERAL (signaling) events to drop
  const stats = { dropped: 0, forwarded: 0 };
  const store = new Map();
  const subs = new Map();
  const wss = new WebSocketServer({ port });
  wss.on("connection", (ws) => {
    subs.set(ws, new Map());
    ws.on("message", (buf) => {
      let msg; try { msg = JSON.parse(buf.toString()); } catch { return; }
      const [type, ...rest] = msg;
      if (type === "EVENT") {
        const e = rest[0];
        // Model a lossy public relay: ACK the sender but silently DON'T forward a
        // fraction of ephemeral signaling (offers/answers/ICE). This is how a real
        // relay loses ephemeral events (never stored, no replay).
        if (lossRate > 0 && isEphemeral(e.kind) && Math.random() < lossRate) {
          stats.dropped++;
          if (ws.readyState === 1) ws.send(JSON.stringify(["OK", e.id, true, ""]));
          return;
        }
        if (isEphemeral(e.kind)) stats.forwarded++;
        if (!isEphemeral(e.kind)) {
          const k = storeKey(e), prev = store.get(k);
          if (!prev || prev.created_at <= e.created_at) store.set(k, e);
        }
        for (const [cws, cmap] of subs)
          for (const [subId, filters] of cmap)
            if (filters.some((f) => matchFilter(e, f)) && cws.readyState === 1)
              cws.send(JSON.stringify(["EVENT", subId, e]));
        if (ws.readyState === 1) ws.send(JSON.stringify(["OK", e.id, true, ""]));
      } else if (type === "REQ") {
        const subId = rest[0], filters = rest.slice(1);
        subs.get(ws).set(subId, filters);
        for (const e of store.values())
          if (filters.some((f) => matchFilter(e, f))) ws.send(JSON.stringify(["EVENT", subId, e]));
        ws.send(JSON.stringify(["EOSE", subId]));
      } else if (type === "CLOSE") {
        subs.get(ws)?.delete(rest[0]);
      }
    });
    ws.on("close", () => subs.delete(ws));
  });
  return new Promise((resolve) => wss.on("listening", () =>
    resolve({ wss, stats, port: wss.address().port, url: `ws://127.0.0.1:${wss.address().port}` })));
}

// Runnable standalone: `node nip01-relay.mjs [port]` for a local/LAN Duke relay.
if (import.meta.url === `file://${process.argv[1]}`) {
  const port = Number(process.argv[2]) || 7777;
  startRelay(port).then(({ url }) => console.log("Duke local relay listening at", url));
}
