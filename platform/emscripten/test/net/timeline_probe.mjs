// Timeline probe: host a fresh private bot match and screenshot the SAME
// session at t=2/30/60/120/180s after launch, counting [impact] wall-vs-sprite
// hits and [frag]s between frames. Decides whether "blacked out" surfaces are
// progressive shot-out state (bot wall-spray breaking lights/screens) or
// present from the first frame (a real render/data bug).
import { chromium } from "playwright";
import { execSync } from 'child_process';
const OUT = '/tmp/claude-1000/-home-user-dukenukem3d/2702fb27-c600-4a19-8d97-aac9514061e4/scratchpad';
const availMB = parseInt(execSync("awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo", { encoding: 'utf8' }));
if (availMB < 6144) { console.log(JSON.stringify({ fail: 'MEM-UNSAFE', availMB })); process.exit(4); }

const n = { wallImp: 0, sprImp: 0, frag: 0, shot: 0 };
const B = await chromium.launch({ headless: true, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'] });
const H = await B.newPage({ viewport: { width: 1024, height: 768 } });
H.on('crash', () => { console.log('HOST CRASHED'); process.exit(3); });
H.on('console', (m) => {
  const t = m.text();
  const im = t.match(/\[impact\] shooter=\d+ wall=(-?\d+) spr=(-?\d+)/);
  if (im) { if (parseInt(im[2]) >= 0) n.sprImp++; else n.wallImp++; }
  if (/\[frag\]/.test(t)) n.frag++;
  if (/\[shot\]/.test(t)) n.shot++;
  if (/\[botsep\]|\[frag\]|\[bot1\]/.test(t)) console.log(`[fwd] ${t.slice(0, 170)}`);
});
await H.goto('http://127.0.0.1:7800/', { waitUntil: 'commit', timeout: 60000 });
let booted = false;
for (let i = 0; i < 150 && !booted; i++) {
  booted = await H.evaluate(() => { const g = document.getElementById('gear'); return !!g && getComputedStyle(g).display !== 'none'; }).catch(() => false);
  if (!booted) await new Promise(r => setTimeout(r, 1000));
}
if (!booted) { console.log('FAIL boot'); process.exit(1); }
await H.bringToFront().catch(() => {});
await H.evaluate(() => { const c = document.querySelector('canvas'); if (c) { c.setAttribute('tabindex', '0'); c.focus(); } });
await H.evaluate(() => { try { Module.ccall('Web_SetForensics', null, ['number'], [1]); } catch (e) {} }).catch(() => {});
if (process.env.LOCALBOT === '1')
  await H.evaluate(() => { try { Module.ccall('Web_SetLocalBot', null, ['number'], [1]); } catch (e) {} }).catch(() => {});
const st = () => Promise.race([H.evaluate(() => window.__e32menu || {}).catch(() => ({})), new Promise(r => setTimeout(() => r({}), 4000))]);
const KEY = { Up: 'ArrowUp', Down: 'ArrowDown', Left: 'ArrowLeft', Right: 'ArrowRight', Return: 'Enter' };
const tap = async (k) => { await H.keyboard.down(KEY[k] || k).catch(() => {}); execSync('sleep 0.15'); await H.keyboard.up(KEY[k] || k).catch(() => {}); };
async function selTo(target, label) {
  for (let i = 0; i < 60; i++) { const m = await st(); if (m.sel === target) return true;
    await tap(m.sel >= 0 && m.sel > target ? 'Up' : 'Down'); await new Promise(r => setTimeout(r, 500)); }
  console.log(`SEL TIMEOUT ${label}`); return false;
}
async function enterTo(id, label) {
  for (let i = 0; i < 10; i++) { await tap('Return');
    for (let j = 0; j < 6; j++) { const m = await st(); if (m.open === 1 && m.id === id) return true; await new Promise(r => setTimeout(r, 500)); } }
  console.log(`ENTER TIMEOUT ${label}`); return false;
}
for (let i = 0; i < 60; i++) { const m = await st(); if (m.open === 1 && m.id === 0) break; await new Promise(r => setTimeout(r, 500)); }
if (!(await selTo(2, 'MAIN>Multiplayer')) || !(await enterTo(20001, 'MPROOT'))) process.exit(1);
if (!(await selTo(2, 'MP>HostPriv')) || !(await enterTo(20013, 'HOSTCFG'))) process.exit(1);
if (!(await selTo(8, 'HOSTCFG>Start')) || !(await enterTo(20016, 'LOBBY'))) process.exit(1);
await selTo(0, 'LOBBY>Launch');
let inGame = false;
for (let a = 0; a < 8 && !inGame; a++) { await tap('Return');
  for (let j = 0; j < 4; j++) { const m = await st(); if (m.game === 1) { inGame = true; break; } await new Promise(r => setTimeout(r, 500)); } }
if (!inGame) { console.log('FAIL launch'); process.exit(1); }
console.log('LIVE: timeline starting');
const marks = (process.env.MARKS || '2,30,60,120,180').split(',').map(Number);
let prev = 0;
for (let k = 0; k < marks.length; k++) {
  await new Promise(r => setTimeout(r, (marks[k] - prev) * 1000)); prev = marks[k];
  await H.screenshot({ path: `${OUT}/tl_${marks[k]}s.png` }).catch(e => console.log('shot fail ' + e));
  const m = await st();
  console.log(`t=${marks[k]}s np=${m.np} sync=${m.sync} shots8=${n.shot} wallImp=${n.wallImp} sprImp=${n.sprImp} frags=${n.frag}`);
}
console.log(`TIMELINE VERDICT wallImp=${n.wallImp} sprImp=${n.sprImp} ratio=${(n.wallImp / Math.max(1, n.wallImp + n.sprImp)).toFixed(2)} frags=${n.frag}`);
await B.close().catch(() => {});
process.exit(0);
