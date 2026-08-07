// CPU-player smoke: ONE headless browser hosts with 2 bots. Asserts the bots
// seat (np=3), the match runs (sc grows), bots actually move (p1 changes),
// and no sync flag appears. Machine-light: single chromium, ~4 min, auto-close.
import pkg from '/home/user/dukenukem3d/webduke3d/node_modules/playwright/index.js';
import { execSync } from 'child_process';
const { chromium } = pkg;
const availMB = parseInt(execSync("awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo", { encoding: 'utf8' }));
if (availMB < 6144) { console.log(JSON.stringify({ fail: 'MEM-UNSAFE', availMB })); process.exit(4); }

const B = await chromium.launch({ headless: true, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'] });
const H = await B.newPage({ viewport: { width: 1024, height: 768 } });
const consoleHits = { seated: 0, oos: 0, softsnap: 0 };
const ring = [];
H.on('console', m => {
  const t = m.text();
  ring.push(t.slice(0, 160)); if (ring.length > 120) ring.shift();
  if (/\[bot\]|seated/.test(t)) { console.log('console: ' + t.slice(0, 140)); }
  if (/seated \d+ CPU/.test(t)) consoleHits.seated++;
  if (/Out.Of.Sync/i.test(t)) consoleHits.oos++;
  if (/softsnap/.test(t)) consoleHits.softsnap++;
});
H.on('pageerror', e => console.log('PAGEERR: ' + String(e && e.message || e).slice(0, 300)));
H.on('crash', () => console.log('PAGE-CRASHED'));
const dumpRing = (label) => { console.log(`--- console ring (${label}) ---`); for (const l of ring) console.log('  ' + l); };
await H.goto('http://127.0.0.1:7800/', { waitUntil: 'commit', timeout: 60000 });
let booted = false;
for (let i = 0; i < 150 && !booted; i++) {
  booted = await H.evaluate(() => { const g = document.getElementById('gear'); return !!g && getComputedStyle(g).display !== 'none'; }).catch(() => false);
  if (!booted) await new Promise(r => setTimeout(r, 1000));
}
if (!booted) { console.log(JSON.stringify({ fail: 'boot' })); process.exit(1); }
await H.bringToFront().catch(() => {});
await H.evaluate(() => { const c = document.querySelector('canvas'); if (c) { c.setAttribute('tabindex', '0'); c.focus(); } });

const st = () => Promise.race([H.evaluate(() => window.__e32menu || {}).catch(() => ({})), new Promise(r => setTimeout(() => r({}), 4000))]);
const KEY = { Up: 'ArrowUp', Down: 'ArrowDown', Left: 'ArrowLeft', Right: 'ArrowRight', Return: 'Enter' };
const tap = async (k) => { await H.keyboard.down(KEY[k] || k).catch(() => {}); execSync('sleep 0.15'); await H.keyboard.up(KEY[k] || k).catch(() => {}); };
async function selTo(target, label) {
  for (let i = 0; i < 60; i++) { const m = await st(); if (m.sel === target) { console.log(`${label}: sel=${target}`); return true; }
    await tap(m.sel >= 0 && m.sel > target ? 'Up' : 'Down'); await new Promise(r => setTimeout(r, 500)); }
  console.log(`SEL TIMEOUT ${label}: ` + JSON.stringify(await st())); return false;
}
async function enterTo(id, label) {
  for (let i = 0; i < 10; i++) { await tap('Return');
    for (let j = 0; j < 6; j++) { const m = await st(); if (m.open === 1 && m.id === id) { console.log(`${label} reached`); return true; } await new Promise(r => setTimeout(r, 500)); } }
  console.log(`ENTER TIMEOUT ${label}: ` + JSON.stringify(await st())); return false;
}
for (let i = 0; i < 60; i++) { const m = await st(); if (m.open === 1 && m.id === 0) break; await new Promise(r => setTimeout(r, 500)); }
if (!(await selTo(2, 'MAIN>Multiplayer')) || !(await enterTo(20001, 'MPROOT'))) process.exit(1);
if (!(await selTo(2, 'MP>HostPriv')) || !(await enterTo(20013, 'HOSTCFG'))) process.exit(1);  // PRIVATE: never pollute the user's lobby browser
// CPU Players defaults to 4 now -- no adjustment needed; np assert = 5.
if (!(await selTo(8, 'HOSTCFG>Start')) || !(await enterTo(20016, 'LOBBY'))) process.exit(1);
await selTo(0, 'LOBBY>Launch');
let inGame = false;
for (let a = 0; a < 8 && !inGame; a++) { await tap('Return');
  for (let j = 0; j < 4; j++) { const m = await st(); if (m.game === 1) { inGame = true; break; } await new Promise(r => setTimeout(r, 500)); } }
if (!inGame) {
  const alive = await Promise.race([H.evaluate(() => 1 + 1), new Promise(r => setTimeout(() => r('WEDGED'), 5000))]).catch(e => 'DEAD:' + String(e).slice(0, 80));
  console.log(JSON.stringify({ fail: 'launch', probe: alive, last: await st() }));
  dumpRing('launch failure');
  process.exit(1);
}

let seated3 = false, moved = false, lastP1 = '';
let syncBad = 0;
const bounds = { minx: Infinity, maxx: -Infinity, miny: Infinity, maxy: -Infinity };
for (let t = 0; t < 90; t++) {
  await new Promise(r => setTimeout(r, 1000));
  const m = await st();
  if (m.np === 3) seated3 = true;
  if (m.sync && m.sync !== 0) syncBad++;
  const p1 = JSON.stringify(m.p1 || '');
  if (Array.isArray(m.p1) && m.p1.length >= 2) {
    bounds.minx = Math.min(bounds.minx, m.p1[0]); bounds.maxx = Math.max(bounds.maxx, m.p1[0]);
    bounds.miny = Math.min(bounds.miny, m.p1[1]); bounds.maxy = Math.max(bounds.maxy, m.p1[1]);
  }
  if (lastP1 && p1 !== lastP1) moved = true;
  lastP1 = p1;
  if (t % 15 === 14) console.log(`t=${t} np=${m.np} game=${m.game} sync=${m.sync} p1=${p1.slice(0, 44)}`);
}
const spread = (bounds.maxx - bounds.minx) + (bounds.maxy - bounds.miny);
// Roaming bar: the old spawn-camper bots had spread 0; crossing rooms on
// E1L1 means tens of thousands of map units.
const roamed = isFinite(spread) && spread > 8192;
const verdict = { seated3, botsMoved: moved, roamed, spread: isFinite(spread) ? spread : -1,
  syncBadSamples: syncBad, oosLines: consoleHits.oos, softsnaps: consoleHits.softsnap };
console.log('VERDICT ' + JSON.stringify(verdict));
await B.close().catch(() => {});
process.exit(seated3 && moved && roamed && syncBad === 0 ? 0 : 2);
