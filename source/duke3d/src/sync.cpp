//-------------------------------------------------------------------------
/*
Copyright (C) 1997, 2005 - 3D Realms Entertainment

This file is part of Shadow Warrior version 1.2

Shadow Warrior is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

Original Source: 1997 - Frank Maddin and Jim Norwood
Prepared for public release: 03/28/2005 - Charlie Wiederhold, 3D Realms
*/
//-------------------------------------------------------------------------

#include "duke3d.h"
#include "soundefs.h"  // DUKE_KILLED2 (desync alert sound)

extern "C" void Web_NetProbe(void);   // oldnet.cpp: world-truth probe line
#ifdef __EMSCRIPTEN__
# include <emscripten.h>
#endif

char g_szfirstSyncMsg[MAX_SYNC_TYPES][60];

static int crctable[256];
#define updatecrc(dcrc,xz) (dcrc = (crctable[((dcrc)>>8)^((xz)&255)]^((dcrc)<<8)))

int8_t syncData[MOVEFIFOSIZ][MAX_SYNC_TYPES];
bool syncError[MAX_SYNC_TYPES];

// Raw divergence detector, independent of the display path (Net_DisplaySyncMsg
// sets g_foundSyncError but was never wired into this fork's render loop).
int Net_SyncErrorDetected(void)
{
    // Stream mode (state authority): divergence is not a fault condition --
    // the host stream repaints it continuously. Nothing may act on verdicts.
    if (g_netStreamMode)
        return 0;
    for (int32_t i = 0; i < MAX_SYNC_TYPES; i++)
        if (syncError[i])
            return 1;
    return 0;
}

// Per-sender compared-tic ring: multi-stamp batches arrive on an UNORDERED
// channel, so a monotonic "last compared tic" cursor silently discards any
// batch that arrives behind a newer one -- exactly the coverage holes that hid
// fork birth tics. Each tic is compared at most once, in ANY arrival order.
static int32_t s_cmpTic[MAXPLAYERS][MOVEFIFOSIZ];

// Which tic each LOCAL syncData slot was actually stamped for. A remote stamp
// may only be compared against a slot stamped for the SAME tic -- comparing
// against an unstamped (post-ClearFIFO zeroed) or lapped slot produced
// all-category "local=0" storms that fired needless auto-resyncs (soak-caught
// at tic 88: every category local=0, own input column zeroed, remote sane).
int32_t g_syncStampTic[MOVEFIFOSIZ];

// Comparison floor: no stamp below this tic is ever compared. The old check
// was absolute (tickNum < 30) -- correct only for a tic-0 launch. Every OTHER
// racy window this grace exists for starts mid-timeline (late-join catchup at
// snapshotPlc, post-heal reload, the joiner's seat) where tics are in the
// thousands and the absolute check was dead code: settle artifacts flagged
// instantly. Reset sites raise the floor to "now + 1s of tics".
int32_t g_netCompareFloorTic = 30;

// PERSISTENCE GATE: a verdict must be proportionate to the evidence. The sim
// produces occasional ONE-TIC flickers (something crosses a tic boundary out
// of phase, then settles -- paired STATDUMPs prove the worlds equal at rest)
// and a single flagged tic used to latch the full alarm/report/heal chain: a
// 7-second reload cure for a self-healing hiccup, in a loop. A REAL fork
// flags every compared tic, so score +3 per flagged tic, -1 per clean tic,
// and only latch the verdict at 30 (~10 net flagged tics ~ 0.5s of genuine
// divergence; isolated flickers decay to zero). Cats seen while the score is
// nonzero accumulate so the eventual latch names every diverging category.
static int32_t  s_flagScore;
static uint32_t s_flagCatMask;
enum { SYNC_LATCH_SCORE = 30 };

// Clear the divergence verdict (auto-resync: the host just pushed an
// authoritative snapshot and every peer reloaded identical state).
void Net_ResetSyncCheck(void)
{
    g_foundSyncError = false;
    Bmemset(syncError, 0, sizeof(syncError));
    Bmemset(g_szfirstSyncMsg, 0, sizeof(g_szfirstSyncMsg));
    Bmemset(desynched_players, 0, sizeof(desynched_players));
    for (auto &row : s_cmpTic)
        for (auto &t : row)
            t = -1;
    for (auto &t : g_syncStampTic)
        t = -1;
    g_netCompareFloorTic = movefifoplc + 30;
    s_flagScore   = 0;
    s_flagCatMask = 0;
}
bool g_foundSyncError = false;

void initsynccrc(void)
{
    int i, j, k, a;

    for (j=0;j<256;j++)     //Calculate CRC table
    {
        k = (j<<8); a = 0;
        for (i=7;i>=0;i--)
        {
            if (((k^a)&0x8000) > 0)
                a = ((a<<1)&65535) ^ 0x1021;   //0x1021 = genpoly
            else
                a = ((a<<1)&65535);
            k = ((k<<1)&65535);
        }
        crctable[j] = (a&65535);
    }
}

static char Sync_PlayerPos(void)
{
    unsigned short crc = 0;

    // DESIGN (user): the direction a player is FACING is not part of desync
    // detection -- q16ang/q16horiz/sprite ang are still simulation state
    // (applied from inputs identically everywhere, and soft corrections
    // align them), but drift there must never latch a verdict. Divergence
    // that matters shows up in what facing CAUSES: positions, RNG draws,
    // damage -- all still hashed.
    for (int32_t ALL_PLAYERS(i))
    {
        auto pp = g_player[i].ps;
        updatecrc(crc, pp->pos.x);
        updatecrc(crc, pp->pos.y);
        updatecrc(crc, pp->pos.z);
        updatecrc(crc, sprite[pp->i].x);
        updatecrc(crc, sprite[pp->i].y);
        updatecrc(crc, sprite[pp->i].z);
    }

    return ((char) crc & 255);
}

