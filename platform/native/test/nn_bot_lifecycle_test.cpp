#include "bot_lifecycle.h"

#include <cstdio>
#include <cstdlib>

static int g_fail;

static void check(bool condition, char const *name)
{
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", name);
    if (!condition)
        g_fail++;
}

static BotPlayerTargetFacts liveFacts()
{
    return { 1, 2, 16, 1, 1, 42, 4096, 7, 1024, 0,
             10, 1024, 1405, 1405, 2, 100, 0, 0, 1 };
}

int main()
{
    BotRouteFailureState route;
    BotRouteStoreResult(route, BOT_TARGET_PLAYER, 2, false);
    bool earlyDrop = false;
    for (int tic = 1; tic < 40; tic++)
        earlyDrop |= BotRouteFailureTick(route, BOT_TARGET_PLAYER, 2, true);
    check(!earlyDrop, "route failure stays engaged before tic 40");
    check(BotRouteFailureTick(route, BOT_TARGET_PLAYER, 2, true),
          "player route failure disengages on generated tic 40");
    check(route.failedTics == 40, "failure counter is per input tic");

    BotRouteStoreResult(route, BOT_TARGET_PLAYER, 2, true);
    check(!BotRouteFailureTick(route, BOT_TARGET_PLAYER, 2, true)
          && route.failedTics == 0, "route success clears failure time");

    BotRouteStoreResult(route, BOT_TARGET_MONSTER, 99, false);
    check(!BotRouteFailureTick(route, BOT_TARGET_MONSTER, 100, true)
          && route.kind == BOT_TARGET_NONE && route.failedTics == 0,
          "target change clears stale route verdict");

    BotRouteStoreResult(route, BOT_TARGET_MONSTER, 99, false);
    for (int tic = 1; tic < 40; tic++)
        BotRouteFailureTick(route, BOT_TARGET_MONSTER, 99, true);
    check(BotRouteFailureTick(route, BOT_TARGET_MONSTER, 99, true),
          "coop monster uses the same 40-tic disengagement");

    auto facts = liveFacts();
    check(BotLivePlayerTarget(facts), "live enemy player target accepted");
    facts.health = 0;
    check(!BotLivePlayerTarget(facts), "corpse target rejected");
    facts = liveFacts(); facts.dead = 1;
    check(!BotLivePlayerTarget(facts), "dead_flag target rejected");
    facts = liveFacts(); facts.connected = 0;
    check(!BotLivePlayerTarget(facts), "disconnected target rejected");
    facts = liveFacts(); facts.spriteSeat = 3;
    check(!BotLivePlayerTarget(facts), "reused player sprite rejected");
    facts = liveFacts(); facts.teamGame = 1; facts.botTeam = facts.targetTeam = 1;
    check(!BotLivePlayerTarget(facts), "team target rejected");

    std::printf(g_fail ? "\nBOT LIFECYCLE TEST FAIL (%d)\n"
                       : "\nBOT LIFECYCLE TEST OK\n", g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
