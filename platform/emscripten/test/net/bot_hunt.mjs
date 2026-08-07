// Desync hunt with CPU players: host+2 bots + guest joined PRE-LAUNCH (both
// sims from tic 0), forensics armed on both peers, ~5 min of continuous bot
// combat. Any divergence prints MISMATCH cats + RNG/stat dumps -- captured
// verbatim with [H]/[G] tags. 2 browsers; mem-gated.
import pkg from '/home/user/dukenukem3d/webduke3d/node_modules/playwright/index.js';
import { execSync } from 'child_process';
const { chromium } = pkg;
const availMB = parseInt(execSync("awk '/MemAvailable/{print int($2/1024)}' /proc/meminfo", { encoding: 'utf8' }));
if (availMB < 8192) { console.log(JSON.stringify({ fail: 'MEM-UNSAFE', availMB })); process.exit(4); }

const HEADLESS_ARGS = ['--mute-audio', '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'];
const FORENSIC = /MISMATCH|RNGDUMP|STATDUMP|RNGCNT|softsnap|Out.Of.Sync|desync|DESYNC|heal|SYNC|syncstat|seated|\[bot\]/;
const forensicLines = [];
async function mkPage(B, tag) {
  const P = await B.newPage({ viewport: { width: 1024, height: 768 } });
  P.on('console', m => {
    const t = m.text();
    if (FORENSIC.test(t)) { const line = `[${tag}] ${t.slice(0, 300)}`; forensicLines.push(line); console.log(line); }
  });
  P.on('pageerror', e => console.log(`[${tag}:PAGEERR] ` + String(e && e.message || e).slice(0, 200)));
  P.on('crash', () => console.log(`[${tag}] PAGE-CRASHED`));
  await P.goto('http://127.0.0.1:7800/' + (process.env.LOSSQ || ''), { waitUntil: 'commit', timeout: 60000 });
  let booted = false;
  for (let i = 0; i < 150 && !booted; i++) {
    booted = await P.evaluate(() => { const g = document.getElementById('gear'); return !!g && getComputedStyle(g).display !== 'none'; }).catch(() => false);
    if (!booted) await new Promise(r => setTimeout(r, 1000));
  }
  if (!booted) { console.log(`FAIL boot ${tag}`); process.exit(1); }
  await P.bringToFront().catch(() => {});
  await P.evaluate(() => { const c = document.querySelector('canvas'); if (c) { c.setAttribute('tabindex', '0'); c.focus(); } });
  return P;
}
const stOf = (P) => Promise.race([P.evaluate(() => window.__e32menu || {}).catch(() => ({})), new Promise(r => setTimeout(() => r({}), 4000))]);
const KEY = { Up: 'ArrowUp', Down: 'ArrowDown', Left: 'ArrowLeft', Right: 'ArrowRight', Return: 'Enter' };

const HB = await chromium.launch({ headless: true, args: HEADLESS_ARGS });
const H = await mkPage(HB, 'H');
const tap = async (k) => { await H.keyboard.down(KEY[k] || k).catch(() => {}); execSync('sleep 0.15'); await H.keyboard.up(KEY[k] || k).catch(() => {}); };
async function selTo(target, label) {
  for (let i = 0; i < 60; i++) { const m = await stOf(H); if (m.sel === target) { console.log(`${label}: sel=${target}`); return true; }
    await tap(m.sel >= 0 && m.sel > target ? 'Up' : 'Down'); await new Promise(r => setTimeout(r, 500)); }
  console.log(`SEL TIMEOUT ${label}: ` + JSON.stringify(await stOf(H))); return false;
}
async function enterTo(id, label) {
  for (let i = 0; i < 10; i++) { await tap('Return');
    for (let j = 0; j < 6; j++) { const m = await stOf(H); if (m.open === 1 && m.id === id) { console.log(`${label} reached`); return true; } await new Promise(r => setTimeout(r, 500)); } }
  console.log(`ENTER TIMEOUT ${label}`); return false;
}
for (let i = 0; i < 60; i++) { const m = await stOf(H); if (m.open === 1 && m.id === 0) break; await new Promise(r => setTimeout(r, 500)); }
if (!(await selTo(2, 'MAIN>Multiplayer')) || !(await enterTo(20001, 'MPROOT'))) process.exit(1);
if (!(await selTo(2, 'MP>HostPriv')) || !(await enterTo(20013, 'HOSTCFG'))) process.exit(1);  // PRIVATE: never pollute the user's lobby browser
// CPU Players defaults to 4 -- 4 world bots + 2 client-bot humans = 6 fighters.
if (!(await selTo(8, 'HOSTCFG>Start')) || !(await enterTo(20016, 'LOBBY'))) process.exit(1);
let invite = '';
for (let i = 0; i < 25 && !invite; i++) { invite = await H.evaluate(() => { try { return window.DukeNet.match.inviteCode() || ''; } catch { return ''; } }).catch(() => ''); if (!invite) await new Promise(r => setTimeout(r, 1000)); }
if (!invite) { console.log('FAIL invite'); process.exit(1); }
console.log(`invite ok (${invite.length})`);