static char Sync_PlayerData(void)
{
    unsigned short crc = 0;

    for(int32_t ALL_PLAYERS(i))
    {
        auto pp = g_player[i].ps;
        updatecrc(crc, pp->curr_weapon);
        updatecrc(crc, pp->kickback_pic);
        updatecrc(crc, pp->bobcounter);
        updatecrc(crc, pp->jetpack_on);
        updatecrc(crc, sprite[pp->i].extra);
    }

    return ((char) crc & 255);
}

static char Sync_Actors(void)
{
    unsigned short crc = 0;
    int j;

    for (SPRITES_OF(STAT_ACTOR, j))
    {
        spritetype* spr = &sprite[j];
        updatecrc(crc, spr->x);
        updatecrc(crc, spr->y);
        updatecrc(crc, spr->z);
        updatecrc(crc, spr->lotag);
        updatecrc(crc, spr->hitag);
        updatecrc(crc, spr->ang);
        updatecrc(crc, spr->owner);
    }

    for (SPRITES_OF(STAT_ZOMBIEACTOR, j))
    {
        spritetype* spr = &sprite[j];
        updatecrc(crc, spr->x);
        updatecrc(crc, spr->y);
        updatecrc(crc, spr->z);
        updatecrc(crc, spr->lotag);
        updatecrc(crc, spr->hitag);
        updatecrc(crc, spr->ang);
    }

    return ((char) crc & 255);
}

static char Sync_Projectiles(void)
{
    unsigned short crc = 0;
    int j;
    spritetype *spr;

    for (SPRITES_OF(STAT_PROJECTILE, j))
    {
        spr = &sprite[j];
        updatecrc(crc, spr->x);
        updatecrc(crc, spr->y);
        updatecrc(crc, spr->z);
        updatecrc(crc, spr->ang);
    }

    return ((char) crc & 255);
}

static char Sync_Map(void)
{
    unsigned short crc = 0;
    int j;

    for (SPRITES_OF(STAT_EFFECTOR, j))
    {
        spritetype* spr = &sprite[j];
        updatecrc(crc, spr->x);
        updatecrc(crc, spr->y);
        updatecrc(crc, spr->z);
        updatecrc(crc, spr->ang);
        updatecrc(crc, spr->lotag);
        updatecrc(crc, spr->hitag);
        updatecrc(crc, spr->owner);
        updatecrc(crc, spr->sectnum);
    }

    for (j = 0; j < numwalls; j++)
    {
        walltype* wal = &wall[j];
        updatecrc(crc, wal->x);
        updatecrc(crc, wal->y);
    }

    for (j = 0; j < numsectors; j++)
    {
        sectortype* sect = &sector[j];
        updatecrc(crc, sect->floorz);
        updatecrc(crc, sect->ceilingz);
    }

    return ((char) crc & 255);
}

static char Sync_Random(void)
{
    unsigned short crc = 0;

    updatecrc(crc, randomseed);
    updatecrc(crc, (randomseed >> 8));

    updatecrc(crc, g_globalRandom);
    updatecrc(crc, (g_globalRandom >> 8));

    // [NetDuke32 port] netduke32's separate g_random stream (global/playerweapon)
    // does not exist in this tree; randomseed above is this tree's sole RNG state.

    return ((char) crc & 255);
}

static char Sync_Engine(void)
{
    unsigned short crc = 0;

    updatecrc(crc, Numsprites);
    updatecrc(crc, (Numsprites >> 8));

    updatecrc(crc, tailspritefree);
    updatecrc(crc, (tailspritefree >> 8));

    updatecrc(crc, numwalls);
    updatecrc(crc, (numwalls >> 8));

    updatecrc(crc, numsectors);
    updatecrc(crc, (numsectors >> 8));

    return ((char)crc & 255);
}

#if 0
static char Sync_GameSettings(void)
{
    unsigned short crc = 0;

    updatecrc(crc, ud.dmflags);
    updatecrc(crc, (ud.dmflags >> 8));
    updatecrc(crc, ud.limit_hit);

    return ((char)crc & 255);
}
#endif

#if BOT_DEBUG == 1
static char Sync_Bots(void)
{
    unsigned short crc = 0;

    for (int32_t ALL_PLAYERS(i))
    {
        updatecrc(crc, botAI[i].cycleCount);
        updatecrc(crc, botAI[i].moveCount);
        updatecrc(crc, botAI[i].goalPos.x);
        updatecrc(crc, botAI[i].goalPos.y);
        updatecrc(crc, botAI[i].goalPos.z);
        updatecrc(crc, botAI[i].goalSprite);
        updatecrc(crc, botAI[i].goalWall);
        updatecrc(crc, botAI[i].rand);
    }

    return ((char)crc & 255);
}
#endif

