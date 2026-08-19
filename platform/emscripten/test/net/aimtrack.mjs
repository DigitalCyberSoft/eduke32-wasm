// Self-contained host+guest aim-truth probe. Calls are never swallowed and a
// zero/partial sample run cannot pass: at least 30 measured samples are required.
import {
  invariant,
  multiplayerConfig,
  runProbe,
  sleep,
} from "./probe-util.mjs";

await runProbe("aimtrack", { minimumMB: 8192 }, async (harness) => {
  const config = multiplayerConfig({
    matchName: "Aim Track",
    maxPlayers: 2,
    minPlayers: 2,
    botSkill: 2,
    gametype: 0,
    episode: 0,
    level: 0,
    autoAim: 0,
    localOnly: 0,
  });
  const host = await harness.newPage("H", config);
  const invite = await harness.setupPrivateHost(host);
  const guest = await harness.newPage("G", null);
  const guestSlot = await harness.joinGuest(guest, invite, "AimGuest");
  invariant(guestSlot === 1, "GUEST_SLOT", "two-seat aim fixture did not assign guest seat 1", { guestSlot });
  await harness.launchHost(host);
  await harness.waitExactRoster([host, guest], 2, 90_000);

  const pattern = [[60, 0], [60, 10], [-120, -10], [0, 15], [-60, -25], [120, 5], [0, -5], [-30, 10]];
  let samples = 0;
  let bad = 0;
  let worstAng = 0;
  let worstHoriz = 0;
  const badSamples = [];

  for (const [angle, horiz] of pattern) {
    await harness.ccall(guest, "Web_TestAim", null, ["number", "number"], [angle, horiz]);
    await sleep(180);
    for (let index = 0; index < 5; index++) {
      harness.assertHealthy();
      const gapAng = Number(await harness.ccall(guest, "Web_AimGapAng", "number", [], []));
      const gapHoriz = Number(await harness.ccall(guest, "Web_AimGapHoriz", "number", [], []));
      invariant(Number.isFinite(gapAng) && Number.isFinite(gapHoriz),
        "AIM_SAMPLE", "aim export returned a non-finite sample", { gapAng, gapHoriz });
      samples++;
      worstAng = Math.max(worstAng, Math.abs(gapAng));
      worstHoriz = Math.max(worstHoriz, Math.abs(gapHoriz));
      if (Math.abs(gapAng) > 3 || Math.abs(gapHoriz) > 3) {
        bad++;
        badSamples.push({ sample: samples, gapAng, gapHoriz });
      }
      await sleep(110);
    }
  }

  invariant(samples >= 30, "AIM_ZERO_SAMPLE", "aim probe collected fewer than 30 samples", { samples });
  invariant(bad <= Math.ceil(samples * 0.1), "AIM_DIVERGED", "sim aim did not follow guest view", {
    samples, bad, worstAng, worstHoriz, badSamples,
  });
  return { samples, bad, worstAng, worstHoriz, guestSlot };
});
