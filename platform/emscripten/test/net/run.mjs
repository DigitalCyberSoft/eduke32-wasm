// Headless 2-peer integration harness: real transport (peer.ts/duke-net.ts) over real
// WebRTC (werift) + a local NIP-01 relay, no browser. Proves connect->join end to end.
//   node test/net/run.mjs
import { startRelay } from "./nip01-relay.mjs";
import { spawn } from "node:child_process";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const dir = dirname(fileURLToPath(import.meta.url));
const tsx = join(dir, "../../node_modules/.bin/tsx");
const peerScript = join(dir, "peer.mts");

function runPeer(args) {
  const p = spawn(tsx, [peerScript, ...args], { stdio: ["ignore", "pipe", "pipe"] });
  const events = [], listeners = [];
  let buf = "";
  p.stdout.on("data", (d) => {
    buf += d.toString();
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
      const line = buf.slice(0, i).trim(); buf = buf.slice(i + 1);
      if (!line) continue;
      let ev; try { ev = JSON.parse(line); } catch { console.log(`  [${args[0]}]`, line); continue; }
      console.log(`  [${args[0]}]`, JSON.stringify(ev));
      events.push(ev); listeners.forEach((fn) => fn(ev));
    }
  });
  p.stderr.on("data", (d) => process.stderr.write(`  [${args[0]} err] ${d}`));
  return {
    proc: p, events,
    waitFor: (pred, ms) => new Promise((res, rej) => {
      const hit = events.find(pred); if (hit) return res(hit);
      const fn = (ev) => { if (pred(ev)) { listeners.splice(listeners.indexOf(fn), 1); res(ev); } };
      listeners.push(fn);
      setTimeout(() => rej(new Error("timeout")), ms);
    }),
  };
}

async function main() {
  const lossRate = Number(process.env.LOSS) || 0;
  const { wss, url, stats } = await startRelay(0, { lossRate });
  console.log("local relay:", url, lossRate ? `(signaling loss ${lossRate * 100}%)` : "");
  let ok = false, host, guest;
  try {
    host = runPeer(["host", url]);
    const hosted = await host.waitFor((e) => e.ev === "hosted", 15000);
    console.log("host advertised; inviteCode len", (hosted.inviteCode || "").length);
    guest = runPeer(["guest", url, hosted.inviteCode]);
    const joined = await guest.waitFor((e) => e.ev === "joined", 20000);
    console.log("guest JOINED at slot", joined.slot);
    const roster2 = await host.waitFor((e) => e.ev === "roster" && e.count >= 2, 6000).catch(() => null);
    console.log("host sees roster>=2:", !!roster2);
    ok = true;
  } catch (e) {
    console.error("FAIL:", e.message);
  } finally {
    host?.proc.kill(); guest?.proc.kill(); wss.close();
  }
  console.log(`relay dropped ${stats.dropped} / forwarded ${stats.forwarded} signaling events`);
  console.log(ok ? "\nPASS: connect + join over werift + local relay" : "\nFAIL: guest did not join (stall)");
  process.exit(ok ? 0 : 1);
}
main();