const GB = await chromium.launch({ headless: true, args: HEADLESS_ARGS });
const G = await mkPage(GB, 'G');
await G.evaluate((c) => window.NetMenu.joinCode(c, 'HuntGuest'), invite);
let joined = false;
for (let i = 0; i < 60 && !joined; i++) { joined = (await G.evaluate(() => window.DukeNet.myConnectIndex).catch(() => -1)) > 0; if (!joined) await new Promise(r => setTimeout(r, 1000)); }
if (!joined) { console.log('FAIL prelaunch join'); process.exit(1); }
console.log('guest joined pre-launch; launching with 2 bots');
await selTo(0, 'LOBBY>Launch');
let inGame = false;
for (let a = 0; a < 8 && !inGame; a++) { await tap('Return');
  for (let j = 0; j < 4; j++) { const m = await stOf(H); if (m.game === 1) { inGame = true; break; } await new Promise(r => setTimeout(r, 500)); } }
if (!inGame) { console.log(JSON.stringify({ fail: 'launch', last: await stOf(H) })); process.exit(1); }

let formed = false;
for (let i = 0; i < 90 && !formed; i++) {
  const hm = await stOf(H), gm = await stOf(G);
  if (hm.game === 1 && gm.game === 1 && hm.np === 6 && gm.np === 6) formed = true;
  else await new Promise(r => setTimeout(r, 1000));
  if (i % 10 === 9) console.log(`forming... H np=${hm.np} game=${hm.game} | G np=${gm.np} game=${gm.game}`);
}
console.log(formed ? 'FORMED np=6 both peers' : 'WARN: np=6 not confirmed, hunting anyway');
await H.evaluate(() => window.Module.ccall('Web_SetForensics', null, ['number'], [1])).catch(() => {});
await G.evaluate(() => window.Module.ccall('Web_SetForensics', null, ['number'], [1])).catch(() => {});
if (process.env.LOCALBOT === '1') {
  // CLIENT-BOT MODE: both humans play themselves via the bot brain through the
  // FULL human pipeline (frame sampling, staging, S2M, prediction) -- wire
  // traffic indistinguishable from real players.
  await H.evaluate(() => window.Module.ccall('Web_SetLocalBot', null, ['number'], [1])).catch(() => {});
  await G.evaluate(() => window.Module.ccall('Web_SetLocalBot', null, ['number'], [1])).catch(() => {});
  console.log('LOCALBOT armed on host+guest');
}
console.log('forensics armed on both peers -- watching bot combat');

let softsnaps = 0, mismatches = 0, syncBadH = 0, syncBadG = 0;
const t0 = Date.now();
for (let t = 0; t < 300; t++) {
  await new Promise(r => setTimeout(r, 1000));
  const hm = await stOf(H), gm = await stOf(G);
  if (hm.sync && hm.sync !== 0) syncBadH++;
  if (gm.sync && gm.sync !== 0) syncBadG++;
  softsnaps = forensicLines.filter(l => /softsnap applied/.test(l)).length;
  mismatches = forensicLines.filter(l => /MISMATCH/.test(l)).length;
  if (t % 30 === 29)
    console.log(`t=${t}s H(np=${hm.np},sync=${hm.sync},sc=${hm.sc},plc=${hm.plc}) G(np=${gm.np},sync=${gm.sync},sc=${gm.sc},plc=${gm.plc}) soft=${softsnaps} mis=${mismatches}`);
  if (mismatches > 40) { console.log('mismatch burst captured -- ending early'); break; }
}
const verdict = { ranSecs: Math.round((Date.now() - t0) / 1000), softsnaps, mismatches,
  syncBadH, syncBadG, forensicTotal: forensicLines.length };
console.log('VERDICT ' + JSON.stringify(verdict));
await HB.close().catch(() => {}); await GB.close().catch(() => {});
process.exit(0);
