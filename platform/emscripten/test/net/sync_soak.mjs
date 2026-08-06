// Gameplay-sync soak: headed host (Xvfb :7, real menu) + headless guest.
// Mode normal:   guest joins BEFORE launch, host launches with 2 players.
// Mode latejoin: host launches SOLO, guest joins the running game (relaunch path).
// Asserts per-peer __e32menu {game,np,idx,sync} through a 60s soak with host
// movement, and measures guest frame-to-frame pixel deltas (screen "shake").
import pkg from '/home/user/dukenukem3d/webduke3d/node_modules/playwright/index.js';
import fs from 'fs';
import { execSync } from 'child_process';
const { chromium } = pkg;
const MODE = process.argv[2] || 'normal';
const LOG = `/tmp/sync_soak_${MODE}.log`;
fs.writeFileSync(LOG, `=== sync soak ${MODE} ${new Date().toISOString()} ===\n`);
const rec = (m) => fs.appendFileSync(LOG, m + '\n');
const sh = (c) => { try { return execSync(c, { encoding: 'utf8' }).trim(); } catch { return ''; } };
const keys = (...ks) => { for (const k of ks) { sh(`DISPLAY=:7 xdotool keydown --clearmodifiers ${k}`); sh(`sleep 0.15`); sh(`DISPLAY=:7 xdotool keyup ${k}`); sh(`sleep 0.7`); } };
const pids = () => { try { return new Set(execSync(`pgrep -f 'ms-playwright.*(chrome|chromium)|--headless'`, { encoding: 'utf8' }).split(/\s+/).filter(Boolean).map(Number)); } catch { return new Set(); } };
const before = pids();

const ringAll = { H: [], G: [] };
// PREDMODE env (debug bisect): bit0=correction pass, bit1=view swap. Applied
// to every page right after boot; absent = engine default (3).
const PREDMODE = process.env.PREDMODE;
async function applyPredMode(page) {
  if (PREDMODE === undefined) return;
  await page.evaluate((m) => window.Module.ccall('Web_SetPredictMode', null, ['number'], [m]), Number(PREDMODE)).catch(() => {});
}
async function boot(page, tag) {
  page.on('console', m => { const r = ringAll[tag]; if (r) { r.push(m.text().slice(0, 200)); if (r.length > 40) r.shift(); } });
  await page.addInitScript(() => {
    window.__rejections = [];
    window.addEventListener('unhandledrejection', (e) => {
      try { window.__rejections.push(String(e.reason && (e.reason.stack || e.reason.message) || e.reason).slice(0, 400)); } catch {}
    });
  });
  page.on('console', m => { const t = m.text().slice(0, 200); if (/dnet|nnet|nnative|net:|\[eng\]|excise|schedule|Wire|E1L1|Out.Of.Sync|late|snapshot|sav|barrier|WAITING|Error|error/i.test(t)) rec(`[${tag}] ${t}`); });
  page.on('pageerror', e => rec(`[${tag}:PAGEERR] ` + String(e && e.message || e).slice(0, 300)));
  for (let i = 0; i < 150; i++) {
    const ok = await page.evaluate(() => { const g = document.getElementById('gear'); return !!g && getComputedStyle(g).display !== 'none'; }).catch(() => false);
    if (ok) { await applyPredMode(page); return true; }
    await new Promise(r => setTimeout(r, 1000));
  }
  return false;
}

