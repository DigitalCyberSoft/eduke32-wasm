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
// OpenArena-model state authority (2026-08-09 user directive): 1 = host streams
// authoritative player+sprite state continuously and ALL lockstep divergence
// machinery (CRC compare, desync reports, heal ladder, tic-stamped snap gating)
// is off. 0 = legacy lockstep repair, kept only as an A/B kill-switch.
// Plain extern (not OLDNET_EXTERN): the default is 1, set at the definition.
extern int32_t g_netStreamMode;
extern int32_t g_netLateJoinMask; // slots whose peer-up landed mid-game; host seats them via relaunch (menus.cpp)

// Stream-mode coop REPLAY-KILL: the guest simulates enemies locally (so they
// animate), but it deals no damage (A_DamageObject is host-only), so its CON can
// never reach the dead state on its own -- the host-killed monster would stay a
// "zombie" forever. When the stream reports an enemy dead, oldnet.cpp arms a
// pending lethal hit (htextra) so the guest's OWN CON runs the real death --
// anim, gibs, sound -- within a tic of the host's.
void Net_StreamClearDeadActors(void);            // G_EnterLevel reset (indices reused)

// NEW_GAME flags bit (high half of the low 16 reserved bits): guests must NOT
// run their own level entry -- the host enters first and streams each guest
// the entry snapshot through the late-join pipeline. Two independent premaps
// are never bit-identical (local sprite-insert histories differ), and a guest
// forked at tic 0 lives on the softsnap drip forever.
#define NEWGAME_VIA_SNAPSHOT 0x8000
void Net_SeatLateJoiners(void);   // apply the mask to connected[] + rebuild the chain
void Net_DemoteGuestsToSnapshotEntry(void);  // launch: guests leave entry roster -> join pipeline
extern int32_t g_netHostGone;     // guest: the host peer went down; exit to the main menu
extern int32_t g_netSnapshotReady; // receiver: late-join snapshot file landed; load + barrier
void Net_InsertLatePlayer(int k);  // host: materialize a late joiner in the live world
int  Net_SaveLateJoinSnapshot(void);
int  Net_ApplyLateJoinSnapshot(void);
extern uint8_t g_netMoveEpoch;    // lockstep generation stamp on move packets
extern int32_t g_netEpochDrops;

// ── Tic-indexed loss-tolerant move protocol (transport track) ────────────────
// Every M2S/S2M packet is SELF-CONTAINED: it names the absolute tic range it
// carries and re-sends everything the receiver has not yet acknowledged, so the
// move channel runs UNRELIABLE/UNORDERED and any loss/reorder pattern repairs
// itself from the next packet. See oldnet.cpp "wire format" comment.
extern input_t g_netStagedInput;  // MP-sampled local input (never inputfifo[0]: that IS ring slot 0)
extern int32_t g_netDupTics;      // redundant tic records deduplicated (loss-repair proof)
extern int32_t g_netGapDrops;     // packets ignored because their window started past our high-water
extern int32_t g_netStallSince;   // totalclock when the consume gate first blocked (0 = flowing)
extern int32_t g_netStallMask;    // players the consume gate is waiting on
void Net_ConsumeQuitInputs(void); // deterministic voluntary-leave excision (consumption time)
void Net_ApplyPendingDrops(void); // deterministic involuntary excision at the master-stamped tic
void Net_ExcisePlayer(int i);     // remove a player from the running match (classic quit body)
extern int32_t g_netPredictMode;  // prediction mask (NN_PREDICT): bit0 = correction pass,
                                  // bit1 = view swap, bit2 = reserved (P3), bit3 = P2 sounds,
                                  // bit4 = idle correction deadband (standstill-flash fix)
void Net_FlushPendingDrops(void); // barrier entry: fold pending drops into the roster, no world edits

// ── Canonical stream + no-stall join (host = input sequencer) ────────────────
// The master's M2S stream is THE timeline: slaves consume every column from the
// echo, INCLUDING their own (local samples go to g_netSendRing for the wire and
// the predictor, never straight into the consume fifo). That makes master-side
// synthesis (deadline-fill for laggards, zero-fill for a seating joiner) safe
// by construction -- every peer consumes identical bytes, so lag can no longer
// desync anyone and a slow peer stalls nobody but themself.
extern input_t g_netSendRing[MOVEFIFOSIZ]; // slave: locally sampled inputs staged for S2M + prediction
extern int32_t g_netSampleHead;   // slave: absolute tic of the next local sample
extern int32_t g_netFillTics;     // master: tics synthesized past the fill deadline (telemetry)
extern int32_t g_netJoinCatchup;  // joiner/healing guest: snapshot applied, streaming to live
void Net_ApplyPendingJoins(void); // deterministic seat at the master-stamped joinTic (all peers)
void Net_HostJoinFlow(void);      // host: barrier-free join state machine (menus.cpp frame point)
int  Net_JoinFlowActive(void);    // a join is streaming/announced: defer any heal until it lands

