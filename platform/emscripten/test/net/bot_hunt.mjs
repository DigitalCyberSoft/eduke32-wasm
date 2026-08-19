// Two-peer desync hunt over the repository's local NIP-01 relay only. The exact
// roster is mandatory; OOS, mismatch, sync repair, crashes, errors, and stalls fail.
import {
  invariant,
  multiplayerConfig,
  runProbe,
} from "./probe-util.mjs";

const ROSTER = Number(process.env.ROSTER ?? 6); // host + guest + four CPU seats
invariant(Number.isInteger(ROSTER) && ROSTER >= 2 && ROSTER <= 16,
  "CONFIG", "ROSTER must be 2..16");
const DURATION_MS = Number(process.env.DURATION_MS ?? 300_000);

await runProbe("bot_hunt", { minimumMB: 8192 }, async (harness) => {
  const config = multiplayerConfig({
    matchName: "Bot Hunt",
    maxPlayers: ROSTER,
    minPlayers: ROSTER,
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
  const guestSlot = await harness.joinGuest(guest, invite, "HuntGuest");
  invariant(guestSlot >= 1 && guestSlot <= 15, "GUEST_SLOT", "guest received an invalid seat", { guestSlot });

  await harness.launchHost(host);
  await harness.waitExactRoster([host, guest], ROSTER, 90_000);
  await harness.ccall(host, "Web_SetForensics", null, ["number"], [1]);
  await harness.ccall(guest, "Web_SetForensics", null, ["number"], [1]);
  if (process.env.LOCALBOT === "1") {
    await harness.ccall(host, "Web_SetLocalBot", null, ["number"], [1]);
    await harness.ccall(guest, "Web_SetLocalBot", null, ["number"], [1]);
  }

  await harness.watchSimulation([host, guest], DURATION_MS, {
    expectedPlayers: ROSTER,
    stallMs: 15_000,
    intervalMs: 1000,
  });

  const mismatchLines = harness.lines.filter((line) => /\bMISMATCH\b/i.test(line));
  invariant(mismatchLines.length === 0, "MISMATCH", "forensics reported state mismatch", {
    lines: mismatchLines.slice(-20),
  });
  return {
    roster: ROSTER,
    guestSlot,
    durationMs: DURATION_MS,
    localRelay: harness.relay.url,
    relayStats: harness.relay.stats,
    localBot: process.env.LOCALBOT === "1",
  };
});
