#ifndef OLDNET_H
#define OLDNET_H

#include "build.h"
#include "player.h"
#include "sync.h"

#ifdef OLDNET_CPP_
#define OLDNET_EXTERN
#else
#define OLDNET_EXTERN extern
#endif

// Number of players present when the current match started. Not in mainline.
extern int32_t playerswhenstarted;

// netduke32's all-players iterator + helper (mainline lacks both). Safe to
// define globally: the stock tree does not use the ALL_PLAYERS name.
static inline int32_t G_GetNextPlayer(int32_t pNum)
{
    for (int32_t i = pNum + 1; i < MAXPLAYERS; i++)
        if ((g_player[i].ps != NULL) && g_player[i].connected)
            return i;
    return -1;
}
#ifndef ALL_PLAYERS
# define ALL_PLAYERS(i) i = 0; i != -1; i = G_GetNextPlayer(i)
#endif

#define INPUTFIFO_CURTICK (movefifoplc & (MOVEFIFOSIZ - 1))
#define INPUTFIFO_LASTTICK ((movefifoplc - 1) & (MOVEFIFOSIZ - 1))
#define INPUTFIFO_PREDICTTICK (predictfifoplc & (MOVEFIFOSIZ - 1))

struct votedata_t
{
    int32_t dmflags;
    int8_t starter = -1, level = -1, episode = -1;
    int8_t yes_votes;
    int8_t gametype, skill;
};
OLDNET_EXTERN votedata_t vote;

extern int quittimer;
extern int lastpackettime;
extern int mymaxlag, otherminlag, bufferjitter;

extern int movefifosendplc;
extern int movefifoplc;

extern int g_networkBroadcastMode;
extern int botNameSeed;

// ---------------------------------------------------------------------------
// Snapshot-netcode compatibility shims.
//
// This tree's game code was written against mainline's client/server SNAPSHOT
// netcode (network.h). Under NETDUKE32 that header is replaced by this lockstep
// netcode, so we map the snapshot symbols the game code still references onto
// lockstep equivalents. In the game files (everything except the excluded
// network.cpp) g_netServer/g_netClient are used purely as truthiness -- all
// enet/pointer member access lived in network.cpp -- so these macros suffice.
// ---------------------------------------------------------------------------
#ifdef __GNUC__
# define EDUKE32_UNUSED __attribute__((unused))
#else
# define EDUKE32_UNUSED
#endif

// Lockstep role predicates: "am I the master/authoritative peer" and "am I a
// non-master peer". (In the snapshot model these were ENetHost* handles.)
#define g_netServer (numplayers > 1 && myconnectindex == connecthead)
#define g_netClient (numplayers > 1 && myconnectindex != connecthead)

// Snapshot per-sprite network TRACKING has no lockstep equivalent (the sim is
// deterministic and replays identically on every peer) -- but the snapshot
// functions did more than track. Net_InsertSprite's insertion duty is handled
// by A_InsertSprite's NETDUKE32 branch (game.cpp calls insertsprite directly),
// so the insert hook reduces to a true no-op. Net_DeleteSprite's delete duty
// does NOT: mainline deletes the sprite in every branch (solo deletesprite,
// client hide, server tracked delete). Stubbing it to ((void)0) silently
// disabled EVERY A_DeleteSprite in the game -- one-shot actors (e.g. the E1L1
// DUKECAR cinematic) re-ran their death blocks each tic, flooding the sprite
// list to MAXSPRITES within seconds and corrupting memory at the cap. Lockstep
// semantics = plain local delete on all peers, classic mmulti style.
#define Net_InsertSprite(...) ((void)0)
#define Net_DeleteSprite(spriteNum) deletesprite(spriteNum)

// Snapshot world-state machinery has no lockstep equivalent (deterministic
// replay needs no world snapshots) -> no-op, as under mainline NETCODE_DISABLE.
#define Net_StoreClientState(...) ((void)0)
#define Net_ResetPrediction(...) ((void)0)
#define Net_InitMapStateHistory(...) ((void)0)
#define Net_AddWorldToInitialSnapshot(...) ((void)0)
#define Net_WaitForInitialSnapshot(...) ((void)0)
#define DumpMapStateHistory(...) ((void)0)

// Snapshot connection layer. The transport track owns the wire; these exist so
// the game compiles, but the in-engine snapshot connect/dedicated-server paths
// stay inert (g_networkMode stays NET_CLIENT -- no dedicated server in-browser).
enum { NET_CLIENT = 0, NET_SERVER, NET_DEDICATED_CLIENT, NET_DEDICATED_SERVER };
enum { CHAN_MOVE = 0, CHAN_GAMESTATE, CHAN_CHAT, CHAN_MISC, CHAN_MAX };
extern int      g_networkMode;
extern int      g_netDisconnect;
extern char     g_netPassword[32];
extern uint16_t g_netPort;

// Snapshot packet-type names the game code still uses -> lockstep packet types.
#define PACKET_MESSAGE PACKET_TYPE_MESSAGE
#define PACKET_RTS     PACKET_TYPE_RTS

