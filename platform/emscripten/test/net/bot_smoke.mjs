// Shareware CPU-player smoke. One private host seats exactly two bots and the
// gate samples every bot seat for simulation progress and real movement.
import {
  bitSeats,
  invariant,
  multiplayerConfig,
  parseWorldProbe,
  runProbe,
} from "./probe-util.mjs";

const FLOOR = Number(process.env.MIN_PLAYERS ?? 3);
invariant(Number.isInteger(FLOOR) && FLOOR >= 2 && FLOOR <= 16,
  "CONFIG", "MIN_PLAYERS must be 2..16");
const DURATION_MS = Number(process.env.DURATION_MS ?? 90_000);

await runProbe("bot_smoke", { minimumMB: 6144 }, async (harness) => {
  const config = multiplayerConfig({
    matchName: "Bot Smoke",
    maxPlayers: FLOOR,
    minPlayers: FLOOR,
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
  await harness.waitExactRoster([host], FLOOR);

  const mask = await harness.botMask(host);
  const seats = bitSeats(mask);
  invariant(seats.length === FLOOR - 1, "BOT_ROSTER", "configured floor did not create the exact bot roster", {
    floor: FLOOR, mask: `0x${mask.toString(16)}`, seats,
  });
  invariant(!seats.includes(0), "BOT_HOST", "host seat 0 was marked as a bot", { seats });

  const bounds = new Map(seats.map((seat) => [seat, {
    samples: 0, minX: Infinity, maxX: -Infinity, minY: Infinity, maxY: -Infinity,
  }]));
  await harness.watchSimulation([host], DURATION_MS, {
    expectedPlayers: FLOOR,
    stallMs: 15_000,
    intervalMs: 1000,
    onSample: async (_record, _state, sample) => {
      if (sample % 2 !== 0) return;
      const before = host.probeLines.length;
      await harness.ccall(host, "Web_NetProbe", null, [], []);
      const line = await (async () => {
        for (let attempt = 0; attempt < 20; attempt++) {
          if (host.probeLines.length > before) return host.probeLines.at(-1);
          await new Promise((resolve) => setTimeout(resolve, 25));
        }
        return null;
      })();
      invariant(line, "PROBE_EXPORT", "Web_NetProbe emitted no world sample");
      const world = parseWorldProbe(line);
      invariant(world?.players === FLOOR, "WORLD_ROSTER", "world probe reported the wrong roster", { line });
      for (const seat of seats) {
        const player = world.seats.get(seat);
        invariant(player, "BOT_SAMPLE", `world probe omitted bot seat ${seat}`, { line });
        const b = bounds.get(seat);
        b.samples++;
        b.minX = Math.min(b.minX, player.x); b.maxX = Math.max(b.maxX, player.x);
        b.minY = Math.min(b.minY, player.y); b.maxY = Math.max(b.maxY, player.y);
      }
    },
  });

  const movement = seats.map((seat) => {
    const b = bounds.get(seat);
    return {
      seat,
      samples: b.samples,
      spread: (b.maxX - b.minX) + (b.maxY - b.minY),
    };
  });
  for (const sample of movement) {
    invariant(sample.samples >= 20, "BOT_SAMPLE_COUNT", `bot seat ${sample.seat} was undersampled`, sample);
    invariant(sample.spread > 512, "BOT_NO_MOVEMENT", `bot seat ${sample.seat} did not move`, sample);
  }
  return { floor: FLOOR, mode: "deathmatch", episode: 0, level: 0, botMask: mask, movement };
});
