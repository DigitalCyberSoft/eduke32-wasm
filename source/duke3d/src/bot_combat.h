#pragma once

#include "duke3d.h"

#ifdef NETDUKE32

enum BotShotKind : uint8_t
{
    BOT_SHOT_NONE,
    BOT_SHOT_HITSCAN,
    BOT_SHOT_LINEAR,
};

struct BotShotModel
{
    int16_t tile;
    int16_t speed;
    int16_t splash;
    BotShotKind kind;
};

static inline BotShotModel Bot_CombatStockShotModel(int const mappedTile, int const currentWeapon,
                                                     int const rpgRadius)
{
    BotShotModel out = { (int16_t)mappedTile, 0, 0, BOT_SHOT_NONE };

    switch (mappedTile)
    {
    case SHOTSPARK1__: case SHOTGUN__: case CHAINGUN__: case GROWSPARK__: case KNEE__:
        out.kind = BOT_SHOT_HITSCAN;
        break;
    case RPG__:
        out.speed = 644;
        out.splash = (int16_t)(currentWeapon == DEVISTATOR_WEAPON ? rpgRadius >> 1 : rpgRadius);
        out.kind = BOT_SHOT_LINEAR;
        break;
    case FREEZEBLAST__:
        out.speed = 644;
        out.kind = BOT_SHOT_LINEAR;
        break;
    case SHRINKSPARK__:
        out.speed = 768;
        out.kind = BOT_SHOT_LINEAR;
        break;
    case FIREBALL__:
        out.speed = 840;
        out.kind = BOT_SHOT_LINEAR;
        break;
    default:
        break;
    }
    return out;
}

static inline int32_t Bot_CombatLeadTicsFromDistance(int32_t const distance, int32_t const speed)
{
    return speed > 0 ? distance / speed : 0;
}

static inline int32_t Bot_CombatWantHorizon(int32_t const dz, int32_t const dist2d)
{
    return (dist2d > 256)
         ? clamp(100 - (int)(((int64_t)dz * 16) / max(dist2d, 256)), 60, 140)
         : 100;
}

static inline int32_t Bot_CombatHorizonDeltaUnits(int32_t const effectiveHoriz,
                                                  int32_t const dz, int32_t const dist2d)
{
    return clamp(Bot_CombatWantHorizon(dz, dist2d) - effectiveHoriz, -6, 6);
}

static inline fix16_t Bot_CombatHorizonDelta(fix16_t const q16horiz, fix16_t const q16horizoff,
                                             int32_t const dz, int32_t const dist2d)
{
    return F16(Bot_CombatHorizonDeltaUnits(fix16_to_int(fix16_sadd(q16horiz, q16horizoff)),
                                           dz, dist2d));
}

static inline bool Bot_CombatBreakFireAllowed(bool const hasCombatTarget, bool const exactBlocker,
                                              bool const safetyVeto, bool const throttleOK)
{
    return !hasCombatTarget && exactBlocker && !safetyVeto && throttleOK;
}

#endif
