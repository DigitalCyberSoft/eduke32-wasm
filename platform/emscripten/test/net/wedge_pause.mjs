// Definitive wedge localizer: keep the wasm debuggable from boot, join the
// match, wait for the post-seat wedge, then Debugger.pause the spinning main
// thread and print the exact wasm call stack.
import { chromium } from "playwright";
const CODE = process.env.CODE || '';
if (!CODE) { console.log('need CODE env'); process.exit(2); }
const t0 = Date.now();
const ts = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(6);

const B = await chromium.launch({ headless: true, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding'] });
const P = await B.newPage({ viewport: { width: 800, height: 600 } });
let seatedAt = 0;
P.on('console', (m) => {
  const t = m.text();
  if (/joinApplied|joinScheduled|excise|Apply:|spin|alive|\[pred\]|\[eng\]|\[clip\]/.test(t)) {
    console.log(`${ts()} [con] ${t.slice(0, 160)}`);
    if (t.includes('joinApplied')) seatedAt = Date.now();
  }
});
P.on('crash', () => { console.log(`${ts()} PAGE CRASHED`); process.exit(3); });

const cdp = await P.context().newCDPSession(P);
await cdp.send('Debugger.enable');           // BEFORE wasm compiles: keeps frames walkable
cdp.on('Debugger.paused', async (ev) => {
  console.log(`${ts()} === PAUSED: call stack (top first) ===`);
  for (const f of ev.callFrames.slice(0, 10)) {
    const fn = f.functionName || '(anon)';
    console.log(`  ${fn}`);
  }
  // Dump wasm locals/stack for the top frames: getzrange's params tell us what
  // position/sector it is spinning on.
  for (const f of ev.callFrames.slice(0, 3)) {
    console.log(`--- scopes of ${f.functionName} ---`);
    for (const sc of f.scopeChain || []) {
      if (!sc.object?.objectId) continue;
      try {
        const props = await cdp.send('Runtime.getProperties', { objectId: sc.object.objectId, ownProperties: true });
        const vals = (props.result || []).slice(0, 24).map(p => {
          const v = p.value || {};
          let d = v.description ?? v.value;
          if (typeof d === 'object') d = JSON.stringify(d);
          return `${p.name}=${String(d).slice(0, 24)}`;
        });
        console.log(`  [${sc.type}] ${vals.join(' ')}`);
      } catch (e) { console.log(`  [${sc.type}] <${e.message}>`); }
    }
  }
  try { await cdp.send('Debugger.resume'); } catch {}
  setTimeout(() => process.exit(0), 500);
});

await P.goto(`http://127.0.0.1:7800/?join=${CODE}`, { waitUntil: 'commit', timeout: 60000 });
console.log(`${ts()} loaded`);

for (let i = 0; i < 120 && !seatedAt; i++) await new Promise(r => setTimeout(r, 1000));
if (!seatedAt) { console.log('never seated'); await B.close(); process.exit(1); }
console.log(`${ts()} seated; letting the wedge set in (6s)...`);
await new Promise(r => setTimeout(r, 6000));
console.log(`${ts()} sending Debugger.pause`);
await cdp.send('Debugger.pause');
// If the paused event never lands, report and bail.
setTimeout(async () => { console.log(`${ts()} pause never landed`); await B.close().catch(() => {}); process.exit(1); }, 20000);