// DEBUG bisection of Sync_PlayerPos (desync validation): one category per
// field group, so a mismatch names the diverging FIELD. Temporary; remove
// with the rest of the validation instrumentation.
static char Sync_DbgPos(void)
{
    unsigned short crc = 0;
    for (int32_t ALL_PLAYERS(i))
    {
        auto pp = g_player[i].ps;
        updatecrc(crc, pp->pos.x); updatecrc(crc, pp->pos.y); updatecrc(crc, pp->pos.z);
    }
    return ((char)crc & 255);
}
static char Sync_DbgAng(void)
{
    unsigned short crc = 0;
    for (int32_t ALL_PLAYERS(i))
        updatecrc(crc, g_player[i].ps->q16ang);
    return ((char)crc & 255);
}
static char Sync_DbgHoriz(void)
{
    unsigned short crc = 0;
    for (int32_t ALL_PLAYERS(i))
        updatecrc(crc, g_player[i].ps->q16horiz);
    return ((char)crc & 255);
}
static char Sync_DbgSprite(void)
{
    unsigned short crc = 0;
    // Player sprite ang deliberately excluded: facing is not a desync (see
    // Sync_PlayerPos).
    for (int32_t ALL_PLAYERS(i))
    {
        auto pp = g_player[i].ps;
        updatecrc(crc, sprite[pp->i].x); updatecrc(crc, sprite[pp->i].y);
        updatecrc(crc, sprite[pp->i].z);
    }
    return ((char)crc & 255);
}
// The INPUT each sim consumed for the tic that Net_GetSyncStat is stamping
// (movefifoplc was already incremented: the consumed tic is plc-1). If these
// categories flag, the peers consumed DIFFERENT INPUT BYTES for the same tic
// (a wire/fifo bug); if they stay clean while world categories fork, the sim
// itself is nondeterministic somewhere.
static char Sync_DbgInputHash(int plr)
{
    unsigned short crc = 0;
    uint8_t const *b = (uint8_t const *)&inputfifo[(movefifoplc - 1) & (MOVEFIFOSIZ - 1)][plr];
    for (unsigned k = 0; k < sizeof(input_t); k++)
        updatecrc(crc, b[k]);
    return ((char)crc & 255);
}
static char Sync_DbgInp0(void) { return Sync_DbgInputHash(0); }
static char Sync_DbgInp1(void) { return Sync_DbgInputHash(1); }

// Per-tic sprite spawn/delete accounting (fed from A_InsertSprite /
// A_DeleteSprite; reset after each stamp). Numsprites diverges IN-TIC at the
// residual root fork, so the tic where these counts differ -- and the picnum
// ring dumped at the mismatch -- names the one-peer-only spawner directly.
int32_t g_dbgInsCount, g_dbgDelCount;
int16_t g_dbgInsRing[32], g_dbgInsOwnRing[32];
int32_t g_dbgInsRingN;
int16_t g_dbgDelRing[32];   // picnums of this tic's DELETED sprites
int32_t g_dbgDelRingN;
// [tic & mask][0]=insert count (capped 6), [1]=delete count, [2..7]=picnums,
// [8..13]=spawner (owner) picnums, [14..16]=deleted picnums
static int16_t g_dbgSpawnHist[MOVEFIFOSIZ][18];
static char Sync_DbgSpawnCount(void)
{
    unsigned short crc = 0;
    updatecrc(crc, g_dbgInsCount); updatecrc(crc, g_dbgDelCount);
    return ((char)crc & 255);
}
static char Sync_DbgSpawnPicnums(void)
{
    unsigned short crc = 0;
    int const n = min(g_dbgInsRingN, 32);
    for (int k = 0; k < n; k++)
    { updatecrc(crc, g_dbgInsRing[k]); updatecrc(crc, (g_dbgInsRing[k] >> 8)); }
    return ((char)crc & 255);
}

// Per-tic EXECUTION GATES. G_DoMoveThings advances movefifoplc unconditionally
// but gates the world step (krand draw, P_ProcessInput, G_MoveWorld) on
// ud.pause_on -- so any per-peer asymmetry in these gates silently shifts every
// actor's execution phase against the tic counter (residual-fork suspect: the
// DUKECAR's pData[0] drifted 2 tics on one peer inside the first 30 tics).
// This category flags the exact tic a gate ever differs.
static char Sync_DbgGates(void)
{
    unsigned short crc = 0;
    updatecrc(crc, ud.pause_on);
    updatecrc(crc, numplayers);
    updatecrc(crc, (char)(g_player[connecthead].ps ? (g_player[connecthead].ps->gm & 0xff) : 0xee));
    return ((char)crc & 255);
}

// Per-tic krand() DRAW COUNT (reset after each stamp, incremented inside krand
// itself -- random.h). A count mismatch at the fork tic names "one peer ran an
// extra RNG consumer HERE"; equal counts with a diverged randomseed (cat 1)
// mean the state feeding the draws forked earlier. Frame-rate callers (render
// code polluting the sim stream) show up as constant flake of this category.
static char Sync_DbgKrand(void)
{
    return (char)(g_krandCalls & 255);
}

// Cumulative world-step executions (game.cpp increments inside the pause gate).
// Divergence = one peer skipped a world tic the other ran: flags from the first
// skip onward and never re-converges, naming the exact birth tic.
int32_t g_worldExecs;
static char Sync_DbgWorldExec(void)
{
    return (char)(g_worldExecs & 255);
}

