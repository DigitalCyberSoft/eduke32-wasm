#include <cstdio>
#include <cstdlib>
#include <cstdint>

#define NETDUKE32 1
#include "bot_combat.h"

static int failures;

static void check(bool const ok, char const *const message)
{
    if (!ok)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

int main()
{
    BotShotModel grow = Bot_CombatStockShotModel(GROWSPARK__, GROW_WEAPON, 1780);
    check(grow.kind == BOT_SHOT_HITSCAN && grow.speed == 0,
          "Grower is hitscan and receives no fictitious lead");

    BotShotModel freeze = Bot_CombatStockShotModel(FREEZEBLAST__, FREEZE_WEAPON, 1780);
    check(freeze.kind == BOT_SHOT_LINEAR && freeze.speed == 644,
          "Freezer uses the hardcoded 644 projectile speed");

    BotShotModel rpg = Bot_CombatStockShotModel(RPG__, RPG_WEAPON, 1780);
    check(rpg.kind == BOT_SHOT_LINEAR && rpg.speed == 644 && rpg.splash == 1780,
          "RPG model uses hardcoded speed and full radius");

    BotShotModel devastator = Bot_CombatStockShotModel(RPG__, DEVISTATOR_WEAPON, 1780);
    check(devastator.kind == BOT_SHOT_LINEAR && devastator.speed == 644 && devastator.splash == 890,
          "Devastator model uses half RPG radius");

    BotShotModel thrown = Bot_CombatStockShotModel(HEAVYHBOMB__, HANDBOMB_WEAPON, 1780);
    check(thrown.kind == BOT_SHOT_NONE && thrown.speed == 0,
          "Thrown pipebomb does not receive linear leading");

    check(Bot_CombatLeadTicsFromDistance(5000, 500) == 10,
          "Lead time uses Euclidean caller distance");
    check(Bot_CombatLeadTicsFromDistance(5000, 0) == 0,
          "Hitscan/utility lead time is zero");
    // Manhattan distance for the 3-4-5 triangle would produce 14 tics; keeping
    // the exact Euclidean distance at the helper boundary prevents that regression.
    check(Bot_CombatLeadTicsFromDistance(5000, 500) != 14,
          "Lead time does not use Manhattan distance");

    check(Bot_CombatHorizonDeltaUnits(100, 4096, 4096) == -6,
          "Elevated target emits bounded authoritative up-pitch");
    check(Bot_CombatHorizonDeltaUnits(100, -4096, 4096) == 6,
          "Lower target emits bounded authoritative down-pitch");
    check(Bot_CombatHorizonDeltaUnits(106, 0, 0) == -6,
          "No target recenters authoritative pitch");

    check(!Bot_CombatBreakFireAllowed(true, true, false, true),
          "Combat target blocks environmental break-fire");
    check(!Bot_CombatBreakFireAllowed(false, true, true, true),
          "Friendly/safety veto blocks break-fire");
    check(!Bot_CombatBreakFireAllowed(false, true, false, false),
          "Throttle veto blocks break-fire");
    check(!Bot_CombatBreakFireAllowed(false, false, false, true),
          "Mismatched blocker cancels break-fire");
    check(Bot_CombatBreakFireAllowed(false, true, false, true),
          "Exact safe clearing blocker permits break-fire");

    if (failures)
        return EXIT_FAILURE;
    std::puts("PASS: bot combat model and safety gates");
    return EXIT_SUCCESS;
}
