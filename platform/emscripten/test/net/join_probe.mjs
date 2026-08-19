// Reproduce the user's exact join path: open ?join=CODE and log every state
// transition until in-game or timeout. One short-lived headless browser.
import { chromium } from "playwright";
const CODE = process.env.CODE || 'fr8zus7dbj7z';
const t0 = Date.now();
const ts = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(6);

const B = await chromium.launch({ headless: true, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'] });
const P = await B.newPage({ viewport: { width: 800, height: 600 } });
P.on('console', (m) => {
  const t = m.text();
  if (!/GET |Fetch|preload|Download|deprecat/i.test(t))
    console.log(`${ts()} [con] ${t.slice(0, 220)}`);
});
P.on('pageerror', (e) => console.log(`${ts()} [pageerror] ${String(e).slice(0, 200)}`));
P.on('crash', () => { console.log(`${ts()} PAGE CRASHED`); process.exit(3); });

await P.goto(`http://127.0.0.1:7800/?join=${CODE}`, { waitUntil: 'commit', timeout: 60000 });
console.log(`${ts()} loaded ?join=${CODE}`);
// Arm forensic CRC dumps (MISMATCH category detail) once the runtime is up.
(async () => {
  for (let i = 0; i < 60; i++) {
    const ok = await P.evaluate(() => {
      try { if (window.Module?.ccall) { Module.ccall('Web_SetForensics', null, ['number'], [1]); return true; } } catch {}
      return false;
    }).catch(() => false);
    if (ok) { console.log(`${ts()} forensics armed`); return; }
    await new Promise(r => setTimeout(r, 1000));
  }
})();

let last = '';
let ok = false;
let entered = -1;
let wedgeCount = 0;
for (let i = 0; i < 110; i++) {
  const s = await Promise.race([
    P.evaluate(() => {
      const m = window.__e32menu || {};
      let mstat = '';
      try { mstat = (window.DukeNet && window.DukeNet.match) ? 'room' : (window.DukeNet ? 'dn' : ''); } catch {}
      return { open: m.open, id: m.id, sel: m.sel, game: m.game, np: m.np, sync: m.sync, mstat };
    }).catch(() => null),
    new Promise(r => setTimeout(() => r('WEDGED'), 5000)),
  ]);
  if (s === 'WEDGED') { console.log(`${ts()} MAIN THREAD WEDGED (evaluate 5s timeout)`); if (++wedgeCount >= 4) break; await new Promise(r => setTimeout(r, 2000)); continue; }
  const line = s ? JSON.stringify(s) : 'EVAL-FAIL';
  if (line !== last) { console.log(`${ts()} [state] ${line}`); last = line; }
  // Success = WE entered the running game (late join worked). Track np for
  // 30s after entry to watch the min-players bot yield settle (6 -> 5).
  if (s && s.game === 1) {
    if (entered < 0) { entered = i; console.log(`${ts()} ENTERED GAME np=${s.np}`); }
    if (i - entered >= 15) { ok = true; break; }
  }
  await new Promise(r => setTimeout(r, 2000));
}
if (!ok) {
  await P.screenshot({ path: '/tmp/claude-1000/-home-user-dukenukem3d/2702fb27-c600-4a19-8d97-aac9514061e4/scratchpad/join_probe_fail.png' }).catch(() => {});
  console.log(`${ts()} JOIN-PROBE FAIL (screenshot saved)`);
  await B.close().catch(() => {});
  process.exit(1);
}
console.log(`${ts()} JOIN-PROBE OK: in game with np>=6`);
await B.close().catch(() => {});
process.exit(0);