// DEEP WORLD hashes: the observed forks are born in state none of the classic
// categories cover (zombie wake counters first of all -- timetosleep drifts
// invisibly for tens of tics before a wake threshold makes it visible as
// "different krand draws"). With gap-free stamp streaming, the FIRST of these
// to flag names the subsystem and the exact birth tic.
static char Sync_DbgZombies(void)
{
    unsigned short crc = 0;
    int guard = 0;
    for (int i = headspritestat[STAT_ZOMBIEACTOR]; i >= 0 && guard < MAXSPRITES; i = nextspritestat[i], guard++)
    {
        updatecrc(crc, i);
        updatecrc(crc, actor[i].timetosleep);
        updatecrc(crc, (actor[i].timetosleep >> 8));
        updatecrc(crc, actor[i].t_data[0]);
    }
    return ((char)crc & 255);
}
// STAT_FX (MUSICANDSFX) is EXCLUDED from the deep hashes: its t_data is
// per-viewer audio state BY DESIGN (ambient start/stop keys on the local
// screenpeek player's distance) and its divergence cannot cascade into the
// world -- hashing it buried the real signals under permanent tic-30 noise.
static char Sync_DbgAllPos(void)
{
    unsigned short crc = 0;
    int guard = 0;
    for (int st = 0; st < MAXSTATUS; st++)
    {
        if (st == STAT_FX)
            continue;
        for (int i = headspritestat[st]; i >= 0 && guard < MAXSPRITES; i = nextspritestat[i], guard++)
        {
            updatecrc(crc, sprite[i].x); updatecrc(crc, (sprite[i].x >> 8));
            updatecrc(crc, sprite[i].y); updatecrc(crc, (sprite[i].y >> 8));
            updatecrc(crc, sprite[i].z); updatecrc(crc, (sprite[i].z >> 8));
        }
    }
    return ((char)crc & 255);
}
static char Sync_DbgAllActor(void)
{
    unsigned short crc = 0;
    int guard = 0;
    for (int st = 0; st < MAXSTATUS; st++)
    {
        if (st == STAT_FX)
            continue;
        for (int i = headspritestat[st]; i >= 0 && guard < MAXSPRITES; i = nextspritestat[i], guard++)
        {
            updatecrc(crc, actor[i].htextra);
            updatecrc(crc, actor[i].htowner);
            updatecrc(crc, actor[i].t_data[0]);
            updatecrc(crc, actor[i].t_data[4]);
        }
    }
    return ((char)crc & 255);
}

