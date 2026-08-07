// AIM-TRUTH PROBE: join a running match as a guest, wiggle the view like a
// mouse (Web_TestAim), and assert the SIM's direction tracks the VIEW within
// tolerance -- i.e., the shot truth comes from the player that fires.
import pkg from '/home/user/dukenukem3d/webduke3d/node_modules/playwright/index.js';
const { chromium } = pkg;
const CODE = process.env.CODE || '';
if (!CODE) { console.log('need CODE env'); process.exit(2); }
const t0 = Date.now();
const ts = () => ((Date.now() - t0) / 1000).toFixed(1).padStart(6);

const B = await chromium.launch({ headless: true, args: ['--mute-audio', '--autoplay-policy=no-user-gesture-required'] });
const P = await B.newPage({ viewport: { width: 800, height: 600 } });
let seated = false;
P.on('console', (m) => { if (m.text().includes('joinApplied')) seated = true; });
P.on('crash', () => { console.log(`${ts()} PAGE CRASHED`); process.exit(3); });
await P.goto(`http://127.0.0.1:7800/?join=${CODE}`, { waitUntil: 'commit', timeout: 60000 });
for (let i = 0; i < 90 && !seated; i++) await new Promise(r => setTimeout(r, 1000));
if (!seated) { console.log('never seated'); await B.close(); process.exit(1); }
console.log(`${ts()} seated -- wiggling aim`);
await new Promise(r => setTimeout(r, 2500));

// Deterministic wiggle pattern: sweeps in yaw and pitch, with settle reads.
const pattern = [[60, 0], [60, 10], [-120, -10], [0, 15], [-60, -25], [120, 5], [0, -5], [-30, 10]];
let samples = 0, bad = 0, worstA = 0, worstH = 0;
for (const [da, dh] of pattern) {
  await P.evaluate(([a, h]) => Module.ccall('Web_TestAim', null, ['number', 'number'], [a, h]), [da, dh]).catch(() => {});
  // Let 4-5 tics consume (30Hz), then sample the gap over the next ~0.5s.
  await new Promise(r => setTimeout(r, 180));
  for (let s = 0; s < 5; s++) {
    const g = await P.evaluate(() => [
      Module.ccall('Web_AimGapAng', 'number', [], []),
      Module.ccall('Web_AimGapHoriz', 'number', [], []),
    ]).catch(() => null);
    if (g) {
      samples++;
      const [ga, gh] = g.map((v) => Math.abs(v));
      worstA = Math.max(worstA, ga); worstH = Math.max(worstH, gh);
      if (ga > 3 || gh > 3) { bad++; console.log(`${ts()} GAP ang=${g[0]} horiz=${g[1]}`); }
    }
    await new Promise(r => setTimeout(r, 110));
  }
}
console.log(`samples=${samples} bad=${bad} worstAng=${worstA} worstHoriz=${worstH}`);
if (samples >= 30 && bad <= Math.ceil(samples * 0.1)) console.log('AIMTRACK PASS: sim follows the firing player\'s view');
else console.log('AIMTRACK FAIL');
await B.close().catch(() => {});
process.exit(bad <= Math.ceil(samples * 0.1) ? 0 : 1);