// LOSSQ (e.g. '?lossmove=25&jitmove=150') injects synthetic duke-move loss and
// reorder on BOTH peers -- the loopback bench never drops anything on its own.
const LOSSQ = process.env.LOSSQ || '';
const hostBrowser = await chromium.launch({ headless: false, env: { ...process.env, DISPLAY: ':7' }, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required', '--window-position=0,0', '--window-size=1100,850', '--no-first-run'] });
const H = await hostBrowser.newPage({ viewport: { width: 1024, height: 768 } });
await H.goto('http://127.0.0.1:7800/' + LOSSQ, { waitUntil: 'commit', timeout: 60000 });
if (!(await boot(H, 'H'))) { console.log('FAIL host boot'); process.exit(1); }
const win = sh(`DISPLAY=:7 xdotool search --onlyvisible --class 'chrom' | tail -1`);
sh(`DISPLAY=:7 xdotool windowfocus --sync ${win}`);
await H.evaluate(() => { const c = document.querySelector('canvas'); if (c) { c.setAttribute('tabindex', '0'); c.focus(); } });
const g = await H.evaluate(() => { const c = document.querySelector('canvas'); const r = c.getBoundingClientRect(); return { x: r.x, y: r.y, uiH: window.outerHeight - window.innerHeight }; });
sh(`DISPLAY=:7 xdotool mousemove ${Math.round(g.x + 25)} ${Math.round(g.uiH + g.y + 25)}`);
sh(`DISPLAY=:7 xdotool mousemove ${Math.round(g.x + 29)} ${Math.round(g.uiH + g.y + 29)}`);
// evaluate() on a WEDGED (not crashed) page hangs forever -- no default
// timeout. Race every poll so an engine wedge shows up as empty telemetry
// instead of freezing the whole soak loop (learned: display-loop wedge run).
const st = (p) => Promise.race([
  p.evaluate(() => window.__e32menu || {}).catch(() => ({})),
  new Promise(r => setTimeout(() => r({ wedged: 1 }), 4000)),
]);
async function waitMenu(page, id, secs, label) {
  for (let i = 0; i < secs * 2; i++) { const m = await st(page); if (m.open === 1 && m.id === id) { rec(`${label} reached`); return true; } await new Promise(r => setTimeout(r, 500)); }
  rec(`TIMEOUT ${label}: ${JSON.stringify(await st(H))}`); return false;
}
if (!(await waitMenu(H, 0, 30, 'MAIN'))) { console.log('FAIL main'); process.exit(1); }
// SELECTION-VERIFIED navigation: press one arrow, confirm __e32menu.sel moved,
// repeat -- immune to dropped/repeated keys at any frame rate.
const tap = (k) => { sh(`DISPLAY=:7 xdotool keydown --clearmodifiers ${k}`); sh(`sleep 0.15`); sh(`DISPLAY=:7 xdotool keyup ${k}`); };
async function selTo(target, label, secs = 30) {
  for (let i = 0; i < secs * 2; i++) {
    const m = await st(H);
    if (m.sel === target) { rec(`${label}: sel=${target}`); return true; }
    tap(m.sel >= 0 && m.sel > target ? 'Up' : 'Down');
    await new Promise(r => setTimeout(r, 500));
  }
  rec(`SEL TIMEOUT ${label}: ${JSON.stringify(await st(H))}`); return false;
}
async function enterTo(id, label, secs = 12) {
  for (let i = 0; i < secs; i++) {
    tap('Return');
    for (let j = 0; j < 6; j++) { const m = await st(H); if (m.open === 1 && m.id === id) { rec(`${label} reached`); return true; } await new Promise(r => setTimeout(r, 500)); }
  }
  rec(`ENTER TIMEOUT ${label}: ${JSON.stringify(await st(H))}`); return false;
}
if (!(await selTo(2, 'MAIN>Multiplayer')) || !(await enterTo(20001, 'MULTIPLAYER'))) { console.log('FAIL mproot'); process.exit(1); }
// sel counts HIDDEN rows: index 0 is Change Map (hidden out-of-game) -> HostPub = 1
if (!(await selTo(1, 'MP>HostPub')) || !(await enterTo(20013, 'HOSTCFG'))) { console.log('FAIL hostcfg'); process.exit(1); }
if (!(await selTo(6, 'HOSTCFG>Start')) || !(await enterTo(20016, 'LOBBY'))) { console.log('FAIL lobby'); process.exit(1); }
let invite = '';
for (let i = 0; i < 25 && !invite; i++) { invite = await H.evaluate(() => { try { return window.DukeNet.match.inviteCode() || ''; } catch { return ''; } }).catch(() => ''); if (!invite) await new Promise(r => setTimeout(r, 1000)); }
if (!invite) { console.log('FAIL invite'); process.exit(1); }
rec(`invite ok (${invite.length})`);

const preGuestPids = pids();
const guestBrowser = await chromium.launch({ headless: false, env: { ...process.env, DISPLAY: ':8' }, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required', '--window-position=0,0', '--window-size=1100,850', '--no-first-run'] });
const G = await guestBrowser.newPage({ viewport: { width: 1024, height: 768 } });
await G.goto('http://127.0.0.1:7800/' + LOSSQ, { waitUntil: 'commit', timeout: 60000 });
if (!(await boot(G, 'G'))) { console.log('FAIL guest boot'); process.exit(1); }
const guestOnlyPids = [...pids()].filter(x => !preGuestPids.has(x));
// FOCUS the guest window too: an unfocused game auto-opens the menu and pauses
// rendering -- every earlier "guest stuck at menu over black" screenshot was THIS
// rig artifact, not the game. Park its pointer off the menu rows as well.
{
  const gwin = sh(`DISPLAY=:8 xdotool search --onlyvisible --class 'chrom' | tail -1`);
  sh(`DISPLAY=:8 xdotool windowfocus --sync ${gwin}`);
  await G.evaluate(() => { const c = document.querySelector('canvas'); if (c) { c.setAttribute('tabindex', '0'); c.focus(); } });
  const gg = await G.evaluate(() => { const c = document.querySelector('canvas'); const r = c.getBoundingClientRect(); return { x: r.x, y: r.y, uiH: window.outerHeight - window.innerHeight }; });
  sh(`DISPLAY=:8 xdotool mousemove ${Math.round(gg.x + 25)} ${Math.round(gg.uiH + gg.y + 25)}`);
  sh(`DISPLAY=:8 xdotool mousemove ${Math.round(gg.x + 29)} ${Math.round(gg.uiH + gg.y + 29)}`);
}

if (MODE === 'normal' || MODE === 'loss' || MODE === 'guestexit' || MODE === 'desync' || MODE === 'latejoin3') {
  await G.evaluate((c) => window.NetMenu.joinCode(c, 'SoakGuest'), invite);
  let joined = false;
  for (let i = 0; i < 60 && !joined; i++) { joined = (await G.evaluate(() => window.DukeNet.myConnectIndex).catch(() => -1)) > 0; if (!joined) await new Promise(r => setTimeout(r, 1000)); }
  if (!joined) { console.log('FAIL prelaunch join'); process.exit(1); }
  rec('guest joined pre-launch; host launching');
  await selTo(0, 'LOBBY>Launch'); tap('Return'); // Launch with 2 players
} else {
  await selTo(0, 'LOBBY>Launch');
  for (let a = 0; a < 8; a++) { tap('Return'); let ok = false; for (let j = 0; j < 4; j++) { const m = await st(H); if (m.game === 1) { ok = true; break; } await new Promise(r => setTimeout(r, 500)); } if (ok) break; }
  // NO-RESET proof: walk the host clearly off its spawn BEFORE the guest joins;
  // its position must SURVIVE the join (snapshot join preserves the world).
  await new Promise(r => setTimeout(r, 3000));
  for (let b = 0; b < 3; b++) { sh(`DISPLAY=:7 xdotool keydown w`); sh(`sleep 0.7`); sh(`DISPLAY=:7 xdotool keyup w`); await new Promise(r => setTimeout(r, 500)); }
  await new Promise(r => setTimeout(r, 1500));
  const pre = await st(H);
  global.__preJoinP0 = JSON.stringify(pre.p0);
  rec(`host walked pre-join: p0=${global.__preJoinP0} (spawn was [-31488,7168]-ish)`);
  rec('host solo in-game (moved); late guest joining');
  await G.evaluate((c) => window.NetMenu.joinCode(c, 'SoakGuest'), invite);
}

// Seam wire counters (temporary diagnosis): count outbound send/broadcast and
// inbound enqueues per channel on BOTH peers. move=0 rel=1 bulk=2.
for (const P of [H, G]) {
  await P.evaluate(() => {
    const s = window.DukeNet.seam;
    window.__seamctr = { out: [0, 0, 0], bro: [0, 0, 0], inq: [0, 0, 0] };
    const osend = s.send.bind(s);
    s.send = (t, ch, r, b) => { window.__seamctr.out[ch] = (window.__seamctr.out[ch] || 0) + 1; return osend(t, ch, r, b); };
    const obro = s.broadcast.bind(s);
    s.broadcast = (ch, r, b) => { window.__seamctr.bro[ch] = (window.__seamctr.bro[ch] || 0) + 1; return obro(ch, r, b); };
    const oenq = s.enqueueFrameByDevice.bind(s);
    s.enqueueFrameByDevice = (d, ch, b) => { window.__seamctr.inq[ch] = (window.__seamctr.inq[ch] || 0) + 1; return oenq(d, ch, b); };
  }).catch(() => {});
}

// Transition logger: record EVERY guest state change + page focus, 1s cadence.
const gTrail = [];
const trailTimer = setInterval(async () => {
  const m = await G.evaluate(() => ({ ...(window.__e32menu || {}), f: document.hasFocus() ? 1 : 0 })).catch(() => null);
  if (m) { const key = JSON.stringify(m); if (!gTrail.length || gTrail[gTrail.length - 1].key !== key) { gTrail.push({ key, t: Date.now() }); rec(`G-TRANSITION ${key}`); } }
}, 1000);

// both must reach game:1 np:2
let formed = false;
for (let i = 0; i < 150 && !formed; i++) {
  const hm = await st(H), gm = await st(G);
  if (hm.game === 1 && hm.np === 2 && gm.game === 1 && gm.np === 2) {
    formed = true; rec(`session formed: H=${JSON.stringify(hm)} G=${JSON.stringify(gm)}`);
    if (MODE === 'desync') {   // arm MISMATCH cat dumps for the heal-loop hunt
      await H.evaluate(() => window.Module.ccall('Web_SetForensics', null, ['number'], [1])).catch(() => {});
      await G.evaluate(() => window.Module.ccall('Web_SetForensics', null, ['number'], [1])).catch(() => {});
      rec('forensics armed on both pages');
    }
  }
  else if (i % 10 === 9) {
    rec(`forming... H=${JSON.stringify(hm)} G=${JSON.stringify(gm)}`);
    const hr = await H.evaluate(() => window.__rejections || []).catch(() => []);
    const gr = await G.evaluate(() => window.__rejections || []).catch(() => []);
    if (hr.length) rec(`H REJECTIONS: ${JSON.stringify(hr.slice(-2))}`);
    if (gr.length) rec(`G REJECTIONS: ${JSON.stringify(gr.slice(-2))}`);
  }
  if (!formed) await new Promise(r => setTimeout(r, 1000));
}
if (!formed) { console.log('FAIL session never formed 2p'); rec('FORM FAIL'); }

if (MODE === 'hostexit') {
  // Form the session, play 8s, then KILL the host browser abruptly. The guest must
  // land back on the MAIN MENU (open:1 id:0 game:0) instead of starving/soloing.
  await new Promise(r => setTimeout(r, 8000));
  rec('killing host browser');
  const hpids = sh(`pgrep -f 'ms-playwright.*chrom.*:7|--display=:7'`) || '';
  try { await hostBrowser.close(); } catch {}
  sh(`DISPLAY=:7 true`);
  let backToMenu = false;
  for (let t = 0; t < 40; t++) {
    await new Promise(r => setTimeout(r, 1000));
    const m = await st(G);
    if (t % 5 === 0) rec(`hostexit t=${t} G=${JSON.stringify(m)}`);
    if (m.open === 1 && m.id === 0 && m.game === 0) { backToMenu = true; rec(`guest back at MAIN MENU @${t}s`); break; }
  }
  console.log(JSON.stringify({ mode: MODE, formed, backToMenu }));
  await Promise.race([guestBrowser.close(), new Promise(r => setTimeout(r, 3000))]).catch(() => {});
  for (const p of [...pids()].filter(x => !before.has(x))) { try { process.kill(p, 'SIGKILL'); } catch { } }
  process.exit(backToMenu ? 0 : 1);
}

if (MODE === 'guestexit') {
  // DISCONNECT ARC: combat, then SIGKILL the guest browser ABRUPTLY (no clean
  // close, no beforeunload -- the worst case). The host must (a) keep simulating,
  // (b) deterministically drop the seat (np 2->1), then (c) accept a brand-new
  // guest into the running match via snapshot join.
  for (let t = 0; t < 8; t += 4) {
    sh(`DISPLAY=:7 xdotool keydown w`); sh(`DISPLAY=:8 xdotool keydown w`); sh(`sleep 0.3`);
    sh(`DISPLAY=:7 xdotool keyup w`); sh(`DISPLAY=:8 xdotool keyup w`);
    await new Promise(r => setTimeout(r, 3500));
  }
  const preKill = await st(H);
  rec(`pre-kill H=${JSON.stringify(preKill)}`);
  rec(`SIGKILLing guest pids: ${guestOnlyPids.join(',')}`);
  for (const p of guestOnlyPids) { try { process.kill(p, 'SIGKILL'); } catch {} }

  let npDropped = false, dropSecs = -1, hostContinued = false;
  let c2AtDrop = -1, p0AtDrop = null;
  for (let t = 0; t < 30; t++) {
    await new Promise(r => setTimeout(r, 1000));
    const m = await st(H);
    if (t % 3 === 0) rec(`guestexit t=${t} H=${JSON.stringify(m)}`);
    if (!npDropped && m.np === 1 && m.game === 1) {
      npDropped = true; dropSecs = t + 1; c2AtDrop = m.c2 | 0; p0AtDrop = JSON.stringify(m.p0);
      rec(`seat dropped @${dropSecs}s`);
      sh(`DISPLAY=:7 xdotool keydown w`); sh(`sleep 1.2`); sh(`DISPLAY=:7 xdotool keyup w`);
      continue;
    }
    // Solo mode freezes movefifoplc BY DESIGN -- continuation is proven by the
    // tic counter (c2) still climbing and the host's avatar actually walking.
    if (npDropped && m.game === 1 && (m.c2 | 0) > c2AtDrop + 60 && JSON.stringify(m.p0) !== p0AtDrop) {
      hostContinued = true; rec(`host still simulating (c2 ${c2AtDrop} -> ${m.c2}, p0 ${p0AtDrop} -> ${JSON.stringify(m.p0)})`);
      break;
    }
  }

  // (c) a brand-new guest drops into the running match (snapshot join).
  let rejoinFormed = false, rejoinSync = false;
  if (npDropped && hostContinued) {
    const g2Browser = await chromium.launch({ headless: false, env: { ...process.env, DISPLAY: ':8' }, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required', '--window-position=0,0', '--window-size=1100,850', '--no-first-run'] });
    const G2 = await g2Browser.newPage({ viewport: { width: 1024, height: 768 } });
    await G2.goto('http://127.0.0.1:7800/' + LOSSQ, { waitUntil: 'commit', timeout: 60000 });
    if (await boot(G2, 'G')) {
      await G2.evaluate((c) => window.NetMenu.joinCode(c, 'Rejoiner'), invite);
      for (let t = 0; t < 90 && !rejoinFormed; t++) {
        await new Promise(r => setTimeout(r, 1000));
        const hm = await st(H), gm = await st(G2);
        if (t % 10 === 9) rec(`rejoin t=${t} H=${JSON.stringify(hm)} G2=${JSON.stringify(gm)}`);
        if (hm.game === 1 && hm.np === 2 && gm.game === 1 && gm.np === 2) { rejoinFormed = true; rec(`REJOIN formed @${t}s`); }
      }
      if (rejoinFormed) {
        // brief combat, then require GROWING CRC comparisons and no sync flag.
        // (Absolute sc floors trip on bench-load variance; growth is the real
        // liveness signal, and sync:0 with growth means clean comparisons.)
        const h0 = await st(H), g0 = await st(G2);
        for (let t = 0; t < 15; t += 5) {
          sh(`DISPLAY=:7 xdotool keydown w`); sh(`DISPLAY=:8 xdotool keydown Left`); sh(`sleep 0.3`);
          sh(`DISPLAY=:7 xdotool keyup w`); sh(`DISPLAY=:8 xdotool keyup Left`);
          await new Promise(r => setTimeout(r, 4500));
        }
        const hm = await st(H), gm = await st(G2);
        rejoinSync = hm.sync === 0 && gm.sync === 0
          && (hm.sc - h0.sc) > 30 && (gm.sc - g0.sc) > 30;
        rec(`rejoin end H=${JSON.stringify(hm)} G2=${JSON.stringify(gm)} scDelta=[${hm.sc - h0.sc},${gm.sc - g0.sc}]`);
      }
    }
    await Promise.race([g2Browser.close(), new Promise(r => setTimeout(r, 3000))]).catch(() => {});
  }
  const verdict = { mode: MODE, formed, npDropped, dropSecs, hostContinued, rejoinFormed, rejoinSync };
  rec('VERDICT ' + JSON.stringify(verdict));
  console.log(JSON.stringify(verdict));
  await Promise.race([hostBrowser.close(), new Promise(r => setTimeout(r, 3000))]).catch(() => {});
  for (const p of [...pids()].filter(x => !before.has(x))) { try { process.kill(p, 'SIGKILL'); } catch { } }
  process.exit(npDropped && hostContinued && rejoinFormed && rejoinSync ? 0 : 1);
}

if (MODE === 'desync') {
  // AUTO-RESYNC IN ANGER: play clean, then FORK the guest's deterministic RNG
  // (Web_ForceDesync). The CRC watchdog must flag the divergence, the host must
  // push a healing snapshot (epoch bump), and the match must come back
  // CRC-clean and playable -- the exact route a real desync bug would take.
  for (let t = 0; t < 10; t += 5) {
    sh(`DISPLAY=:7 xdotool keydown w`); sh(`DISPLAY=:8 xdotool keydown Left`); sh(`sleep 0.3`);
    sh(`DISPLAY=:7 xdotool keyup w`); sh(`DISPLAY=:8 xdotool keyup Left`);
    await new Promise(r => setTimeout(r, 4500));
  }
  const pre = await st(H);
  rec(`pre-desync H.ep=${pre.ep} sc=${pre.sc} plc=${pre.plc}`);
  await G.evaluate(() => window.Module.ccall('Web_ForceDesync', null, [], []));
  rec('FORCED DESYNC on guest');
  // GROUND TRUTH: compare the raw CRC bytes at the shared 64-aligned reference
  // tic. Distinguishes "worlds re-converged" from "detector is blind".
  for (let q = 0; q < 40; q++) {
    const hs = await st(H), gs = await st(G);
    if (hs.cr && gs.cr && hs.cr[0] === gs.cr[0] && hs.cr[0] > 0) {
      rec(`CRC-PROBE tic=${hs.cr[0]} H=[${hs.cr[1]},${hs.cr[2]}] G=[${gs.cr[1]},${gs.cr[2]}] differ=${hs.cr[1] !== gs.cr[1] || hs.cr[2] !== gs.cr[2]}`);
      break;
    }
    await new Promise(r => setTimeout(r, 400));
  }
  // TARGETED HEAL (build 39+): the host streams the snapshot to the diverged
  // guest ONLY. Assertions flipped from the broadcast era: the epoch must NOT
  // bump (the running generation keeps flowing for veterans), the HOST must
  // never stall (its c2 keeps advancing through the whole cure), and the guest
  // must pass through watcher catchup (jn 1 -> 0, healResume).
  let fired = false, healed = false, jnSeen = false, epStable = true;
  const hStall = { last: -1, at: Date.now(), max: 0 };
  let monOn = true;
  const mon = setInterval(async () => {
    if (!monOn) return;
    const hm = await st(H); const now = Date.now();
    if (hm.c2 === undefined) { hStall.at = now; return; }  // harness-side sample failure: not a stall
    const c = hm.c2 | 0;
    if (c !== hStall.last) { hStall.last = c; hStall.at = now; }
    else hStall.max = Math.max(hStall.max, now - hStall.at);
    const gm2 = await st(G);
    if (gm2.jn === 1) jnSeen = true;
  }, 250);
  for (let t = 0; t < 45; t++) {
    await new Promise(r => setTimeout(r, 1000));
    const hm = await st(H), gm2 = await st(G);
    if (t % 5 === 0) rec(`desync t=${t} H(sync=${hm.sync} ep=${hm.ep} plc=${hm.plc} sc=${hm.sc} c2=${hm.c2}) G(sync=${gm2.sync} ep=${gm2.ep} plc=${gm2.plc} sc=${gm2.sc} jn=${gm2.jn})`);
    if (!fired && /AUTO-RESYNC/.test(fs.readFileSync(LOG, 'utf8'))) { fired = true; rec(`targeted heal FIRED @${t}s`); }
    if (hm.ep !== undefined && hm.ep !== pre.ep) { epStable = false; rec(`EPOCH BUMPED (${pre.ep} -> ${hm.ep}) -- targeted heal must not do that`); }
    if (fired && hm.sync === 0 && gm2.sync === 0 && gm2.ep === hm.ep && hm.game === 1 && gm2.game === 1 && gm2.jn === 0 && jnSeen) {
      healed = true; rec(`HEALED @${t}s (ep stable at ${hm.ep}, host maxStall ${hStall.max}ms)`); break;
    }
  }
  monOn = false; clearInterval(mon);
  let postClean = false;
  if (healed) {
    const h0 = await st(H), g0 = await st(G);
    for (let t = 0; t < 20; t += 5) {
      sh(`DISPLAY=:7 xdotool keydown w`); sh(`DISPLAY=:8 xdotool keydown Right`); sh(`sleep 0.3`);
      sh(`DISPLAY=:7 xdotool keyup w`); sh(`DISPLAY=:8 xdotool keyup Right`);
      await new Promise(r => setTimeout(r, 4500));
    }
    const h1 = await st(H), g1 = await st(G);
    postClean = h1.sync === 0 && g1.sync === 0 && (h1.sc - h0.sc) > 30 && (g1.sc - g0.sc) > 30 && h1.plc > h0.plc;
    rec(`post-heal H=${JSON.stringify(h1)} G=${JSON.stringify(g1)} scDelta=[${h1.sc - h0.sc},${g1.sc - g0.sc}]`);
  }
  const verdict = { mode: MODE, formed, fired, jnSeen, epStable, healed, postClean, hostMaxStallMs: hStall.max };
  rec('VERDICT ' + JSON.stringify(verdict));
  console.log(JSON.stringify(verdict));
  clearInterval(trailTimer);
  await Promise.race([hostBrowser.close(), new Promise(r => setTimeout(r, 3000))]).catch(() => {});
  await Promise.race([guestBrowser.close(), new Promise(r => setTimeout(r, 3000))]).catch(() => {});
  for (const p of [...pids()].filter(x => !before.has(x))) { try { process.kill(p, 'SIGKILL'); } catch { } }
  process.exit(fired && epStable && healed && postClean && hStall.max < 2500 ? 0 : 1);
}

if (MODE === 'latejoin3') {
  // NO-STALL LATE JOIN (the user's directive): host + G1 play a RUNNING match;
  // G2 joins mid-game. The barrier-free flow must keep BOTH veterans' tic
  // counters climbing through G2's entire snapshot/texture-load/catchup, seat
  // G2 deterministically (np=3 everywhere), and stay CRC-clean afterward.
  for (let t = 0; t < 10; t += 5) {
    sh(`DISPLAY=:7 xdotool keydown w`); sh(`DISPLAY=:8 xdotool keydown Left`); sh(`sleep 0.3`);
    sh(`DISPLAY=:7 xdotool keyup w`); sh(`DISPLAY=:8 xdotool keyup Left`);
    await new Promise(r => setTimeout(r, 4500));
  }
  // Stall monitor: track the longest wall-clock gap in which each veteran's
  // consume counter (c2) failed to advance. Surface emits are ~500ms cadence,
  // so only gaps well past that are real stalls (the OLD flow froze 5-10s+).
  const stall = { H: { last: -1, at: Date.now(), max: 0 }, G: { last: -1, at: Date.now(), max: 0 } };
  let monitorOn = true;
  const mon = setInterval(async () => {
    if (!monitorOn) return;
    for (const [tag, page] of [['H', H], ['G', G]]) {
      const m = await st(page); const s = stall[tag]; const now = Date.now();
      if (m.c2 === undefined) { s.at = now; continue; }  // harness-side sample failure: not a stall
      const c = m.c2 | 0;
      if (c !== s.last) { s.last = c; s.at = now; }
      else s.max = Math.max(s.max, now - s.at);
    }
  }, 250);
  rec('launching G2 (headless) into the running match');
  // Headless pages throttle rAF when chromium decides they are backgrounded --
  // the engine loop then stops SAMPLING and the host's 10s real-progress axe
  // (correctly) kicks the "dead" peer. These flags keep a headless G2 running
  // at full rate.
  const g2Browser = await chromium.launch({ headless: true, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required',
    '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows'] });
  const G2 = await g2Browser.newPage({ viewport: { width: 1024, height: 768 } });
  await G2.goto('http://127.0.0.1:7800/' + LOSSQ, { waitUntil: 'commit', timeout: 60000 });
  let joined3 = false, joinSecs = -1, g2jnSeen = false;
  if (await boot(G2, 'G')) {
    const t0 = Date.now();
    await G2.evaluate((c) => window.NetMenu.joinCode(c, 'LateThird'), invite);
    for (let t = 0; t < 120 && !joined3; t++) {
      await new Promise(r => setTimeout(r, 1000));
      const hm = await st(H), g1m = await st(G), g2m = await G2.evaluate(() => window.__e32menu || {}).catch(() => ({}));
      if (g2m.jn === 1) g2jnSeen = true;
      if (t % 8 === 7) rec(`join3 t=${t} H(np=${hm.np},c2=${hm.c2},fl=${hm.fl}) G1(np=${g1m.np}) G2(np=${g2m.np},game=${g2m.game},jn=${g2m.jn},plc=${g2m.plc})`);
      if (hm.np === 3 && g1m.np === 3 && g2m.np === 3 && g2m.game === 1 && g2m.jn === 0) {
        joined3 = true; joinSecs = Math.round((Date.now() - t0) / 1000);
        rec(`G2 SEATED @${joinSecs}s H=${JSON.stringify(hm)} G2=${JSON.stringify(g2m)}`);
      }
    }
  }
  monitorOn = false; clearInterval(mon);
  // Post-join: brief combat, then CRC growth + no sync flag anywhere.
  let postSync = false;
  if (joined3) {
    const h0 = await st(H), g20 = await G2.evaluate(() => window.__e32menu || {}).catch(() => ({}));
    for (let t = 0; t < 15; t += 5) {
      sh(`DISPLAY=:7 xdotool keydown w`); sh(`DISPLAY=:8 xdotool keydown Right`); sh(`sleep 0.3`);
      sh(`DISPLAY=:7 xdotool keyup w`); sh(`DISPLAY=:8 xdotool keyup Right`);
      await new Promise(r => setTimeout(r, 4500));
    }
    const hm = await st(H), g1m = await st(G), g2m = await G2.evaluate(() => window.__e32menu || {}).catch(() => ({}));
    postSync = hm.sync === 0 && g1m.sync === 0 && g2m.sync === 0
      && (hm.sc - h0.sc) > 30 && (g2m.sc - (g20.sc | 0)) > 30;
    rec(`post-join H=${JSON.stringify(hm)}`);
    rec(`post-join G1=${JSON.stringify(g1m)}`);
    rec(`post-join G2=${JSON.stringify(g2m)}`);
  }
  const verdict = { mode: MODE, formed, joined3, joinSecs, g2jnSeen, postSync,
    hostMaxStallMs: stall.H.max, g1MaxStallMs: stall.G.max };
  rec('VERDICT ' + JSON.stringify(verdict));
  console.log(JSON.stringify(verdict));
  clearInterval(trailTimer);
  await Promise.race([g2Browser.close(), new Promise(r => setTimeout(r, 3000))]).catch(() => {});
  await Promise.race([hostBrowser.close(), new Promise(r => setTimeout(r, 3000))]).catch(() => {});
  await Promise.race([guestBrowser.close(), new Promise(r => setTimeout(r, 3000))]).catch(() => {});
  for (const p of [...pids()].filter(x => !before.has(x))) { try { process.kill(p, 'SIGKILL'); } catch { } }
  process.exit(joined3 && postSync && stall.H.max < 2500 && stall.G.max < 2500 ? 0 : 1);
}

// SOAK 60s: BOTH players move in alternating bursts (player 2's own input path
// was the untested direction); sample state; guest shake via consecutive frames.
const start = { h: null, g: null };
{
  const m = await st(H);
  start.h = JSON.stringify(m.p0); start.g = JSON.stringify(m.p1);
  rec(`start positions p0=${start.h} p1=${start.g}`);
}
let syncBad = false, syncFlags = 0, shakes = [];
// COMBAT SOAK: both players walk, TURN, and the host FIRES -- the input mix a
// real match produces (turns exercise avel, fire exercises krand-heavy paths).
const hostActs = [['w', 0.25], ['Right', 0.35], ['w', 0.2], ['control', 0.3], ['Left', 0.35], ['control', 0.3]];
const guestActs = [['w', 0.2], ['Left', 0.35], ['w', 0.25], ['Right', 0.35]];
for (let t = 0; t < 90; t += 5) {
  const ha = hostActs[(t / 5) % hostActs.length], ga = guestActs[(t / 5) % guestActs.length];
  sh(`DISPLAY=:7 xdotool keydown ${ha[0]}`); sh(`sleep ${ha[1]}`); sh(`DISPLAY=:7 xdotool keyup ${ha[0]}`);
  sh(`DISPLAY=:8 xdotool keydown ${ga[0]}`); sh(`sleep ${ga[1]}`); sh(`DISPLAY=:8 xdotool keyup ${ga[0]}`);
  await new Promise(r => setTimeout(r, 1000));
  const hm = await st(H), gm = await st(G);
  rec(`t=${t} H=${JSON.stringify(hm)} G=${JSON.stringify(gm)}`);
  if (hm.sync === 1 || gm.sync === 1) { syncFlags++; rec('SYNC ERROR FLAGGED (auto-resync should heal)'); }
  if (t === 25 || t === 50) {
    for (let f = 0; f < 4; f++) { sh(`import -display :8 -window root /tmp/nn_shots/soak_${MODE}_g_t${t}_f${f}.png`); await new Promise(r => setTimeout(r, 700)); }
    for (let f = 0; f < 3; f++) {
      const d = sh(`compare -metric AE /tmp/nn_shots/soak_${MODE}_g_t${t}_f${f}.png /tmp/nn_shots/soak_${MODE}_g_t${t}_f${f + 1}.png null: 2>&1 || true`);
      shakes.push(`${t}:${f}=${d}`);
    }
  }
  await new Promise(r => setTimeout(r, 2900));
}
// QUIESCE: all input off, let the fifos drain, then compare EXACT world state.
// release EVERY key we ever pressed (a lost keyup walks a player through the
// "quiesce" and aliases as divergence in stale surface emits)
for (const k of ['w', 'Left', 'Right', 'control']) { sh(`DISPLAY=:7 xdotool keyup ${k}`); sh(`DISPLAY=:8 xdotool keyup ${k}`); }
await new Promise(r => setTimeout(r, 6000));
// plc-matched compare: sample repeatedly; only compare when both peers report the
// SAME simulated tic (falling/settling players otherwise alias as divergence).
let quiesceAgree = null, qH = null, qG = null;
for (let q = 0; q < 12; q++) {
  qH = await st(H); qG = await st(G);
  if (qH.plc === qG.plc) {
    quiesceAgree = JSON.stringify(qH.p0) === JSON.stringify(qG.p0) && JSON.stringify(qH.p1) === JSON.stringify(qG.p1);
    break;
  }
  await new Promise(r => setTimeout(r, 700));
}
const syncEnd = (qH.sync === 0 && qG.sync === 0);
// THE authoritative verdict: the lockstep CRC compared thousands of per-tic
// state hashes (engine/rng/positions/projectiles/actors/map). Positions above
// are advisory only -- surface emits are up to 500ms stale on a moving world.
const syncActive = (qH.sc > 500 && qG.sc > 500);
rec(`QUIESCE plcH=${qH.plc} plcG=${qG.plc} H.p0=${JSON.stringify(qH.p0)} G.p0=${JSON.stringify(qG.p0)} H.p1=${JSON.stringify(qH.p1)} G.p1=${JSON.stringify(qG.p1)} agree=${quiesceAgree} syncEnd=${syncEnd} syncActive=${syncActive} scH=${qH.sc} scG=${qG.sc}`);
sh(`import -display :7 -window root /tmp/nn_shots/soak_${MODE}_h_final.png`);
sh(`import -display :8 -window root /tmp/nn_shots/soak_${MODE}_g_final.png`);
clearInterval(trailTimer);
// BOTH players must have MOVED, and both peers must agree on both positions.
const endH = await st(H), endG = await st(G);
const p0Moved = JSON.stringify(endH.p0) !== start.h;
const p1Moved = JSON.stringify(endH.p1) !== start.g;
rec('=== G full console tail ===');
for (const l of ringAll.G.slice(-25)) rec('G>> ' + l);
let noReset = null;
if (MODE === 'latejoin' && global.__preJoinP0) {
  // compare the FIRST formed-session p0 sample against the pre-join walk position:
  // a relaunch would snap the host back to its spawn point.
  const pre = JSON.parse(global.__preJoinP0);
  const post = endH.p0; // end-of-soak includes soak movement; use the formed-time trail instead
  const trail = fs.readFileSync(LOG, 'utf8').match(/session formed: H=({[^\n]*?})/);
  let formedP0 = null;
  if (trail) { try { formedP0 = JSON.parse(trail[1]).p0; } catch {} }
  if (formedP0) {
    const d = Math.hypot(formedP0[0] - pre[0], formedP0[1] - pre[1]);
    noReset = d < 4096; // within ~16 map-units-of-256: no teleport back to spawn
    rec(`no-reset check: pre=${JSON.stringify(pre)} formed=${JSON.stringify(formedP0)} dist=${Math.round(d)} -> ${noReset}`);
  }
}
const verdict = { mode: MODE, formed, syncFlags, syncActive, syncEnd, quiesceAgree, p0Moved, p1Moved, noReset,
  duH: endH.du, duG: endG.du, gpH: endH.gp, gpG: endG.gp, stH: endH.st, stG: endG.st };
rec('VERDICT ' + JSON.stringify(verdict));
console.log(JSON.stringify(verdict));
await Promise.race([hostBrowser.close(), new Promise(r => setTimeout(r, 3000))]).catch(() => {});
await Promise.race([guestBrowser.close(), new Promise(r => setTimeout(r, 3000))]).catch(() => {});
for (const p of [...pids()].filter(x => !before.has(x))) { try { process.kill(p, 'SIGKILL'); } catch { } }
process.exit(0);