// ── Targeted resync (the join streamer minus the seat) ───────────────────────
// A diverged guest reloads a host snapshot and catches up while everyone else
// plays on (deadline-fill covers its column). The old broadcast heal reloaded
// every peer through a barrier: one guest's divergence froze the whole match.
extern int32_t g_netDesyncReporters; // host: guests whose DESYNC_REPORT named them diverged
int  Net_StartHealFlow(int k);    // host: begin healing seat k (0 = flow started)
void Net_CheckHealResume(void);   // healing guest: leave watcher mode at the live edge
extern int32_t g_netBotMask;      // CPU seats (host synthesizes their inputs)
#ifdef __cplusplus
extern "C" int Net_GetBotMask(void); // transport admission: engine CPU reservations
#else
int Net_GetBotMask(void);
#endif
extern int32_t g_netBotSkill;     // 0..2, host-side only
extern int32_t g_netMinPlayers;   // bots fill to this count, yield as humans join
extern int32_t g_netLocalBot;     // TEST MODE: own input from the bot brain
void Net_SeatBots(int minPlayers, int skill); // host: retain/fill seats up to floor pre-launch/relaunch
#ifdef NETNATIVE
int Net_TestSeatBotsRelaunch(void); // deterministic H01 characterization fixture
#endif
input_t Net_BotInput(void);       // bot input for the LOCAL peer (test mode)
void Net_SendStateSnap(int k);    // host: in-place RNG+player correction to one guest
int  Net_CorrectDivergence(int k);// host: correction ladder (soft snaps -> snapshot heal)
void Net_ApplyPendingStateSnap(void);
void Net_SmoothRemoteSeats(void); // guest: consume stashed snap at its stamped tic
void Net_SelfPosWatch(const char *tag); // forensics: names the tic phase that yanked OUR pos

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
    // A guest's CRC comparisons flagged a divergence the host's own compares
    // may never see (stamp visibility is lag-asymmetric: future-stamped tics
    // are dropped, not deferred). Only the HOST can heal (snapshot push), so
    // guests report; the host latches g_foundSyncError and the auto-resync
    // consumer takes it from there. Soak-caught: a guest sat visibly desynced
    // for 70+ seconds while the host stayed blind -- permanent silent split.
    PACKET_TYPE_DESYNC_REPORT,
    PACKET_TYPE_STATE_SNAP,   // host -> diverged guest: in-place RNG+player correction (no reload)
    PACKET_TYPE_SPRITE_STREAM, // host -> guest: continuous authoritative sprite deltas (stream mode)
    PACKET_TYPE_SECTOR_STREAM, // host -> guest: sector ceiling/floor heights (doors are host-owned)
    PACKET_TYPE_HIT_REPORT,    // guest -> host: CLIENT-AUTHORITATIVE hitscan (unified; kind 0=player seat, 1=enemy sprite)
    PACKET_TYPE_WEAPON_STATE,  // guest -> host: authoritative live curr_weapon (+got/ammo) so the host fires what the guest holds
    PACKET_TYPE_POS_REPORT,    // guest -> host: CLIENT-AUTHORITATIVE self position/facing ("trust the client where it ended up")
    PACKET_TYPE_ACCESS_STATE,  // both ways (coop): shared key-card bits -- any player's card unlocks for everyone
    PACKET_TYPE_WALL_STREAM,   // host -> guest: wall-vertex positions of wall-motion doors (swing/slide) -- heights alone missed them
    PACKET_TYPE_READY_ROSTER,  // host -> guests, ~1Hz while at the entry barrier: bitmask of seats already in (display only)
    PACKET_TYPE_LEVEL_GO,      // host -> guests, stream mode: EXPLICIT entry release ([1] = host crossing number); the only signal that opens a guest's barrier
    PACKET_TYPE_JOIN_ACK,      // catchup peer -> host, RELIABLE: [1]=epoch, [2..5]=contiguous M2S applied; paces the join/heal flow (s_slaveAck)
    PACKET_TYPE_SEAT_SPAWNED,  // host -> guests, stream: [1]=seat that just spawned (on-demand or respawn). Bystanders get ZERO-FILLED inputs for other guests, so they can never infer another guest's spawn from the input stream -- it must be told.
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
void Net_DrainObjectHits(void);   // host, on-tick: apply guests' reported breakable/object hits
void Net_DrainEnemyHits(void);    // host, on-tick: apply guests' reported enemy hits (wake dormant + damage)
void Net_GlideEnemies(void);      // guest, per-tic: glide live enemies onto streamed positions (no snap jitter)
void Net_ApplyGuestWeapon(int seat); // host, per-tic: force a guest seat's curr_weapon/got/ammo from its report
void Net_ApplyGuestPos(int seat);    // host, per-tic: adopt a guest seat's reported position/facing (client-authoritative movement)
void Net_BroadcastSeatSpawn(int playerNum); // host, stream: reliable "seat spawned" telegram to every guest (bystanders cannot infer it -- see PACKET_TYPE_SEAT_SPAWNED)
void Net_ShareCoopAccess(void);      // host, per-tic (coop): union every seat's key cards and share to all (guest no-op)
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
void Net_SendEol(void);                 // stream host: authoritative level-flip broadcast
extern int32_t g_netEolFromHost;        // stream guest: current MODE_EOL is host-authorized
void Net_EnterMessage(void);

void Net_InitiateVote();
void Net_CancelVote();

void Net_ClearFIFO(void);
void Net_CheckPlayerQuit(int i);
void Net_Disconnect(bool showScores);
void Net_WaitForPlayers();

#endif