// Vote senders -> lockstep equivalents (oldnet.cpp). Variadic to absorb the
// snapshot signatures' args (e.g. Net_SendMapVoteCancel(failed)).
#define Net_SendMapVoteInitiate(...) Net_InitiateVote()
#define Net_SendMapVoteCancel(...)   Net_CancelVote()

// Loud one-shot marker for deferred peripheral MP features. Never a silent
// no-op: each deferred site logs once at ERROR. Game files use it too.
#define NETDUKE32_MP_TODO(what)                                                            \
    do {                                                                                   \
        static bool warned_ = false;                                                       \
        if (!warned_) { warned_ = true;                                                    \
            LOG_F(ERROR, "MP: %s not yet ported (NetDuke32 port); feature disabled", what);\
        }                                                                                  \
    } while (0)

// Snapshot client/server state-sync entry points the game code still calls.
// The lockstep FIFO (Net_HandleInput, wired into G_MoveLoop) carries per-tic
// state instead, so these snapshot senders are inert. Map "wait for server"
// onto the lockstep ready handshake; the rest no-op.
#define Net_WaitForServer       Net_WaitForPlayers
#define Net_SendClientUpdate(...)   ((void)0)
#define Net_SendServerUpdates(...)  ((void)0)
#define Net_SendMapUpdate(...)      ((void)0)
#define Net_NotifyNewGame(...)      ((void)0)
#define Net_SpawnPlayer(...)        ((void)0)

// Deferred peripheral senders (loud, not silent): in-engine chat send (transport
// carries lobby chat), per-player map-vote cast (vote UI deferred).
#define Net_SendMessage(...)  NETDUKE32_MP_TODO("in-engine chat send")
#define Net_SendMapVote(...)  NETDUKE32_MP_TODO("in-engine map-vote cast")

// Client info on join -> lockstep player name + options (oldnet.cpp).
void Net_SendClientInfo(void);

OLDNET_EXTERN input_t netInput;

OLDNET_EXTERN bool oldnet_gotinitialsettings; // True if we got PACKET_TYPE_INIT_SETTINGS from the host.
extern int32_t g_netLateJoinMask; // slots whose peer-up landed mid-game; host seats them via relaunch (menus.cpp)
void Net_SeatLateJoiners(void);   // apply the mask to connected[] + rebuild the chain
extern int32_t g_netHostGone;     // guest: the host peer went down; exit to the main menu
extern int32_t g_netSnapshotReady; // receiver: late-join snapshot file landed; load + barrier
void Net_InsertLatePlayer(int k);  // host: materialize a late joiner in the live world
int  Net_SaveLateJoinSnapshot(void);
int  Net_ApplyLateJoinSnapshot(void);
extern uint8_t g_netMoveEpoch;    // lockstep generation stamp on move packets
extern int32_t g_netEpochDrops;

//OLDNET_EXTERN PredictBackup_t predictBackup[MOVEFIFOSIZ];

enum DukePacket_t
{
    PACKET_TYPE_MASTER_TO_SLAVE,
    PACKET_TYPE_SLAVE_TO_MASTER,
    PACKET_TYPE_BROADCAST,
    SERVER_GENERATED_BROADCAST,
    PACKET_TYPE_VERSION,

    /* don't change anything above this line */

    PACKET_TYPE_MESSAGE,

    PACKET_TYPE_NEW_GAME,
    PACKET_TYPE_RTS,
    PACKET_TYPE_MENU_LEVEL_QUIT,
    PACKET_TYPE_WEAPON_CHOICE,
    PACKET_TYPE_PLAYER_OPTIONS,
    PACKET_TYPE_PLAYER_NAME,
    PACKET_TYPE_INIT_SETTINGS,

    PACKET_TYPE_USER_MAP,

    PACKET_TYPE_MAP_VOTE,
    PACKET_TYPE_MAP_VOTE_INITIATE,
    PACKET_TYPE_MAP_VOTE_CANCEL,

    PACKET_TYPE_LOAD_GAME,
    PACKET_TYPE_NULL_PACKET,
    PACKET_TYPE_PLAYER_READY,
    PACKET_TYPE_FRAGLIMIT_CHANGED,
    PACKET_TYPE_EOL,
    PACKET_TYPE_PING,
    PACKET_END, // Should remain last in list.
};

enum NetMode_t
{
    NETMODE_MASTERSLAVE,
    NETMODE_P2P, // UNSUPPORTED.
    NETMODE_OFFLINE = 255
};

void faketimerhandler(void);
void Net_HandleInput(void);
void Net_GetPackets(void);
void Net_ParsePackets(void);
void Net_SendQuit(void);
void Net_SendWeaponChoice(void);
void Net_SendVersion(void);
void Net_SendPlayerOptions(void);
void Net_SendFragLimit(void);
void Net_SendPlayerName(void);
void Net_SendUserMapName(void);
void Net_SendInitialSettings(void);
void Net_SendNewGame(uint32_t flags);
void Net_EndOfLevel(bool secret);
void Net_EnterMessage(void);

void Net_InitiateVote();
void Net_CancelVote();

void Net_ClearFIFO(void);
void Net_CheckPlayerQuit(int i);
void Net_Disconnect(bool showScores);
void Net_WaitForPlayers();

#endif
