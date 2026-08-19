// Localbot guest soak: join a running arena by code, drive the local seat with
// the in-engine bot (full client input pipeline incl. closed-loop aim staging),
// forensics armed, and watch sync health for ~27 minutes. This is the v2
// localbot hunt rerun on the served hygiene build. Expected profile from the
// accepted baseline: ONE heal shortly post-seat (fresh joiner -> targeted
// heal), then silence: no further desync reports, no softsnaps, sync=0.
import { chromium } from "playwright";
const CODE = process.env.CODE || '';
if (!CODE) { console.log('NO CODE'); process.exit(2); }
const MINUTES = parseInt(process.env.MINUTES || '27', 10);
const t0 = Date.now();
const ts = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(7);
const n = { mismatch: 0, desyncReport: 0, heal: 0, softsnap: 0, joinApplied: 0, frag: 0, wedged: 0 };

const B = await chromium.launch({ headless: true, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'] });
const P = await B.newPage({ viewport: { width: 800, height: 600 } });
P.on('console', (m) => {
  const t = m.text();
  if (/MISMATCH/.test(t)) n.mismatch++;
  if (/desync report from peer|guest desync/.test(t)) n.desyncReport++;
  if (/healFlow start/.test(t)) n.heal++;
  if (/softsnap applied/.test(t)) n.softsnap++;
  if (/joinApplied/.test(t)) n.joinApplied++;
  if (/\[frag\]/.test(t)) n.frag++;
  if (/MISMATCH|RNGDUMP|desync|softsnap|healFlow|joinApplied|\[frag\]|LOAD REFUSED|\[spin\]|GARBAGE|WEDG/.test(t))
    console.log(`${ts()} [con] ${t.slice(0, 200)}`);
});
P.on('pageerror', (e) => console.log(`${ts()} [pageerror] ${String(e).slice(0, 160)}`));
P.on('crash', () => { console.log(`${ts()} PAGE CRASHED`); process.exit(3); });

await P.goto(`http://127.0.0.1:7800/?join=${CODE}`, { waitUntil: 'commit', timeout: 60000 });
console.log(`${ts()} loaded ?join=${CODE}`);
(async () => {
  for (let i = 0; i < 90; i++) {
    const ok = await P.evaluate(() => {
      try { if (window.Module?.ccall) { Module.ccall('Web_SetForensics', null, ['number'], [1]);
        Module.ccall('Web_SetLocalBot', null, ['number'], [1]); return true; } } catch {}
      return false;
    }).catch(() => false);
    if (ok) { console.log(`${ts()} forensics+localbot armed`); return; }
    await new Promise(r => setTimeout(r, 1000));
  }
})();

const st = () => Promise.race([
  P.evaluate(() => { const m = window.__e32menu || {}; return { game: m.game, np: m.np, sync: m.sync }; }).catch(() => null),
  new Promise(r => setTimeout(() => r('WEDGED'), 5000)),
]);

let entered = false;
for (let i = 0; i < 120 && !entered; i++) {
  const s = await st();
  if (s === 'WEDGED') { n.wedged++; console.log(`${ts()} WEDGED pre-entry`); if (n.wedged >= 5) break; }
  else if (s && s.game === 1) { entered = true; console.log(`${ts()} ENTERED GAME np=${s.np}`); break; }
  await new Promise(r => setTimeout(r, 2000));
}
if (!entered) { console.log(`${ts()} SOAK-GUEST FAIL: never entered game`); await B.close().catch(() => {}); process.exit(1); }

let lastSync = -1, lastNp = -1;
for (let min = 0; min < MINUTES; min++) {
  await new Promise(r => setTimeout(r, 60000));
  const s = await st();
  if (s === 'WEDGED') { n.wedged++; console.log(`${ts()} guest t=${min + 1}m WEDGED`); continue; }
  lastSync = s ? s.sync : -1; lastNp = s ? s.np : -1;
  console.log(`${ts()} guest t=${min + 1}m np=${lastNp} sync=${lastSync} mm=${n.mismatch} dr=${n.desyncReport} heal=${n.heal} snap=${n.softsnap}`);
  if (!s || !s.np) { console.log(`${ts()} GUEST DEAD/EMPTY`); break; }
}
console.log(`SOAK-GUEST VERDICT joined=1 np=${lastNp} sync=${lastSync} mismatches=${n.mismatch} desyncReports=${n.desyncReport} heals=${n.heal} softsnaps=${n.softsnap} frags=${n.frag} wedged=${n.wedged}`);
await B.close().catch(() => {});
process.exit(0);
