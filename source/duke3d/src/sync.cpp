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
    for (int32_t i = 0; i < MAX_SYNC_TYPES; i++)
        if (syncError[i])
            return 1;
    return 0;
}

// Clear the divergence verdict (auto-resync: the host just pushed an
// authoritative snapshot and every peer reloaded identical state).
void Net_ResetSyncCheck(void)
{
    g_foundSyncError = false;
    Bmemset(syncError, 0, sizeof(syncError));
    Bmemset(g_szfirstSyncMsg, 0, sizeof(g_szfirstSyncMsg));
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

    for (int32_t ALL_PLAYERS(i))
    {
        auto pp = g_player[i].ps;
        updatecrc(crc, pp->pos.x);
        updatecrc(crc, pp->pos.y);
        updatecrc(crc, pp->pos.z);
        updatecrc(crc, pp->q16ang);
        updatecrc(crc, pp->q16horiz);
        updatecrc(crc, sprite[pp->i].x);
        updatecrc(crc, sprite[pp->i].y);
        updatecrc(crc, sprite[pp->i].z);
        updatecrc(crc, sprite[pp->i].ang);
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
    for (int32_t ALL_PLAYERS(i))
    {
        auto pp = g_player[i].ps;
        updatecrc(crc, sprite[pp->i].x); updatecrc(crc, sprite[pp->i].y);
        updatecrc(crc, sprite[pp->i].z); updatecrc(crc, sprite[pp->i].ang);
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

#if defined(__EMSCRIPTEN__)
    // DEBUG (desync validation): prove the ring is being FILLED, with what, at
    // which slot. Throttled to one line per 64 tics.
    if ((movefifoplc & 63) == 0)
        EM_ASM({ console.log('[eng] syncstat plc=' + $0 + ' slot=' + $1 + ' pp=' + $2 + ' map=' + $3); },
               movefifoplc, INPUTFIFO_CURTICK,
               (int)(uint8_t)syncData[INPUTFIFO_CURTICK][2],
               (int)(uint8_t)syncData[INPUTFIFO_CURTICK][6]);
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

            int ypos = 180;
            for (i = 0; i < MAXPLAYERS; i++)
            {
                if (desynched_players[i] == 1)
                {
                    Bsprintf(tempbuf, "DESYNCHED: %s (IDX: %d)", g_player[i].user_name, i);
                    printext256(4, ypos, 31, 0, tempbuf, 0);
                    ypos += 8;
                }
            }
        }
    }
}

void Net_AddSyncInfoToPacket(int *j)
{
    auto p = &g_player[myconnectindex];
    int32_t const tickNum = (myconnectindex == connecthead) ? movefifoplc-1 : movefifoplc;
    
    if(tickNum == p->lastSyncTick || g_gameQuit) // Already sent this one, or disconnecting.
    {
        B_BUF32(&packbuf[(*j)], -1);
        (*j) += sizeof(int32_t);
        return;
    }

    p->lastSyncTick = tickNum;
    B_BUF32(&packbuf[(*j)], tickNum);
    (*j) += sizeof(int32_t);

    for (int32_t sb = 0; sb < NUM_SYNC_TYPES; sb++)
    {
        packbuf[(*j)++] = syncData[tickNum & (MOVEFIFOSIZ-1)][sb];
    }
}

extern int32_t g_netSyncCompares;
void Net_GetSyncInfoFromPacket(char *packbuf, int *j, int otherconnectindex)
{
    // if ready2send is not set, or player is disconnected then don't try to get sync info
#if 1
    if (!ready2send || !g_player[otherconnectindex].connected || g_gameQuit)
        return;
#endif

    int32_t const tickNum = (int32_t)B_UNBUF32(&packbuf[(*j)]);
    (*j) += sizeof(int32_t);

    // Reject tics outside the live ring window: the move channel is unreliable/
    // unordered, so a badly delayed packet could otherwise compare a REMOTE tic
    // against a syncData slot that has been REUSED by a newer generation
    // (tickNum & (MOVEFIFOSIZ-1) aliases every MOVEFIFOSIZ tics) and latch a
    // false "Out Of Sync" that triggers a needless resync.
    // tickNum < 30: comparison grace for the first second of every session.
    // The moments right after a barrier/snapshot reload are racy on purpose
    // (rings refill, cursors reset, stale stamps drain) and comparing there
    // produced false flags that re-triggered the auto-resync in a loop
    // (soak-caught: post-heal sessions re-flagged at tic 1-2 with zero-hash
    // artifacts). Real divergence persists past the grace and still flags.
    if (tickNum < 30 || tickNum == g_player[otherconnectindex].lastSyncTick || (tickNum > movefifoplc)
        || tickNum <= movefifoplc - (MOVEFIFOSIZ - 8))
        return;

    g_netSyncCompares++; // debug surface: proves the CRC comparison actually runs
    g_player[otherconnectindex].lastSyncTick = tickNum;

    // Grab sync info from packet buffer
    for (int32_t sb = 0; sb < NUM_SYNC_TYPES; sb++)
    {
        char head_sync = syncData[tickNum & (MOVEFIFOSIZ - 1)][sb];
        char player_sync = packbuf[(*j)++];

        if (player_sync != head_sync)
        {
            if (!syncError[sb])
            {
                OSD_Printf("Desynchronized! Player: %d, Type: %s, Head CRC: %d, Player CRC: %d, TickNum: %d, MoveFifoPlc: %d\n", otherconnectindex, syncType[sb].name, head_sync, player_sync, tickNum, movefifoplc);
                syncError[sb] = true;
            }

#if defined(__EMSCRIPTEN__)
            // DEBUG (desync validation): the first few RAW mismatches, typed.
            // One category flaking = real divergence in that subsystem; ALL
            // categories at once = comparison misalignment.
            {
                static int s_worldLogs, s_inputLogs;
                bool const isInputCat = (sb >= 11);
                if (isInputCat ? (s_inputLogs < 24) : (s_worldLogs < 8))
                {
                    (isInputCat ? s_inputLogs : s_worldLogs)++;
                    EM_ASM({ console.log('[eng] MISMATCH cat=' + $0 + ' tic=' + $1 + ' local=' + $2 + ' remote=' + $3 + ' plc=' + $4 + ' ep=' + $5); },
                           sb, tickNum, (int)(uint8_t)head_sync, (int)(uint8_t)player_sync, movefifoplc,
                           (int)g_netMoveEpoch);
                    // Input-hash mismatch: dump the LOCAL ring's raw fields for
                    // that tic, both players, so the two sides' logs can be
                    // diffed field-by-field.
                    if (isInputCat)
                        for (int plr = 0; plr < 2; plr++)
                        {
                            input_t const *in = &inputfifo[(tickNum - 1) & (MOVEFIFOSIZ - 1)][plr];
                            EM_ASM({ console.log('[eng] INPDUMP tic=' + $0 + ' p=' + $1 + ' fvel=' + $2 + ' svel=' + $3 + ' avel=' + $4 + ' horz=' + $5 + ' bits=0x' + ($6 >>> 0).toString(16) + ' ext=0x' + ($7 >>> 0).toString(16)); },
                                   tickNum - 1, plr, in->fvel, in->svel, (int)in->q16avel, (int)in->q16horz,
                                   (int)in->bits, (int)in->extbits);
                        }
                }
            }
#endif
            desynched_players[otherconnectindex] = 1;
        }
    }
}


