// Visual probe: late-join a running arena on the SERVED build, arm localbot so
// the view moves, and screenshot the world at intervals -- hunting the user's
// "some things are blacked out" report with my own eyes.
import { chromium } from "playwright";
const CODE = process.env.CODE || '51xg3am2ks5p';
const OUT = '/tmp/claude-1000/-home-user-dukenukem3d/2702fb27-c600-4a19-8d97-aac9514061e4/scratchpad';
const B = await chromium.launch({ headless: true, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'] });
const P = await B.newPage({ viewport: { width: 1024, height: 768 } });
P.on('crash', () => { console.log('CRASHED'); process.exit(3); });
await P.goto(`http://127.0.0.1:7800/?join=${CODE}`, { waitUntil: 'commit', timeout: 60000 });
(async () => {
  for (let i = 0; i < 90; i++) {
    const ok = await P.evaluate(() => { try { if (window.Module?.ccall) { Module.ccall('Web_SetLocalBot', null, ['number'], [1]); return true; } } catch {} return false; }).catch(() => false);
    if (ok) { console.log('localbot armed'); return; }
    await new Promise(r => setTimeout(r, 1000));
  }
})();
const st = () => Promise.race([P.evaluate(() => window.__e32menu || {}).catch(() => null), new Promise(r => setTimeout(() => r(null), 5000))]);
let entered = false;
for (let i = 0; i < 60 && !entered; i++) { const s = await st(); if (s && s.game === 1) entered = true; else await new Promise(r => setTimeout(r, 2000)); }
console.log('entered=' + entered);
if (!entered) { await P.screenshot({ path: `${OUT}/shotprobe_noentry.png` }).catch(() => {}); await B.close().catch(() => {}); process.exit(1); }
for (let k = 0; k < 4; k++) {
  await new Promise(r => setTimeout(r, k ? 7000 : 2500));
  await P.screenshot({ path: `${OUT}/shotprobe_${k}.png` }).catch(e => console.log('shot fail ' + e));
  const s = await st(); console.log(`shot ${k} np=${s && s.np} sync=${s && s.sync}`);
}
await B.close().catch(() => {});
process.exit(0);
