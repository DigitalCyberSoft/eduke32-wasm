// H-01 RED characterization: CPU ownership must survive a same-match Change Map.
// Baseline Net_SeatBots clears the mask, counts still-connected CPUs as humans,
// and leaves no CPU input source. The resulting bot movement stops.
import {
  bitSeats,
  invariant,
  multiplayerConfig,
  parseWorldProbe,
  runProbe,
  sleep,
} from "./probe-util.mjs";

async function sampleWorld(harness, host) {
  const before = host.probeLines.length;
  await harness.ccall(host, "Web_NetProbe", null, [], []);
  for (let attempt = 0; attempt < 20; attempt++) {
    if (host.probeLines.length > before) {
      const world = parseWorldProbe(host.probeLines.at(-1));
      invariant(world, "WORLD_PROBE", "could not parse Web_NetProbe output");
      return world;
    }
    await sleep(25);
  }
  throw new Error("Web_NetProbe emitted no line");
}

await runProbe("h01_change_map_red", { minimumMB: 6144 }, async (harness) => {
  const config = multiplayerConfig({
    matchName: "H01 Change Map",
    maxPlayers: 3,
    minPlayers: 3,
    botSkill: 2,
    gametype: 0,
    episode: 0,
    level: 0,
    autoAim: 0,
    localOnly: 1,
  });
  const host = await harness.newPage("H", config);
  await harness.setupPrivateHost(host);
  await harness.launchHost(host);
  await harness.waitExactRoster([host], 3);
  const beforeMask = await harness.botMask(host);
  const seats = bitSeats(beforeMask);
  invariant(seats.length === 2, "FIXTURE", "H-01 fixture needs exactly two CPU seats", { beforeMask, seats });

  // Prove they were live input sources before relaunch.
  const beforeFirst = await sampleWorld(harness, host);
  await sleep(5_000);
  const beforeLast = await sampleWorld(harness, host);
  const preMove = seats.map((seat) => {
    const a = beforeFirst.seats.get(seat), b = beforeLast.seats.get(seat);
    invariant(a && b, "FIXTURE", `bot seat ${seat} missing before Change Map`);
    return Math.abs(b.x - a.x) + Math.abs(b.y - a.y);
  });
  invariant(preMove.some((distance) => distance > 128), "FIXTURE", "bots were not moving before Change Map", { preMove });

  // Open Multiplayer from the running game, choose Change Map, then Warp. The
  // current map is sufficient: ownership loss is caused by relaunch itself.
  await harness.tap(host, "Escape");
  await harness.waitForState(host, (state) => state.open === 1, "in-game menu", 10_000);
  await harness.selectRow(host, 3, "in-game main > multiplayer");
  await harness.enterMenu(host, 20001, "multiplayer root");
  await harness.selectRow(host, 0, "multiplayer > change map");
  await harness.enterMenu(host, 20017, "change map");
  await harness.selectRow(host, 2, "change map > warp");
  await harness.tap(host, "Return");
  await harness.waitForState(host, (state) => state.game === 1 && state.np === 3,
    "Change Map relaunch", 90_000);

  const afterMask = await harness.botMask(host);
  invariant(afterMask === beforeMask, "H01_BOT_MASK_LOST", "Change Map dropped CPU seat ownership", {
    beforeMask: `0x${beforeMask.toString(16)}`,
    afterMask: `0x${afterMask.toString(16)}`,
  });
  const afterFirst = await sampleWorld(harness, host);
  await sleep(8_000);
  const afterLast = await sampleWorld(harness, host);
  const postMove = seats.map((seat) => {
    const a = afterFirst.seats.get(seat), b = afterLast.seats.get(seat);
    invariant(a && b, "H01_BOT_MISSING", `bot seat ${seat} missing after Change Map`);
    return Math.abs(b.x - a.x) + Math.abs(b.y - a.y);
  });
  for (let index = 0; index < seats.length; index++)
    invariant(postMove[index] > 128, "H01_BOT_STOPPED", `bot seat ${seats[index]} stopped after Change Map`, { postMove });
  return { seats, beforeMask, afterMask, preMove, postMove };
});