// Per-statnum sub-hash (positions + actor fields), dumped when cat 18/19
// mismatches: diffing the two peers' STATDUMP lines names the diverging
// statnum in one run.
static void Sync_DumpStatHashes(int32_t tickNum)
{
#if defined(__EMSCRIPTEN__)
    int h[16];
    int guard = 0;
    for (int st = 0; st < 16; st++)
    {
        unsigned short crc = 0;
        if (st != STAT_FX)
            for (int i = headspritestat[st]; i >= 0 && guard < MAXSPRITES; i = nextspritestat[i], guard++)
            {
                updatecrc(crc, sprite[i].x); updatecrc(crc, sprite[i].y); updatecrc(crc, sprite[i].z);
                updatecrc(crc, actor[i].htextra); updatecrc(crc, actor[i].t_data[0]);
            }
        h[st] = (int)(crc & 255);
    }
    EM_ASM({ console.log('[eng] STATDUMP tic=' + $0 + ' lo=[' + $1 + ',' + $2 + ',' + $3 + ',' + $4 + ',' + $5 + ',' + $6 + ',' + $7 + ',' + $8 + ']'); },
           tickNum, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    EM_ASM({ console.log('[eng] STATDUMP tic=' + $0 + ' hi=[' + $1 + ',' + $2 + ',' + $3 + ',' + $4 + ',' + $5 + ',' + $6 + ',' + $7 + ',' + $8 + ']'); },
           tickNum, h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
#else
    (void)tickNum;
#endif
}

// RNG TRACE ring history (krand_traced lives in engine.cpp via random.h's
// engine_c_ block; g_krandSiteRing holds the CURRENT tic's first sites).
static const char *s_krandSiteHist[MOVEFIFOSIZ][6];
static int16_t     s_krandSprHist[MOVEFIFOSIZ][6];   // sprite per recorded site

// This must not exceed MAX_SYNC_TYPES
static SyncType_t syncType[] = {
    DEFINE_SYNCFUNC(Sync_Engine),
    DEFINE_SYNCFUNC(Sync_Random),
    DEFINE_SYNCFUNC(Sync_PlayerPos),
    DEFINE_SYNCFUNC(Sync_PlayerData),
    DEFINE_SYNCFUNC(Sync_Projectiles),
    DEFINE_SYNCFUNC(Sync_Actors),
    DEFINE_SYNCFUNC(Sync_Map),
    DEFINE_SYNCFUNC(Sync_DbgPos),     // 7
    DEFINE_SYNCFUNC(Sync_DbgAng),     // 8
    DEFINE_SYNCFUNC(Sync_DbgHoriz),   // 9
    DEFINE_SYNCFUNC(Sync_DbgSprite),  // 10
    DEFINE_SYNCFUNC(Sync_DbgInp0),    // 11  consumed input, player 0
    DEFINE_SYNCFUNC(Sync_DbgInp1),    // 12  consumed input, player 1
    DEFINE_SYNCFUNC(Sync_DbgSpawnCount),   // 13  per-tic insert/delete counts
    DEFINE_SYNCFUNC(Sync_DbgSpawnPicnums), // 14  per-tic inserted picnums
    DEFINE_SYNCFUNC(Sync_DbgGates),        // 15  pause/gm execution gates
    DEFINE_SYNCFUNC(Sync_DbgKrand),        // 16  per-tic krand draw count
    DEFINE_SYNCFUNC(Sync_DbgZombies),      // 17  zombie wake bookkeeping (timetosleep)
    DEFINE_SYNCFUNC(Sync_DbgAllPos),       // 18  every live sprite's position
    DEFINE_SYNCFUNC(Sync_DbgAllActor),     // 19  every live actor's damage/timers
    DEFINE_SYNCFUNC(Sync_DbgWorldExec),    // 20  cumulative world-step executions
#if 0
    DEFINE_SYNCFUNC(Sync_GameSettings),
#endif
#if BOT_DEBUG == 1
    DEFINE_SYNCFUNC(Sync_Bots),
#endif
};
#define NUM_SYNC_TYPES (int32_t)(sizeof(syncType) / sizeof(syncType[0]))

void Net_GetSyncStat(void)
{
    if (numplayers < 2)
        return;

    // Stream mode: lockstep CRC stamps exist only to flag divergence for the
    // repair ladder, which is retired -- skip the whole per-tic CRC walk
    // (allactor/world hashing, real CPU every consumed tic). Forensics keeps
    // the stamps flowing so pair probes can still MEASURE divergence extent.
    {
        extern int32_t g_netForensics;
        if (g_netStreamMode && !g_netForensics)
            return;
        // Tic-aligned world-truth line (see Web_NetProbe): every peer prints
        // at the SAME plc values, so the pair differ compares the same sim
        // tic exactly -- no wall-clock pairing slack.
        if (g_netForensics && (movefifoplc & 255) == 0)
        {
            static int32_t s_probePlc = -1;
            if (movefifoplc != s_probePlc)
            {
                s_probePlc = movefifoplc;
                Web_NetProbe();
            }
        }
    }

    // The CRC table was NEVER initialized in this port -- classic called
    // initsynccrc() from game init, and the call was lost. With a zero table,
    // updatecrc() computes 0^(0<<8)=0 forever: every category CRC was a
    // CONSTANT ZERO on every peer, so the watchdog compared zeros against
    // zeros and could not flag anything (forced-desync soak caught it: a
    // deliberately forked world produced 1100+ "clean" comparisons). Lazy
    // init here is order-proof.
    static bool s_crcInited;
    if (!s_crcInited)
    {
        s_crcInited = true;
        initsynccrc();
    }

    for (int32_t i = 0; i < NUM_SYNC_TYPES; i++)
        syncData[INPUTFIFO_CURTICK][i] = syncType[i].func();
    g_syncStampTic[INPUTFIFO_CURTICK] = movefifoplc;

    // spawn accounting window = one consumed tic (stamped above). Keep a
    // per-tic history so the mismatch handler (which runs when the compare
    // packet arrives, tics later) can still dump the FORK tic's picnums.
    {
        int32_t const slot = INPUTFIFO_CURTICK;
        g_dbgSpawnHist[slot][0] = (int16_t)min(g_dbgInsRingN, 6);
        g_dbgSpawnHist[slot][1] = (int16_t)g_dbgDelCount;
        for (int k = 0; k < 6; k++)
        {
            g_dbgSpawnHist[slot][2 + k] = (k < g_dbgInsRingN && k < 32) ? g_dbgInsRing[k] : -1;
            g_dbgSpawnHist[slot][8 + k] = (k < g_dbgInsRingN && k < 32) ? g_dbgInsOwnRing[k] : -1;
        }
        for (int k = 0; k < 3; k++)
            g_dbgSpawnHist[slot][14 + k] = (k < g_dbgDelRingN && k < 32) ? g_dbgDelRing[k] : -1;
        for (int k = 0; k < 6; k++)
        {
            s_krandSiteHist[slot][k] = (k < g_krandCalls && k < 8) ? g_krandSiteRing[k] : NULL;
            s_krandSprHist[slot][k]  = (k < g_krandCalls && k < 8) ? g_krandSiteSpr[k] : -1;
        }
    }
    g_dbgInsCount = g_dbgDelCount = 0;
    g_dbgInsRingN = g_dbgDelRingN = 0;
    g_krandCalls  = 0;

#if defined(__EMSCRIPTEN__)
    // DEBUG (desync validation): prove the ring is being FILLED, with what, at
    // which slot. Throttled to one line per 64 tics.
    if ((movefifoplc & 63) == 0)
        EM_ASM({ console.log('[eng] syncstat plc=' + $0 + ' slot=' + $1 + ' pp=' + $2 + ' map=' + $3); },
               movefifoplc, INPUTFIFO_CURTICK,
               (int)(uint8_t)syncData[INPUTFIFO_CURTICK][2],
               (int)(uint8_t)syncData[INPUTFIFO_CURTICK][6]);
    // Forensics: EVERY peer dumps per-statnum hashes AND the tic's krand call
    // sites on the same cadence, so cross-peer same-tic diffs name a diverging
    // statnum or RNG caller without waiting for a detector (detection moments
    // never coincide across peers -- measured: zero common dump tics).
    {
        extern int32_t g_netForensics;
        if (g_netForensics && (movefifoplc & 63) == 0)
        {
            Sync_DumpStatHashes(movefifoplc);
            int const slot = INPUTFIFO_CURTICK;
            char buf[512]; int n = 0;
            for (int kk = 0; kk < 6 && s_krandSiteHist[slot][kk] != NULL && n < 440; kk++)
                n += Bsnprintf(buf + n, sizeof(buf) - n, "%s%s#%d", kk ? "," : "",
                               s_krandSiteHist[slot][kk], (int)s_krandSprHist[slot][kk]);
            buf[n] = 0;
            EM_ASM({ console.log('[eng] RNGCAD tic=' + $0 + ' awc=' + $1 + ' sites=' + UTF8ToString($2)); },
                   movefifoplc, g_animWallCnt, buf);
        }
    }
#endif
}

////////////////////////////////////////////////////////////////////////
//
// Sync Message print
//
////////////////////////////////////////////////////////////////////////

int desynched_players[MAXPLAYERS];
int32_t g_netSyncCompares = 0;

void Net_DisplaySyncMsg(void)
{
    static unsigned int moveCount = 0;
    extern unsigned int g_moveThingsCount;

    if (numplayers < 2)
        return;

    // A watcher (joiner/healing guest mid-catchup) has nothing to say about
    // sync -- and nothing should scream at it while it loads in. The catchup
    // progress line lives in screens.cpp.
    if (g_netJoinCatchup)
        return;

    // The red verdict wall (FIRST Out Of Sync / DESYNCHED) is a FORENSIC
    // surface. Players never see it: divergence is handled by the correction
    // ladder (soft state snap -> snapshot heal) and screaming about it was
    // pure alarm with no action the player could take (live feedback: "it
    // regularly thinks things are out of sync"). Web_SetForensics(1) re-arms
    // it for hunts, alarm sound included.
    {
        extern int32_t g_netForensics;
        if (!g_netForensics)
            return;
    }

    for (int32_t i = 0; i < NUM_SYNC_TYPES; i++)
    {
        // syncError is NON 0 - out of sync
        if (syncError[i])
        {
            sprintf(tempbuf, "Out Of Sync - %s", syncType[i].name);
            printext256(4, 100 + (i * 8), 31, 1, tempbuf, 0);

            if (!g_foundSyncError && g_szfirstSyncMsg[i][0] == '\0')
            {
                S_PlaySound(DUKE_KILLED2);

                // g_foundSyncError one so test all of them and then never test again
                g_foundSyncError = true;

                // save off loop count
                moveCount = g_moveThingsCount;

                for (int32_t j = 0; j < NUM_SYNC_TYPES; j++)
                {
                    if (syncError[j] && g_szfirstSyncMsg[j][0] == '\0')
                    {
                        sprintf(tempbuf, "Out Of Sync - %s", syncType[j].name);
                        strcpy(g_szfirstSyncMsg[j], tempbuf);
                    }
                }
            }
        }
    }

    // print out the g_szfirstSyncMsg message you got
    for (int32_t i = 0; i < NUM_SYNC_TYPES; i++)
    {
        if (g_szfirstSyncMsg[i][0] != '\0')
        {
            sprintf(tempbuf, "FIRST %s", g_szfirstSyncMsg[i]);
            printext256(4, 44 + (i * 8), 31, 1, tempbuf, 0);
            sprintf(tempbuf, "moveCount %d",moveCount);
            printext256(4, 52 + (i * 8), 31, 1, tempbuf, 0);

            // Stock code reused `i` here. Latent for decades: with NUM_SYNC_TYPES
            // <= MAXPLAYERS the clobber merely ended the outer loop early. The
            // 5o deep categories (16-20) armed it: a latched message at index
            // >= MAXPLAYERS resets i to 16 forever -> INFINITE LOOP in the
            // render path = the whole wasm main loop wedges (browser tab
            // freezes) on the first real deep-cat divergence.
            int ypos = 180;
            for (int32_t p = 0; p < MAXPLAYERS; p++)
            {
                if (desynched_players[p] == 1)
                {
                    Bsprintf(tempbuf, "DESYNCHED: %s (IDX: %d)", g_player[p].user_name, p);
                    printext256(4, ypos, 31, 0, tempbuf, 0);
                    ypos += 8;
                }
            }
        }
    }
}

// MULTI-STAMP sync block: [count u8][count x (tic i32 + NUM_SYNC_TYPES bytes)].
// The old one-stamp-per-packet form only ever sent the CURRENT tic's stamp:
// any burst of consumed tics (echo batches, catchup fast-forwards) left holes
// in compare coverage, so a fork's BIRTH tic was routinely never compared and
// the first visible mismatch was a many-categories-at-once storm tics later.
// Now every consumed tic's stamp ships exactly once (cursor = own lastSyncTick),
// capped per packet; the receiver walks them all. Slot semantics, both roles:
// syncData[T] is written when movefifoplc becomes T = state AFTER consuming
// tic T-1; label is T. (The old role-split labeling is gone.)
void Net_AddSyncInfoToPacket(int *j)
{
    auto p = &g_player[myconnectindex];
    int const countPos = (*j)++;
    packbuf[countPos] = 0;

    if (g_gameQuit || numplayers < 2)
        return;

    // WATCHER CONTRACT: a catchup peer ships no stamps -- its replay is not
    // evidence about the live match, and stale catchup stamps arriving after
    // the seat would compare against the wrong generation. Pinning the cursor
    // means only tics stamped AFTER it goes live ever leave this machine.
    if (g_netJoinCatchup)
    {
        p->lastSyncTick = movefifoplc;
        return;
    }

    int32_t const newest = movefifoplc;
    int32_t from = p->lastSyncTick + 1;
    if (from < newest - 15)
        from = newest - 15;                    // per-packet cap (oldest dropped)
    if (from < newest - (MOVEFIFOSIZ - 8))
        from = newest - (MOVEFIFOSIZ - 8);     // ring-reuse safety
    if (from > newest)
        return;

    int n = 0;
    for (int32_t t = from; t <= newest && n < 16; t++, n++)
    {
        B_BUF32(&packbuf[(*j)], t);
        (*j) += sizeof(int32_t);
        Bmemcpy(&packbuf[(*j)], syncData[t & (MOVEFIFOSIZ - 1)], NUM_SYNC_TYPES);
        (*j) += NUM_SYNC_TYPES;
    }
    packbuf[countPos] = (char)n;
    p->lastSyncTick = newest;
}

extern int32_t g_netSyncCompares;
void Net_GetSyncInfoFromPacket(char *packbuf, int *j, int otherconnectindex)
{
    int const stampCount = (uint8_t)packbuf[(*j)++];

    for (int s = 0; s < stampCount; s++)
    {
    int32_t const tickNum = (int32_t)B_UNBUF32(&packbuf[(*j)]);
    (*j) += sizeof(int32_t);
    int const bytesPos = (*j);
    (*j) += NUM_SYNC_TYPES;   // ALWAYS consume the stamp bytes, even when skipping

    // if ready2send is not set, or player is disconnected then don't compare
    if (!ready2send || !g_player[otherconnectindex].connected || g_gameQuit)
        continue;

    // WATCHER CONTRACT: a peer that is not alive in the match -- a joiner or
    // healing guest mid-catchup -- is a spectator of the stream, not a sync
    // participant. Its world is a replay-in-progress; comparing it latched
    // "Out Of Sync" + the alarm on the joiner's screen before it ever played
    // (live-reported). It neither compares nor reports until it is seated.
    if (g_netJoinCatchup)
        continue;

    // Reject tics outside the live ring window: the move channel is unreliable/
    // unordered, so a badly delayed packet could otherwise compare a REMOTE tic
    // against a syncData slot that has been REUSED by a newer generation
    // (tickNum & (MOVEFIFOSIZ-1) aliases every MOVEFIFOSIZ tics) and latch a
    // false "Out Of Sync" that triggers a needless resync.
    // The compare floor is the settle grace (~1s of tics past the last reset,
    // seat, or heal): the moments right after a barrier/snapshot reload are
    // racy on purpose (rings refill, cursors reset, stale stamps drain) and
    // comparing there produced false flags that re-triggered the auto-resync
    // in a loop. The floor is RELATIVE -- the old absolute tickNum<30 check
    // only ever covered a tic-0 launch and was dead code for every
    // mid-timeline reload. Real divergence persists past the grace and flags.
    if (tickNum < g_netCompareFloorTic || (tickNum > movefifoplc)
        || tickNum <= movefifoplc - (MOVEFIFOSIZ - 8))
        continue;
    if (g_syncStampTic[tickNum & (MOVEFIFOSIZ - 1)] != tickNum)
        continue;   // local slot unstamped/lapped for this tic: nothing to compare
    if (s_cmpTic[otherconnectindex][tickNum & (MOVEFIFOSIZ - 1)] == tickNum)
        continue;   // already compared (resend/reorder)
    s_cmpTic[otherconnectindex][tickNum & (MOVEFIFOSIZ - 1)] = tickNum;

    g_netSyncCompares++; // debug surface: proves the CRC comparison actually runs

    // Grab sync info from packet buffer
    uint32_t tickMask = 0;
    for (int32_t sb = 0; sb < NUM_SYNC_TYPES; sb++)
    {
        char head_sync = syncData[tickNum & (MOVEFIFOSIZ - 1)][sb];
        char player_sync = packbuf[bytesPos + sb];

        if (player_sync != head_sync)
        {
            // Cats 0-15 are GAMEPLAY truth (world/players/inputs/spawns/gates)
            // and feed the verdict -- EXCEPT 8/9 (player q16ang/q16horiz):
            // facing direction is not a desync (user directive); they log
            // under forensics for hunts but never latch. Cats 16-20 are
            // FORENSIC deep-hashes -- they
            // sweep state no gameplay path reads (all-statnum positions include
            // decoration/limbo sprites; krand counts; cumulative counters) and
            // exist to NAME a fork for the hunt, not to prove one matters. A
            // known benign residual lives there: after a heal load, one sprite
            // runs a persistent one-tic phase lag (cat 18 flags every tic with
            // +/-tiny CRC deltas; paired STATDUMPs match at ADJACENT tics; all
            // gameplay cats stay clean for minutes). Latching on it produced a
            // 7s reload cure, in a loop, for a cosmetic hiccup. If the laggard
            // ever touches gameplay, cats 1-6/13/14 flag and the heal fires.
            if (sb <= 15 && sb != 8 && sb != 9)
                tickMask |= ((uint32_t)1 << sb);

#if defined(__EMSCRIPTEN__)
            // DEBUG (desync validation): the first few RAW mismatches, typed.
            // One category flaking = real divergence in that subsystem; ALL
            // categories at once = comparison misalignment.
            // g_netForensics gates the whole dump machinery (default OFF for
            // ship builds): guest renderers died repeatedly right at storm-time
            // dump bursts, and comparisons/auto-resync work without the spam.
            // The soak harness enables it via Web_SetForensics for hunts.
            {
                extern int32_t g_netForensics;
                static int s_worldLogs, s_inputLogs;
                bool const isInputCat = (sb >= 11);
                if (g_netForensics && (isInputCat ? (s_inputLogs < 24) : (s_worldLogs < 8)))
                {
                    (isInputCat ? s_inputLogs : s_worldLogs)++;
                    EM_ASM({ console.log('[eng] MISMATCH cat=' + $0 + ' tic=' + $1 + ' local=' + $2 + ' remote=' + $3 + ' plc=' + $4 + ' ep=' + $5); },
                           sb, tickNum, (int)(uint8_t)head_sync, (int)(uint8_t)player_sync, movefifoplc,
                           (int)g_netMoveEpoch);
                    // Input-hash mismatch: dump the LOCAL ring's raw fields for
                    // that tic, both players, so the two sides' logs can be
                    // diffed field-by-field. Throttled: a storm printing dozens
                    // of EM_ASM lines in one tic correlated with renderer
                    // deaths on the bench -- the data value is in the FIRST
                    // dumps anyway.
                    static int32_t s_lastInpDump = -1000;
                    if (isInputCat && tickNum - s_lastInpDump > 8)
                    {
                        s_lastInpDump = tickNum;
                        for (int plr = 0; plr < 2; plr++)
                        {
                            input_t const *in = &inputfifo[(tickNum - 1) & (MOVEFIFOSIZ - 1)][plr];
                            EM_ASM({ console.log('[eng] INPDUMP tic=' + $0 + ' p=' + $1 + ' fvel=' + $2 + ' svel=' + $3 + ' avel=' + $4 + ' horz=' + $5 + ' bits=0x' + ($6 >>> 0).toString(16) + ' ext=0x' + ($7 >>> 0).toString(16)); },
                                   tickNum - 1, plr, in->fvel, in->svel, (int)in->q16avel, (int)in->q16horz,
                                   (int)in->bits, (int)in->extbits);
                        }
                    }
                    // krand-count mismatch: dump the fork tic's local call
                    // sites -- diffing the peers' dumps names the asymmetric
                    // RNG consumer directly.
                    if (sb == 16)
                    {
                        const char *const *sites = s_krandSiteHist[tickNum & (MOVEFIFOSIZ - 1)];
                        EM_ASM({ console.log('[eng] RNGDUMP tic=' + $0 + ' n=' + $7 + ' sites='
                                             + ($1 ? UTF8ToString($1) : '')
                                             + ($2 ? ',' + UTF8ToString($2) : '')
                                             + ($3 ? ',' + UTF8ToString($3) : '')
                                             + ($4 ? ',' + UTF8ToString($4) : '')
                                             + ($5 ? ',' + UTF8ToString($5) : '')
                                             + ($6 ? ',' + UTF8ToString($6) : '')); },
                               tickNum, sites[0], sites[1], sites[2], sites[3], sites[4], sites[5],
                               (int)(uint8_t)syncData[tickNum & (MOVEFIFOSIZ - 1)][16]);
                    }
                    // Deep-hash mismatch: name the diverging statnum. (Runs on
                    // CURRENT state, not the fork tic's -- close enough while
                    // the divergence is fresh; the tic label ties dumps together.)
                    if (sb == 18 || sb == 19)
                    {
                        static int32_t s_lastStatDump = -1000;
                        if (tickNum - s_lastStatDump > 8)
                        {
                            s_lastStatDump = tickNum;
                            Sync_DumpStatHashes(tickNum);
                        }
                    }
                    // Spawn-count/picnum mismatch: dump the FORK tic's local
                    // spawn history -- the picnums name the one-peer spawner.
                    if (sb == 13 || sb == 14)
                    {
                        int16_t const *h = g_dbgSpawnHist[tickNum & (MOVEFIFOSIZ - 1)];
                        EM_ASM({ console.log('[eng] SPAWNDUMP tic=' + $0 + ' ins=' + $1 + ' del=' + $2 + ' pics=[' + $3 + ',' + $4 + ',' + $5 + '] by=[' + $6 + ',' + $7 + ',' + $8 + '] delpics=[' + $9 + ',' + $10 + ',' + $11 + ']'); },
                               tickNum, h[0], h[1], h[2], h[3], h[4], h[8], h[9], h[10], h[14], h[15], h[16]);
                    }
                }
            }
#endif
        }
    }

    // Persistence scoring (see s_flagScore): raw per-tic mismatches feed the
    // score; only a SUSTAINED run of flagged tics latches the verdict that
    // drives the UI, the guest's DESYNC_REPORT, and the host's targeted heal.
    if (tickMask)
    {
        s_flagCatMask |= tickMask;
        s_flagScore = min(s_flagScore + 3, SYNC_LATCH_SCORE * 2);
    }
    else if (s_flagScore > 0 && --s_flagScore == 0)
        s_flagCatMask = 0;

    if (s_flagScore >= SYNC_LATCH_SCORE)
    {
        for (int32_t sb = 0; sb < NUM_SYNC_TYPES; sb++)
        {
            if ((s_flagCatMask & ((uint32_t)1 << sb)) && !syncError[sb])
            {
                OSD_Printf("Desynchronized! Player: %d, Type: %s, TickNum: %d, MoveFifoPlc: %d\n",
                           otherconnectindex, syncType[sb].name, tickNum, movefifoplc);
                syncError[sb] = true;
            }
        }
        desynched_players[otherconnectindex] = 1;
    }
    }   // stamp loop
}
