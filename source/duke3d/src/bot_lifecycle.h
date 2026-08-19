#pragma once

#include <stdint.h>

// Draw-free policy primitives shared by the bot brain and its native unit gate.
// Storage and world access stay private to oldnet.cpp.
enum BotCombatTargetKind : int8_t
{
    BOT_TARGET_NONE    = 0,
    BOT_TARGET_PLAYER  = 1,
    BOT_TARGET_MONSTER = 2,
};

struct BotRouteFailureState
{
    int16_t target = -1;
    int16_t failedTics = 0;
    int8_t kind = BOT_TARGET_NONE;
    int8_t result = 0; // 0 unknown, 1 route, -1 no route
};

static inline void BotRouteStoreResult(BotRouteFailureState &state, int kind,
                                       int target, bool routed)
{
    if (kind == BOT_TARGET_NONE || target < 0)
    {
        state = {};
        state.target = -1;
        return;
    }

    if (state.kind != kind || state.target != target)
        state.failedTics = 0;
    state.kind = (int8_t)kind;
    state.target = (int16_t)target;
    state.result = routed ? 1 : -1;
}

// Call exactly once per generated bot input tic. Returns true on and after the
// 40th consecutive failed tic for the current, still-live target.
static inline bool BotRouteFailureTick(BotRouteFailureState &state, int kind,
                                       int target, bool targetLive)
{
    if (!targetLive || state.kind != kind || state.target != target)
    {
        BotRouteStoreResult(state, BOT_TARGET_NONE, -1, false);
        return false;
    }
    if (state.result > 0)
        state.failedTics = 0;
    else if (state.result < 0 && state.failedTics < INT16_MAX)
        state.failedTics++;
    return state.result < 0 && state.failedTics >= 40;
}

struct BotPlayerTargetFacts
{
    int botSeat;
    int targetSeat;
    int maxPlayers;
    int connected;
    int hasPlayer;
    int spriteIndex;
    int maxSprites;
    int sectorIndex;
    int numSectors;
    int dead;
    int spriteStatus;
    int maxStatus;
    int spritePicnum;
    int playerPicnum;
    int spriteSeat;
    int health;
    int teamGame;
    int botTeam;
    int targetTeam;
};

static inline bool BotLivePlayerTarget(BotPlayerTargetFacts const &f)
{
    return (unsigned)f.botSeat < (unsigned)f.maxPlayers
        && (unsigned)f.targetSeat < (unsigned)f.maxPlayers
        && f.targetSeat != f.botSeat && f.connected && f.hasPlayer
        && (unsigned)f.spriteIndex < (unsigned)f.maxSprites
        && (unsigned)f.sectorIndex < (unsigned)f.numSectors && !f.dead
        && f.spriteStatus < f.maxStatus && f.spritePicnum == f.playerPicnum
        && f.spriteSeat == f.targetSeat && f.health > 0
        && (!f.teamGame || f.targetTeam != f.botTeam);
}
