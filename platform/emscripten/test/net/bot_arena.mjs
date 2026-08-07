// PERSISTENT bot arena: one headless host + 4 bots, private match, stays up
// 30 minutes so the user can join and fight/watch anytime. Prints the JOIN
// URL early and a liveness line each minute. One browser; mem-gated.
import pkg from '/home/user/dukenukem3d/webduke3d/node_modules/playwright/index.js';
import { execSync } from 'child_process';
const { chromium } = pkg;
const availMB = parseInt(execSync("awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo", { encoding: 'utf8' }));
if (availMB < 6144) { console.log(JSON.stringify({ fail: 'MEM-UNSAFE', availMB })); process.exit(4); }

const B = await chromium.launch({ headless: true, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'] });
const H = await B.newPage({ viewport: { width: 1024, height: 768 } });
H.on('crash', () => { console.log('ARENA HOST CRASHED'); process.exit(3); });
await H.goto('http://127.0.0.1:7800/', { waitUntil: 'commit', timeout: 60000 });
let booted = false;
for (let i = 0; i < 150 && !booted; i++) {
  booted = await H.evaluate(() => { const g = document.getElementById('gear'); return !!g && getComputedStyle(g).display !== 'none'; }).catch(() => false);
  if (!booted) await new Promise(r => setTimeout(r, 1000));
}
if (!booted) { console.log('FAIL boot'); process.exit(1); }
await H.bringToFront().catch(() => {});
await H.evaluate(() => { const c = document.querySelector('canvas'); if (c) { c.setAttribute('tabindex', '0'); c.focus(); } });
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
let invite = '';
for (let i = 0; i < 25 && !invite; i++) { invite = await H.evaluate(() => { try { return window.DukeNet.match.inviteCode() || ''; } catch { return ''; } }).catch(() => ''); if (!invite) await new Promise(r => setTimeout(r, 1000)); }
if (!invite) { console.log('FAIL invite'); process.exit(1); }
console.log(`ARENA JOIN URL: http://127.0.0.1:7800/?join=${encodeURIComponent(invite)}`);
// WAIT IN THE LOBBY for a human, then launch: the pre-game join is the proven
// path (late join into a running match is still being fixed). Bots fill the
// remaining seats at launch via the min-players floor.
console.log('ARENA WAITING IN LOBBY: join and the match auto-starts (bots fill in)');
let joined = false;
for (let w = 0; w < 750 && !joined; w++) {
  const m = await st();
  if ((m.np | 0) >= 2) { joined = true; break; }
  await new Promise(r => setTimeout(r, 2000));
}
if (!joined) { console.log('ARENA EXPIRED: nobody joined the lobby'); await B.close().catch(() => {}); process.exit(0); }
console.log('PLAYER IN LOBBY -> launching');
await selTo(0, 'LOBBY>Launch');
let inGame = false;
for (let a = 0; a < 8 && !inGame; a++) { await tap('Return');
  for (let j = 0; j < 4; j++) { const m = await st(); if (m.game === 1) { inGame = true; break; } await new Promise(r => setTimeout(r, 500)); } }
if (!inGame) { console.log('FAIL launch'); process.exit(1); }
console.log('ARENA LIVE: humans + CPU fill, running for 30 minutes');
for (let min = 0; min < 30; min++) {
  await new Promise(r => setTimeout(r, 60000));
  const m = await st();
  console.log(`arena t=${min + 1}m np=${m.np} game=${m.game} sync=${m.sync}`);
  if (!m.np) { console.log('ARENA WEDGED/DEAD'); break; }
}
console.log('ARENA CLOSING (30 min up)');
await B.close().catch(() => {});
process.exit(0);
