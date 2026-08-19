#define OLDNET_CPP_

#include "duke3d.h"
#include "oldnet.h"
#include "bot_lifecycle.h"

// Forward decl: file-static, defined mid-file; the seat-mask receive path in the
// packet dispatch runs before it textually.
static void Net_RebuildConnectChain(void);
#include "net_predict.h"
#include "net_transport.h"
#include "net_phase2.h"
#include "chatpipe.h"
#include "timer.h"
#include "demo.h"  // G_CloseDemoWrite (Net_CheckPlayerQuit)
#include "savegame.h"  // late-join snapshot: sv_saveandmakesnapshot / G_LoadPlayer
#if defined(NETNATIVE) && !defined(_WIN32)
# include <unistd.h>  // usleep (NN_TESTSLOWLOAD harness knob)
#endif
#ifdef __EMSCRIPTEN__
# include <emscripten.h>
#endif

// NetDuke32's player-iteration macros differ from our tree's (ours are bare
// loop clauses used as `for (TRAVERSE_CONNECT(i))`; netduke32's bake in the
// for()). Redefine them TU-locally so this module compiles verbatim without
// disturbing the rest of the engine.
#undef TRAVERSE_CONNECT
#define TRAVERSE_CONNECT(i) for (i = connecthead; i != -1; i = connectpoint2[i])
#ifndef ALL_PLAYERS
# define ALL_PLAYERS(i) i = 0; i != -1; i = G_GetNextPlayer(i)
#endif

// netduke32's gametext(x, y, str, shade, orient) -> this tree's 8-arg gametext_
// (our tree's stock gametext is a 3-arg convenience macro). TU-local so the rest
// of the engine is unaffected. Used by the "waiting for players" sync screen.
#undef gametext
#define gametext(x, y, t, s, o) gametext_((x) << 16, (y) << 16, (t), (s), 0, (o), 0, 0)

#define TIMERUPDATESIZ 32

// quickExit: set when the user force-quits (ctrl-alt-del / crash). Not present
// in mainline; owned here by the netcode.
int quickExit = 0;

// Classic multiplayer load-game relay globals (absent from our tree).
signed char multiwho, multipos, multiwhat, multiflag;

// netduke32 deathmatch flag absent from our tree's flag set (used only in a
// visibility-adjustment log line).
#ifndef DMFLAG_ALLOWVISIBILITYCHANGE
# define DMFLAG_ALLOWVISIBILITYCHANGE (1 << 11)
#endif
#ifndef M_DMFLAGS_TEST
# define M_DMFLAGS_TEST(x) (ud.m_dmflags & (x))
#endif

// ---------------------------------------------------------------------------
// Deferred peripheral MP features.
//
// Per the port scope, netduke32's HUD-text / savegame / RTS subsystem
// refactors are intentionally NOT ported (that is the excluded 1019-commit
// non-network refactor). The netcode CORE (input FIFO, master/slave exchange,
// prediction, sync CRC, INIT_SETTINGS/NEW_GAME/PING/EOL) is fully ported; the
// handlers below depend on those unported subsystems and are deferred.
//
// Every deferred site logs LOUDLY (once) via NETDUKE32_MP_TODO (defined in
// oldnet.h so game files can use it too) and then takes the safe/aborting path
// -- never a silent no-op, never a fake-success return. Deferred backlog:
// (1) chat/vote typing UI, (2) LOAD_GAME relay, (3) RTS taunt-over-net,
// (4) player-color validation + renamed ud.config.
// ---------------------------------------------------------------------------

// (4) Player-color validation lives in netduke32's screens/HUD subsystem.
// Deferred: log and pass the raw color through unvalidated.
static int playerColor_getValidPal(int color)
{
    NETDUKE32_MP_TODO("player-color validation");
    return color;
}

// (1) In-engine chat typing box lives in netduke32's HUD-text subsystem; the
// transport track carries lobby chat instead. Deferred: cancel (return -1),
// never a fake success.
static int Net_EnterText(int /*x*/, int /*y*/, char * /*t*/, int /*dalen*/, int /*c*/)
{
    NETDUKE32_MP_TODO("chat typing UI");
    return -1;
}

// ---------------------------------------------------------------------------
// CPU players ("bots"): seats whose inputs the HOST synthesizes into the
// canonical stream each tic. Guests consume them like any remote player --
// their machines never know (or need to know) the seat is CPU, so behavior
// and difficulty are host-decided by construction. Sync-safe: inputs travel,
// the sim stays identical everywhere.
int32_t g_netBotMask;        // slots that are CPU seats (host-authoritative)
int32_t g_netBotSkill = 2;   // CPU skill 0..3, indexes the Duke skill names
                             // (0 Piece Of Cake .. 3 Damn I'm Good); host-side only.
                             // Default HARD: medium's wobble read as "an
                             // absolute shit job at aiming" (live report).
int32_t g_netMinPlayers = 5; // host's match-size floor: bots fill up to this
                             // count and yield seats back as humans join
extern int32_t g_netForensics; // file definition is below; seating/LMS probes use it
int32_t g_netLocalBot;       // TEST MODE: this peer's OWN input comes from the
                             // bot brain through the full human pipeline --
                             // sampling, staging, S2M, prediction. On the wire
                             // it is indistinguishable from a human player.
char    g_netAutoJoin[1024]; // -join CODE: fired once by the menu frame loop

// Transport seam. The netcode never talks to enet/UDP/sockets directly; every
// outgoing packet is classified onto a logical channel + reliability here and
// handed to the pluggable transport (net_transport.h). Inbound frames arrive
// via Net_ReceiveFrame() below. peerToken == connectindex.
// ---------------------------------------------------------------------------
static void oldnet_sendpacket(int other, unsigned char *bufptr, int len)
{
    if (g_netBotMask & (1 << other))
        return;                       // CPU seat: there is no transport peer
    int channel, reliable;

    switch (bufptr[0])
    {
        case PACKET_TYPE_MASTER_TO_SLAVE:
        case PACKET_TYPE_SLAVE_TO_MASTER:
        case PACKET_TYPE_PING:
        case PACKET_TYPE_WEAPON_STATE:  // per-tic guest weapon: unreliable, self-corrects next tic
        case PACKET_TYPE_POS_REPORT:    // per-tic guest position: unreliable, next report supersedes
            channel  = NET_CHAN_MOVE;   // per-tic input: unreliable, unordered
            reliable = 0;
            break;

        // THE OPENARENA SNAPSHOT MODEL. STATE_SNAP is an ABSOLUTE, self-
        // contained repaint of every player's position/score every 10 tics;
        // SECTOR_STREAM is absolute door/floor heights re-sent each pass. Both
        // self-heal on the NEXT send, so they belong on the UNRELIABLE channel
        // exactly like Q3/OpenArena snapshots. Sending them reliable-ordered
        // (the old default) meant any single lost or delayed packet
        // HEAD-OF-LINE-BLOCKED the entire state stream: on a real WebRTC link
        // with jitter/loss (or an ICE hiccup -- observed live) every remote
        // player FROZE until the retransmit landed, then snapped. Localhost
        // (no loss) never showed it, so it survived every client-side
        // smoothing rewrite -- the smoother can't help when the targets stop
        // arriving. Unreliable = a dropped snapshot is simply skipped and the
        // next one (33ms later) carries current truth.
        case PACKET_TYPE_STATE_SNAP:
        case PACKET_TYPE_SECTOR_STREAM:
        case PACKET_TYPE_WALL_STREAM:    // absolute wall-vertex paints: self-heal like sector heights
            if (g_netStreamMode)
            {
                channel  = NET_CHAN_MOVE;
                reliable = 0;
            }
            else                        // legacy lockstep: the snap carries RNG
            {                           // correction that MUST arrive in order
                channel  = NET_CHAN_REL;
                reliable = 1;
            }
            break;

        // SPRITE_STREAM stays reliable-ordered for now: its records are
        // absolute and self-heal via the keyframe sweep, BUT it also carries
        // DELETE records (a picked-up item, a destroyed prop) that must not be
        // lost or the guest keeps a ghost forever. The proper fix is to split
        // it -- kinematic/position records unreliable, delete/identity records
        // reliable -- which is the Q3 "unreliable snapshot + reliable
        // reliable-commands" split. Tracked as the next step; decoupling
        // STATE_SNAP above already frees PLAYER motion from this channel.

        case PACKET_TYPE_USER_MAP:
        case PACKET_TYPE_LOAD_GAME:
            channel  = NET_CHAN_BULK;   // potentially large: isolated bulk channel
            reliable = 1;
            break;

        case PACKET_TYPE_HIT_REPORT:
            // Client-authoritative damage: SPARSE (a few per shot, not per-tic)
            // and gameplay-critical -- a lost report is lost damage, the exact
            // bug this fixes. Reliable. It rides guest->host, where NET_CHAN_REL
            // carries only sparse control (votes/chat), so there is no head-of-
            // line coupling with the heavy host->guest SPRITE_STREAM.
            channel  = NET_CHAN_REL;
            reliable = 1;
            break;

        default:
            channel  = NET_CHAN_REL;    // control / vote / name / chat: reliable, ordered
            reliable = 1;
            break;
    }

    net_send(other, channel, reliable, bufptr, len);
}

int netQuitSend  = 0;
int quittimer = 0;
int lastpackettime = 0;
int mymaxlag, otherminlag, bufferjitter = 1;

int movefifosendplc;
int movefifoplc;

// ── STATE AUTHORITY (the OpenArena model; 2026-08-09 user directive) ────────
// "get to parity with how openarena handles player actions, not this turn
// based thing that keeps breaking." The input plane was already Q3-shaped
// (master aggregates a canonical cmd timeline with deadline-fill; slaves
// predict locally) -- what kept breaking was the STATE plane: guests lockstep-
// simulated the world and any WASM nondeterminism forked them forever, patched
// by 5s soft snaps and reload heals the user felt as rubber-banding. In stream
// mode the host's sim is the ONLY truth: it broadcasts player packs and sprite
// deltas continuously, guests keep their sim as a local predictor that the
// stream repaints, and every divergence-repair mechanism is retired. There is
// no "desync" anymore -- only correction latency, bounded by the stream rate.
int32_t g_netStreamMode = 1;

// NetDuke32 netcode globals not present in mainline. g_networkBroadcastMode
// selects master/slave vs offline; botNameSeed drives deterministic bot names.
int g_networkBroadcastMode = NETMODE_OFFLINE;
int botNameSeed = 0;
int32_t playerswhenstarted = 1;

// Snapshot connection-layer compat globals (see oldnet.h). Inert under the
// lockstep model; the transport track establishes connections.
int      g_networkMode = NET_CLIENT;
int      g_netDisconnect = 0;
char     g_netPassword[32];
uint16_t g_netPort = 0;

static int g_chatPlayer = -1;
static char recbuf[180];

extern char *rtsptr; // game.c

int pingTime;

static void Net_AddPingTimeToPacket(int* j)
{
    B_BUF32(&packbuf[(*j)], pingTime);
    (*j) += sizeof(int32_t);
}

static void Net_GetPingTimeFromPacket(int* j)
{
    pingTime = (int32_t)B_UNBUF32(&packbuf[(*j)]);
    (*j) += sizeof(int32_t);
}

static void Net_AddPlayerPingToPacket(int* j, int playerNum)
{
    B_BUF32(&packbuf[(*j)], g_player[playerNum].ping);
    (*j) += sizeof(int32_t);
}

static void Net_GetPlayerPingFromPacket(int* j, int playerNum)
{
    g_player[playerNum].ping = (uint32_t)B_UNBUF32(&packbuf[(*j)]);
    (*j) += sizeof(int32_t);
}

static void Net_SendPing(void)
{
    if (myconnectindex != connecthead)
        return;

    static uint32_t lastPingTime;

    uint32_t tics = timerGetTicks();
    if (tics - lastPingTime >= 1000)
    {
        packbuf[0] = PACKET_TYPE_PING;
        int j = 1;

        lastPingTime = pingTime = tics;
        Net_AddPingTimeToPacket(&j);

        int i;
        TRAVERSE_CONNECT(i)
        {
            Net_AddPlayerPingToPacket(&j, i);
        }

        TRAVERSE_CONNECT(i)
        {
            if (i != myconnectindex)
                oldnet_sendpacket(i, (unsigned char*)packbuf, j);
        }
    }
}

void Net_GetPackets(void)
{
#if 1
    if ((totalclock > quittimer) && (g_gameQuit == 1))
    {
        OSD_Printf("Timeout: Didn't get quit packet in time, exiting.\n");
        G_GameExit(" ");
    }
#endif

    ChatPipe_Poll();

    // Always drain the transport FIRST: NET_PEER_UP/DOWN events bootstrap numplayers
    // (the first guest joining takes it 1 -> 2). This MUST run even when numplayers < 2,
    // otherwise net_poll (gated in Net_ParsePackets below) never fires, numplayers is
    // stuck at 1, and the host can never reach the >1 state its lobby/launch gate needs.
    net_poll();

    {
        extern uint32_t g_seatDiagUntil;
        uint32_t const wallNow = timerGetTicks();
        if (g_netJoinCatchup || wallNow < g_seatDiagUntil)
        {
            // Joiner heartbeat (~2s), ALWAYS-pumped path, WALL-clock limited
            // (totalclock freezes exactly when the wedges under test hit).
            // Covers catchup AND the first 40s after the seat.
            static uint32_t nextBeat;
            if (wallNow >= nextBeat)
            {
                auto const ps = g_player[myconnectindex].ps;
                nextBeat = wallNow + 2000;
                LOG_F(INFO, "[join] beat: plc=%d send=%d myend=%d sh=%d r2s=%d gm=%d dead=%d tc=%d otc=%d stall=0x%x np=%d",
                      movefifoplc, movefifosendplc, g_player[myconnectindex].movefifoend,
                      g_netSampleHead, (int)ready2send, ps ? (int)ps->gm : -1,
                      ps ? (int)ps->dead_flag : -1, (int32_t)totalclock, (int32_t)ototalclock,
                      (unsigned)g_netStallMask, numplayers);
            }
        }
    }

    if (numplayers < 2 || g_networkBroadcastMode == NETMODE_OFFLINE)
        return;

    Net_SendPing();
    Net_ParsePackets();
}

void Net_AddPlayerInputToPacket(int* j, int playerNum, input_t* osyn, input_t* nsyn)
{
    int inputFlagsPos = (*j)++;
    int extFlagsPos = (*j)++;

    packbuf[inputFlagsPos] = 0;
    packbuf[extFlagsPos] = 0;

    if (nsyn[playerNum].fvel != osyn[playerNum].fvel)
    {
        B_BUF16(&packbuf[(*j)], nsyn[playerNum].fvel);
        (*j) += sizeof(int16_t);
        packbuf[inputFlagsPos] |= 1;
    }
    if (nsyn[playerNum].svel != osyn[playerNum].svel)
    {
        B_BUF16(&packbuf[(*j)], nsyn[playerNum].svel);
        (*j) += sizeof(int16_t);
        packbuf[inputFlagsPos] |= 2;
    }
    if (nsyn[playerNum].q16avel != osyn[playerNum].q16avel)
    {
        B_BUF32(&packbuf[(*j)], nsyn[playerNum].q16avel);
        (*j) += sizeof(fix16_t);
        packbuf[inputFlagsPos] |= 4;
    }
    if (nsyn[playerNum].q16horz != osyn[playerNum].q16horz)
    {
        B_BUF32(&packbuf[(*j)], nsyn[playerNum].q16horz);
        (*j) += sizeof(fix16_t);
        packbuf[inputFlagsPos] |= 8;
    }

    if ((nsyn[playerNum].bits ^ osyn[playerNum].bits) & 0x000000ff) packbuf[(*j)++] = (nsyn[playerNum].bits & 255), packbuf[inputFlagsPos] |= 16;
    if ((nsyn[playerNum].bits ^ osyn[playerNum].bits) & 0x0000ff00) packbuf[(*j)++] = ((nsyn[playerNum].bits >> 8) & 255), packbuf[inputFlagsPos] |= 32;
    if ((nsyn[playerNum].bits ^ osyn[playerNum].bits) & 0x00ff0000) packbuf[(*j)++] = ((nsyn[playerNum].bits >> 16) & 255), packbuf[inputFlagsPos] |= 64;
    if ((nsyn[playerNum].bits ^ osyn[playerNum].bits) & 0xff000000) packbuf[(*j)++] = ((nsyn[playerNum].bits >> 24) & 255), packbuf[inputFlagsPos] |= 128;

    if ((nsyn[playerNum].extbits ^ osyn[playerNum].extbits) & 0x000000ff) packbuf[(*j)++] = (nsyn[playerNum].extbits & 255), packbuf[extFlagsPos] |= 1;
    if ((nsyn[playerNum].extbits ^ osyn[playerNum].extbits) & 0x0000ff00) packbuf[(*j)++] = ((nsyn[playerNum].extbits >> 8) & 255), packbuf[extFlagsPos] |= 2;
    if ((nsyn[playerNum].extbits ^ osyn[playerNum].extbits) & 0x00ff0000) packbuf[(*j)++] = ((nsyn[playerNum].extbits >> 16) & 255), packbuf[extFlagsPos] |= 4;
    if ((nsyn[playerNum].extbits ^ osyn[playerNum].extbits) & 0xff000000) packbuf[(*j)++] = ((nsyn[playerNum].extbits >> 24) & 255), packbuf[extFlagsPos] |= 8;
}

void Net_GetPlayerInputFromPacket(int* j, int playerNum, input_t* osyn, input_t* nsyn)
{
    char inputFlags = packbuf[(*j)++];
    char extFlags = packbuf[(*j)++];

    if (playerNum == myconnectindex)
    {
        if (inputFlags & 1) (*j) += sizeof(int16_t);
        if (inputFlags & 2) (*j) += sizeof(int16_t);
        if (inputFlags & 4) (*j) += sizeof(fix16_t);
        if (inputFlags & 8) (*j) += sizeof(fix16_t);

        if (inputFlags & 16) (*j)++;
        if (inputFlags & 32) (*j)++;
        if (inputFlags & 64) (*j)++;
        if (inputFlags & 128) (*j)++;

        if (extFlags & 1) (*j)++;
        if (extFlags & 2) (*j)++;
        if (extFlags & 4) (*j)++;
        if (extFlags & 8) (*j)++;

        return;
    }

    nsyn[playerNum] = osyn[playerNum];

    if (inputFlags & 1)
    {
        nsyn[playerNum].fvel = (short)B_UNBUF16(&packbuf[(*j)]);
        (*j) += sizeof(int16_t);
    }
    if (inputFlags & 2)
    {
        nsyn[playerNum].svel = (short)B_UNBUF16(&packbuf[(*j)]);
        (*j) += sizeof(int16_t);
    }
    if (inputFlags & 4)
    {
        nsyn[playerNum].q16avel = (fix16_t)B_UNBUF32(&packbuf[(*j)]);
        (*j) += sizeof(fix16_t);
    }
    if (inputFlags & 8)
    {
        nsyn[playerNum].q16horz = (fix16_t)B_UNBUF32(&packbuf[(*j)]);
        (*j) += sizeof(fix16_t);
    }

    if (inputFlags & 16)   nsyn[playerNum].bits = ((nsyn[playerNum].bits & 0xffffff00) | ((int)packbuf[(*j)++]));
    if (inputFlags & 32)   nsyn[playerNum].bits = ((nsyn[playerNum].bits & 0xffff00ff) | ((int)packbuf[(*j)++]) << 8);
    if (inputFlags & 64)   nsyn[playerNum].bits = ((nsyn[playerNum].bits & 0xff00ffff) | ((int)packbuf[(*j)++]) << 16);
    if (inputFlags & 128)  nsyn[playerNum].bits = ((nsyn[playerNum].bits & 0x00ffffff) | ((int)packbuf[(*j)++]) << 24);

    if (extFlags & 1)      nsyn[playerNum].extbits = ((nsyn[playerNum].extbits & 0xffffff00) | ((int)packbuf[(*j)++]));
    if (extFlags & 2)      nsyn[playerNum].extbits = ((nsyn[playerNum].extbits & 0xffff00ff) | ((int)packbuf[(*j)++]) << 8);
    if (extFlags & 4)      nsyn[playerNum].extbits = ((nsyn[playerNum].extbits & 0xff00ffff) | ((int)packbuf[(*j)++]) << 16);
    if (extFlags & 8)      nsyn[playerNum].extbits = ((nsyn[playerNum].extbits & 0x00ffffff) | ((int)packbuf[(*j)++]) << 24);

    g_player[playerNum].movefifoend++;

    if (TEST_SYNC_KEY(nsyn[playerNum].bits, SK_GAMEQUIT) &&
        (playerNum == connecthead) && 
        (playerNum != myconnectindex))
    {
        G_GameExit("Host has terminated the game.");
    }
}

void faketimerhandler(void) { };

// ===========================================================================
// TIC-INDEXED LOSS-TOLERANT MOVE PROTOCOL (transport track)
//
// The stock NetDuke32 M2S/S2M packets are one-tic delta codes with NO sequence
// numbers: they assume a lossless in-order link (COMMIT), so one dropped or
// reordered datagram shifts the delta stream and corrupts every later input.
// The transport-track replacement makes every move packet SELF-CONTAINED:
//
//   S2M: [type][epoch][startTic i32][count u8][ackTic i32]
//        count x input records for the sending slave (record 0 deltas against
//        ZERO-INPUT, each next against the previous record -> absolute content,
//        delta-compact encoding)
//        [sync CRC block, unchanged format]
//
//   M2S: [type][epoch][startTic i32][count u8][playerCount u8][destAck i32]
//        playerCount x directory entries [slot | 0x80 if dropped][minlag i8]
//                                        [goneTic i32 when flagged]
//        count x tics, each = records for every listed player still present at
//        that tic (per-player delta chains, zero-based like S2M)
//        [sync CRC block]
//
// Each packet re-sends EVERYTHING from the receiver's last acknowledged tic
// (acks ride every packet in the opposite direction), capped by NET_TICWIN_CAP
// and NET_BYTE_BUDGET. So the move channel runs UNRELIABLE/UNORDERED:
//   * loss inside the window costs nothing (the next packet re-carries it),
//   * reorder is dedup'd by absolute tic index (g_netDupTics counts it),
//   * a burst that outruns the window stalls the ack, which widens the next
//     window back to the ack -- an automatic repair resend from the input ring
//     (MOVEFIFOSIZ=256 tics of history; the local sampler stops at +100).
// The M2S directory doubles as the AUTHORITATIVE roster: a dropped player is
// flagged with the first tic they are absent from (goneTic), and every peer
// excises them at exactly that tic (Net_ApplyPendingDrops). Voluntary quits
// keep the classic in-band SK_GAMEQUIT path (Net_ConsumeQuitInputs), which is
// deterministic for free because every peer consumes the same input stream.
// ===========================================================================

input_t g_netStagedInput;   // MP local input staging (see dukeFillInputForTic)

int32_t g_netDupTics    = 0;  // redundant tic records ignored (proof repair ran)
int32_t g_netGapDrops   = 0;  // packets whose window started past our high-water
int32_t g_netStallSince = 0;  // consume-gate stall start (G_MoveLoop), 0 = flowing
int32_t g_netStallMask  = 0;  // players the consume gate is waiting on

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)

enum
{
    NET_BYTE_BUDGET = 1360,  // soft size ceiling before the sync block
    NET_KEEPALIVE   = 12,    // master resend/keepalive cadence, totalclock (~100ms)
    NET_STALL_DROP  = 1200,  // master: continuous stall on one WARMED peer ->
                             // drop (10s). Not tighter: alt-tab throttling and
                             // GC pauses produce multi-second silences from
                             // perfectly alive peers, deliberate backgrounding
                             // is reaped faster by the visibility auto-leave
                             // (5s), and true deaths by transport peer-down.
    NET_HOST_SILENT = 1200,  // guest: nothing from the host at all -> host gone (10s)
    NET_FIRST_GRACE = 7200,  // master: a peer that has NEVER delivered a record
                             // this session is presumed LOADING (launch-barrier
                             // art load takes 5-30s) -- reap only after 60s.
                             // Soak-caught: the bare 10s silence axe kicked the
                             // guest mid-level-load on the very first launch.
    NET_EOL_GRACE   = 14400, // BOTH axes, across a LEVEL TRANSITION: a machine
                             // loading the next map is provably mute (wasm
                             // blocks its main thread; native pumps the
                             // transport from the game thread), and slow
                             // hardware takes 30s+ over the art cache -- the
                             // 10s axes tore live cross-machine sessions apart
                             // at every map switch (live-reported: the faster
                             // machine dropped before the host finished
                             // loading). 120s of patience, armed at the EOL
                             // broadcast/receipt and released by the first
                             // post-transition packet. True deaths still reap
                             // instantly via transport peer-downs.

    // Canonical-stream synthesis + barrier-free join (see oldnet.h):
    NET_FILL_DEADLINE = 40,   // master: aggregation blocked on one peer this long
                              // (totalclock, ~330ms) -> synthesize their tic from
                              // their last real input and move on. The match never
                              // stalls longer than this on anyone's ping.
    NET_JOIN_MARGIN   = 26,   // joinTic = aggregation head + this (~1s): every peer
                              // sees the directory announcement well before consuming
                              // the seat tic (it rides every M2S until then)
    NET_JOIN_ANNOUNCE = 1200, // announce a join boundary in the directory this long
    NET_JOIN_RING_MAX = 200,  // catchup gap above this -> re-snapshot (256-tic ring)
    NET_JOIN_RETRY    = 1200, // wait this long (10s) before re-snapshotting a slow joiner
    NET_JOIN_TRIES    = 4,    // re-snapshot attempts before the joiner is kicked
};

static input_t const s_zeroInput = {};

// Per-peer protocol state (indexed by connectindex). Reset in Net_ClearFIFO.
static int32_t s_ackOfMyInput;                  // slave: master's high-water of MY input
static int32_t s_slaveAck[MAXPLAYERS];          // master: each slave's ack of the aggregate stream
static int32_t s_lastSendClock[MAXPLAYERS];     // master: last M2S clock per slave (keepalive)
static int32_t s_goneTic[MAXPLAYERS];           // >= 0: first tic WITHOUT this player; -1 = active
static int32_t s_goneAnnounceUntil[MAXPLAYERS]; // master keeps flagging the drop until this clock
// s_goneTic's "no drop" sentinel is -1, but static storage zero-initializes --
// and 0 is a VALID drop boundary. Net_ResetProtocolState fixes the array, but it
// first runs inside Net_ClearFIFO, which is AFTER Net_FlushPendingDrops consults
// the array at the first barrier: the zero-init read as "drop everyone at tic 0"
// and the host silently unseated every guest at first launch (soak-caught).
static struct GoneTicInit { GoneTicInit() { for (auto &t : s_goneTic) t = -1; } } s_goneTicInit;
static int32_t s_stallSince[MAXPLAYERS];        // master: when aggregation first blocked on peer
                                                // (fill deadline + interruption HUD; drops key on
                                                // real-progress silence, not on this)
static int32_t s_peerDownMask;                  // mid-game transport peer-downs, folded into drops

static int32_t s_sessionStartClock;  // stamped at every barrier (warm-up window)

// ── Canonical stream + barrier-free join state ──────────────────────────────
input_t g_netSendRing[MOVEFIFOSIZ];  // slave: local samples staged for the wire + predictor
int32_t g_netSampleHead;             // slave: absolute tic of the next local sample
int32_t g_netFillTics;               // master: synthesized tics (deadline/join fill)
int32_t g_netJoinCatchup;            // joiner/healing guest: applied the snapshot, streaming to live
int32_t g_netDesyncReporters;        // host: bitmask of guests whose DESYNC_REPORT named them diverged
uint32_t s_pendingSpawnMask;         // late-join seats waiting for their first spawn (press open/fire)
uint32_t g_seatDiagUntil;            // wall-ms deadline for the post-seat cursor beat (diagnostic)
int32_t g_netEolFromHost;            // guest: this MODE_EOL was authorized by PACKET_TYPE_EOL
static int32_t s_eolPending;         // guest: between EOL-received and level entry -- drop
                                     // unreliable stream packs (a stale E1L1 snap/paint applied
                                     // in E1L2 teleports the player / corrupts the fresh world)
// Level-transition load grace (see NET_EOL_GRACE). Host side: seats still
// loading the new map (cleared by their first real post-transition record).
// Guest side: the host is loading (cleared by the first M2S after OUR entry --
// residual old-level M2S while s_eolPending must not release it, or a host
// slower than us would still be axed 10s after we enter).
static int32_t s_eolWaitMask;   // host: seats inside the transition grace
static int32_t s_eolGraceClock; // host: when the EOL broadcast armed the grace
static int32_t s_hostEolWait;   // guest: host presumed loading
static int32_t s_hostEolClock;  // guest: when the EOL receipt armed the grace

// NN_TESTSLOWLOAD=<ms> (native builds only): block the game thread at the
// transition exactly like a slow machine's map/art load, so the grace above is
// provable in a harness instead of shipping on faith.
#ifdef NETNATIVE
// MinGW has no usleep; Sleep comes in via compat.h -> windows_inc.h.
static void Net_SleepMS(int ms)
{
# ifdef _WIN32
    Sleep((DWORD)ms);
# else
    usleep((useconds_t)ms * 1000);
# endif
}
#endif
static void Net_TestSlowLoad(const char *who)
{
#ifdef NETNATIVE
    static int ms = -1, pump = -1;
    if (ms < 0)
    {
        char const *e = getenv("NN_TESTSLOWLOAD");
        ms = e ? Batoi(e) : 0;
        e = getenv("NN_TESTSLOWLOAD_PUMP");
        pump = (e && Batoi(e)) ? 1 : 0;
    }
    if (ms > 0)
    {
        // PUMP mode (host only) mimics a REAL load: G_DoLoadScreen/the bonus
        // screen keep pumping events and the packet drain runs long before the
        // host is ready. The dead usleep hid the master-echo leak (a fast
        // guest's READY was answered mid-load and released its barrier;
        // live-reported 2026-08-16). Guest side never pumps here: this runs
        // inside the packet switch and net_poll() would recurse.
        int const doPump = pump && Bstrcmp(who, "host") == 0;
        LOG_F(INFO, "[eol] NN_TESTSLOWLOAD: %s blocking %d ms (simulated map load%s)",
              who, ms, doPump ? ", pumping" : "");
        if (doPump)
        {
            for (int left = ms; left > 0; left -= 50)
            {
                Net_SleepMS(min(left, 50));
                net_poll();
            }
        }
        else
            Net_SleepMS(ms);
        LOG_F(INFO, "[eol] NN_TESTSLOWLOAD: %s resumed", who);
    }
#else
    UNREFERENCED_PARAMETER(who);
#endif
}

// Join boundary table, the exact mirror of s_goneTic: >= 0 names the first tic
// WITH this player. Persists for the session (record membership for tics below
// it stays "absent" forever); -1 = no boundary (present since tic 0). Same
// zero-init trap as s_goneTic -> initialized alongside it below.
static int32_t s_joinTic[MAXPLAYERS];
static int32_t s_joinAnnounceUntil[MAXPLAYERS]; // master: directory-announce window
static int32_t s_joinAwaitReal;      // master: slots seated but no real S2M record yet (fill at once)
static int32_t s_fillActive;         // master: columns being synthesized right now -- once the
                                     // deadline tripped, fill free-runs at full speed until a real
                                     // record lands (else the match would crawl at one tic per
                                     // deadline while the laggard is out)
static int32_t s_lastRealRecvClock[MAXPLAYERS]; // master: last REAL wire progress per slave --
                                                // with fill running, movefifoend grows on its own
                                                // and can no longer feed the health axes
// Host join-flow state machine (one joiner in flight; others stay queued in
// g_netLateJoinMask until the slot clears).
static int     s_joinFlowSlot = -1;  // slot being streamed a snapshot, -1 idle
static int32_t s_joinFlowClock;      // when the current snapshot was sent (re-send pacing)
static int     s_joinFlowTries;      // re-snapshot attempts for the current joiner
static int32_t s_joinFlowBase;       // movefifoplc at snapshot save: the joiner's acks must
                                     // ADVANCE PAST this before the seat may stamp (the ack
                                     // cursor is initialized AT the base, so a bare gap<=8
                                     // check passed instantly and seated a still-loading peer)
// Joiner-side snapshot metadata (set by Net_SnapshotReady from the transport).
static int32_t  s_snapshotPlc;       // host movefifoplc at snapshot save
static int      s_snapshotIsJoin;    // 1 = barrier-free join/catchup, 0 = legacy resync broadcast

// Targeted heal (a JOIN flow for an already-seated guest: same snapshot stream,
// same retry/kick machinery, no seat -- the guest self-resumes at the live edge).
static int      s_joinFlowIsHeal;    // host: the active flow heals a seated guest
static int32_t  s_healBasePlc = -1;  // guest: snapshot tic of the heal being caught up
// Ack-generation fence: unlike a joiner (fresh peer), a heal target has stale
// S2Ms in flight that still ack the PRE-apply stream near live. The monotonic
// ack guard would lift the rebased s_slaveAck back up, complete the flow
// early, and then reject the guest's post-apply (regressed) acks forever --
// a permanent gap-drop wedge. Post-apply S2Ms are unmistakable: the apply
// zeroes the guest's sample cursor, so its packets carry startTic==0 (a live
// mid-session guest's start is its master-acked sample tic, never 0). Ignore
// the heal slot's acks until that marker arrives.
static int      s_healAckFence;      // host: 1 = post-apply acks are flowing
// Soft-correction ladder bookkeeping (machinery lives further down).
static int32_t  s_softStrikes[MAXPLAYERS];
static int32_t  s_softStrikeClock[MAXPLAYERS];
// Tic-stamped soft snap awaiting its consume tic (guest side).
static char     s_pendingSnap[2048]; // [type][count][tic i32][seed i32][grand i32]
                                     // + 16*75B players + [awc]+64*i16 animwall tags
                                     // 16-seat worst case: 14+1200+1+128 = 1343B. The
                                     // old 768B cap silently dropped every pack past
                                     // ~8 seats (remote players froze); the receive
                                     // guard now screams when a pack outgrows this.
static int      s_pendingSnapLen;
static int32_t  s_pendingSnapTic;
// Stream-mode machinery (defined after Net_ApplyPendingStateSnap).
static void Net_StreamAuthoritativeState(void);
static void Net_ApplySpriteStream(const char *buf, int len);
static void Net_ApplyWallStream(const char *buf, int len);
static void Net_ClientPickupScan(void);   // guest client-authoritative item pickup
static void Net_ApplyHitReport(int attacker, int victim, int dmg, int weaponPic);       // host: apply a guest's reported PLAYER hit
static void Net_ApplyEnemyHitReport(int attacker, int idxHint, int x, int y, int z, int dmg, int weaponPic); // host: apply a guest's reported MONSTER hit (by position)
extern "C" void NetMenu_SetStatus(const char *s);   // menus.cpp: the net menu's status line (lobby-wait messaging)
static void Net_ApplySectorStream(const char *buf, int len);
// Guest-reported BREAKABLE/object hits are QUEUED here and drained ON-TICK
// (Net_DrainObjectHits, called from G_DoMoveThings). Running A_DamageObject from
// packet handling spawned/deleted sprites + rolled RNG off-tick and desynced the
// host; deferring to the tick makes it sync-safe.
static int16_t s_objHitVictim[128];
static uint8_t s_objHitAtk[128];
static int     s_objHitN;
// Guest-reported ENEMY hits, likewise QUEUED and drained ON-TICK (Net_DrainEnemyHits,
// right before G_MoveActors): the apply WAKES dormant monsters (changespritestat) and
// writes health, which must land on the authoritative timeline, not mid-packet.
struct NetEnemyHit { int32_t x, y, z; int16_t idx; uint16_t dmg; uint16_t wpic; uint8_t atk; };
static NetEnemyHit s_enemyHit[128];
static int         s_enemyHitN;
int32_t g_hostProbeIdx = -1, g_hostProbePlc = 0;   // NN_TESTKILL host-death probe (test rig only)
// HOST: each guest's authoritative LIVE weapon -- it decides what it fires, not us.
// Stashed off-tick from PACKET_TYPE_WEAPON_STATE, force-applied on-tick before the
// seat's P_ProcessInput (Net_ApplyGuestWeapon). s_gwHave gates it so we never force a
// seat we've heard nothing from.
static int8_t   s_gwWeapon[MAXPLAYERS];
static uint16_t s_gwGot[MAXPLAYERS];
static int16_t  s_gwAmmo[MAXPLAYERS];
static uint8_t  s_gwHave[MAXPLAYERS];
static uint16_t s_gwSeq[MAXPLAYERS];   // newest applied report (unordered channel)
static void Net_SendWeaponState(void);   // guest -> host, per-tic (defined below)
// HOST: each guest's authoritative POSITION -- the guest owns its own movement
// ("trust the client where it ended up"). The host's input-replay copy of a guest
// drifts (clipping/timing skew), and reflecting that drift back as a correction
// was the live "teleporting me to places I shouldn't be". Reports land per-tic
// from PACKET_TYPE_POS_REPORT; Net_ApplyGuestPos snaps the seat right before its
// P_ProcessInput, so the host sim (enemy AI, hazards, its own render of the
// guest) tracks guest truth within one RTT. Between reports the input-replay
// still integrates forward from the last snapped base -- dropped packets coast.
static vec3_t   s_gpPos[MAXPLAYERS];
static vec3_t   s_gpVel[MAXPLAYERS];
static fix16_t  s_gpAng[MAXPLAYERS], s_gpHoriz[MAXPLAYERS];
static int16_t  s_gpSect[MAXPLAYERS];
static int32_t  s_gpSprZ[MAXPLAYERS];
static uint8_t  s_gpHave[MAXPLAYERS];
static uint16_t s_gpSeq[MAXPLAYERS];
static void Net_SendPosReport(void);     // guest -> host, per-tic (defined below)
// SHARED COOP ACCESS (live 2026-08-14: "access cards need to be shared between
// users if either user collects one"). Cards are granted by each sim's LOCAL CON
// (per-player got_access), so the guest's ledger and the host's copy of the guest
// could disagree -- a card door then opened on one screen and refused on the
// other. Now: guests report their earned bits upstream (reliable, idempotent OR);
// the host unions every seat's bits each tic, ORs the union back into every seat,
// and broadcasts it -- one shared key ring for the whole coop session.
static uint8_t s_accSentMask;    // guest: last mask reported upstream
static uint8_t s_accBcastMask;   // host: last union broadcast
static void Net_SendAccessState(void);   // guest -> host, on change (defined below)
void Net_ClientReportEnemyHit(int idxHint, int x, int y, int z, int damage, int weaponPic); // defined below
// NEW_GAME delivery hardening: a real browser can still be BOOTING the wasm
// when the host launches (live: join_ok -> launch within 1s; the guest's
// engine missed the roster packet and entered an empty world). The host
// stores the launch packet (with a per-launch token) and re-sends it to any
// guest that hasn't sent PLAYER_READY; guests dedupe by token so resends are
// idempotent and can never restart an entered level.
static uint8_t  s_newGameToken;
static char     s_newGameBuf[64];
static int      s_newGameLen;
static int32_t  s_newGameClock;
static uint32_t s_newGameAckMask;
static int32_t  s_newGameResendClock[MAXPLAYERS];
static void Net_ResendNewGameIfUnacked(void);

static struct JoinTicInit { JoinTicInit() { for (auto &t : s_joinTic) t = -1; } } s_joinTicInit;

// ── CPU player input synthesis (host only) ──────────────────────────────────
// LOCAL PRNG -- never krand(): bot decisions run on the host only, and a krand
// draw here would advance the shared sim RNG stream on one peer alone (the
// exact desync class this project hunts).
static uint32_t s_botRng = 0xB07D00Du;
static inline uint32_t Bot_Rnd(void)
{
    s_botRng ^= s_botRng << 13; s_botRng ^= s_botRng >> 17; s_botRng ^= s_botRng << 5;
    return s_botRng;
}
static int8_t  s_botStrafeDir[MAXPLAYERS];
static int16_t s_botWanderAng[MAXPLAYERS];
static int16_t s_botThinkHold[MAXPLAYERS];   // reaction: tics until retarget allowed
static int8_t  s_botTarget[MAXPLAYERS];    // PLAYER index the bot is fighting (DM/TDM). -1 none.
// COOP target is a MONSTER, i.e. a SPRITE index -- which overflows the int8
// player target above (MAXSPRITES >> 127), so it needs its own int16 slot. In
// Cooperative the bot is player-BLIND (never targets or revenge-attacks a human
// teammate, user 2026-08-12) and fights this enemy sprite instead. -1 = none.
static int16_t s_botMonTgt[MAXPLAYERS];
// Waypoint-free navigation: stuck detection + wall-bounce + door press.
static vec2_t  s_botLastPos[MAXPLAYERS];
static int16_t s_botStuckTics[MAXPLAYERS];
static int16_t s_botBounceHold[MAXPLAYERS];  // tics left steering to bounceAng
static int16_t s_botBounceAng[MAXPLAYERS];
static int8_t  s_botWasDead[MAXPLAYERS];
static int16_t s_botSpawnRoam[MAXPLAYERS];   // post-respawn target-blind roam
static int8_t  s_botBreakFire[MAXPLAYERS];   // tics of blocker-clearing fire
static int8_t  s_botStuckEpisodes[MAXPLAYERS]; // consecutive traps -> longer bounces
static int16_t s_botNoHitTics[MAXPLAYERS];   // visible-but-unhittable streak (fence camping)
static int32_t s_botLastTDist[MAXPLAYERS];   // pursuit progress: last distance to target
static int32_t s_botNavX[MAXPLAYERS], s_botNavY[MAXPLAYERS], s_botNavZ[MAXPLAYERS];
static int16_t s_botNavSect[MAXPLAYERS];      // routed combat destination layer
static int8_t  s_botNavOn[MAXPLAYERS];
// Latest route verdict for the CURRENT combat target. The verdict is refreshed
// only by a real route query (think cadence), while failedTics advances once
// per generated input tic below. This keeps "40 failed tics" independent of
// skill/reaction cadence without running extra pathfinds or consuming RNG.
static BotRouteFailureState s_botRouteFail[MAXPLAYERS];
static int16_t s_botLastSect[MAXPLAYERS];    // re-plan the route on sector crossings
static uint8_t s_botLive[MAXPLAYERS];       // body has been anchored to a live spawn
static int32_t s_botSepPlc = -1;            // level-scoped separation telemetry
static int32_t s_botCamLogPlc[MAXPLAYERS];  // level-scoped viewscreen telemetry
static int8_t  s_botTurnPref[MAXPLAYERS];    // wall-following handedness (+1/-1)
static int16_t s_botTrapTics[MAXPLAYERS];    // zero-net-displacement streak (hard trap)
static vec2_t  s_botTrapAnchor[MAXPLAYERS];
static int16_t s_botTrapCool[MAXPLAYERS];    // volley cooldown while trapped
static int16_t s_botTrapDir[MAXPLAYERS];     // last free-move heading (escape axis)
static int8_t  s_botTrapRounds[MAXPLAYERS];  // completed escape ladders; 3+ = dormant

// Layered graph navigation state is defined below after the walk probes.
static int8_t  s_botOpenGrace[MAXPLAYERS];   // door-try: keep pushing before bouncing
static uint8_t s_botBurst[MAXPLAYERS];       // fire cadence phase (24 on / 8 off)
static int16_t s_botTargetHold[MAXPLAYERS];  // tics on the same target without a kill

// ── Room-routine goal model (user directive 2026-08-10) ─────────────────────
// "If items are in a room, small chance it tries to pick them up. Otherwise
// its goal should be to exit the room through a door that it didn't enter
// through. If it finds an opponent, engage. Very little jumping."
// The room memory is a per-bot last-visit stamp on every sector: the freshest
// sector behind a portal is where the bot just came FROM, so steering at the
// STALEST portal is exactly "leave through the other door", and over minutes
// the gradient walks the whole map (rooms in Build are clusters of small
// sectors; percolating stale-ward crosses them like rooms).
static int32_t s_botVisitT[MAXPLAYERS][MAXSECTORS];
static int8_t  s_botGoal[MAXPLAYERS];        // 0 none, 1 portal/waypoint, 2 item
static int32_t s_botGoalX[MAXPLAYERS], s_botGoalY[MAXPLAYERS], s_botGoalZ[MAXPLAYERS];
static int16_t s_botGoalSect[MAXPLAYERS];    // sector the goal leads into (-1 = free waypoint)
static int16_t s_botGoalTics[MAXPLAYERS];    // time spent on this errand
static int8_t  s_botGoalDoor[MAXPLAYERS];    // portal is a door sector: press OPEN on approach
static int8_t  s_botGoalCrouch[MAXPLAYERS];  // low clearance portal: duck through
static int16_t s_botGoalItem[MAXPLAYERS];    // sprite index of the item errand
static int16_t s_botItemShun[MAXPLAYERS];    // last item that refused pickup (full hp etc.)
static int16_t s_botPrevSect[MAXPLAYERS];    // sector this room was entered FROM
static int16_t s_botSightTics[MAXPLAYERS];   // tics since the target was last actually seen
static int8_t  s_botPending[MAXPLAYERS];     // candidate being NOTICED (pre-lock awareness)
static int16_t s_botSeeStreak[MAXPLAYERS];   // tics the pending candidate has held LOS
static int16_t s_botLastWacked[MAXPLAYERS];  // last wackedbyactor sprite seen (new wound -> retaliate)
static int32_t s_botJumps[MAXPLAYERS];       // telemetry: total jump presses (the user meter)
static int8_t  s_botGoalSeen[MAXPLAYERS];    // walk line to errand: 0 unknown, 1 clear, -1 blocked
static int8_t  s_botNavSeen[MAXPLAYERS];     // walk line to chase waypoint, same encoding
static int16_t s_botJumpCool[MAXPLAYERS];    // hard floor between jumps (water exempt)
static int16_t s_botLaneAng[MAXPLAYERS];     // furniture-field lane heading
static int16_t s_botLaneHold[MAXPLAYERS];    // tics left committed to the lane
static uint8_t s_botThreadFails[MAXPLAYERS]; // stuck trips while goal line was "clear"
// IMPOSSIBLE-EXIT memory (user 2026-08-12: "stop it trying to go to impossible
// to reach exits"). The explore planner scores portals by staleness, and a
// sector the bot CAN'T reach is never stamped -> it stays the stalest thing in
// the room and gets re-picked forever. Two defenses: (1) a per-errand progress
// watch abandons a goal that stops closing (see the upkeep block) in ~3s
// instead of the 13s timeout; (2) this small per-bot ring remembers the target
// sectors those abandonments hit, so the planner steers to a DIFFERENT door for
// a while. Ring, not a full [MAXSECTORS] table: a room has a handful of exits.
#define BOT_DEAD_N 8
static int16_t s_botDeadSect[MAXPLAYERS][BOT_DEAD_N]; // recently-unreachable target sectors
static int16_t s_botDeadCool[MAXPLAYERS][BOT_DEAD_N]; // tics of avoidance left (0 = slot free)
static int32_t s_botGoalNear[MAXPLAYERS];    // closest we have come to the current goal
static int16_t s_botGoalStall[MAXPLAYERS];   // tics since that closest approach improved
// ── Combat / aim model (OpenArena-mined, wave 3b). Per-seat static state, no
// heap; RESET at BOTH the Net_SeatBots init loop AND the respawn block, same as
// every other body-state array. Concept ports of ai_dmq3.c BotAimAtEnemy /
// BotCheckAttack + ai_main.c BotChangeViewAngles -- these only SET input bits
// and angles; authoritative weapon fire lives elsewhere.
static int16_t s_botFireSight[MAXPLAYERS];    // (#1) tics the CURRENT target has been
                                              // continuously visible -- FIRE waits for
                                              // this >= reactTics[skill] (their enemysight
                                              // reaction gate, BotCheckAttack :3637)
static int32_t s_botTgtSX[MAXPLAYERS], s_botTgtSY[MAXPLAYERS]; // (#2) last velocity-snapshot pos
static int32_t s_botTgtVX[MAXPLAYERS], s_botTgtVY[MAXPLAYERS]; // (#2/#3) target vel estimate, units/tic
static int32_t s_botTgtSnap[MAXPLAYERS];      // (#2) movefifoplc of the last snapshot
static int8_t  s_botTgtVValid[MAXPLAYERS];    // (#2) a baseline snapshot exists
static int16_t s_botAimDegrade[MAXPLAYERS];   // (#2) tics of degraded accuracy after a jink
static int32_t s_botViewVel[MAXPLAYERS];      // (#5) second-order view-angle velocity (ang/tic)
static int16_t s_botThrWait[MAXPLAYERS];      // (#8) FIRETHROTTLE: tics fire is throttled OFF
static int16_t s_botThrShoot[MAXPLAYERS];     // (#8) tics of the current shoot window left
static int16_t s_botStrafeTic[MAXPLAYERS];    // (#7) strafe-change interval accumulator
static int8_t  s_botStrafeFail[MAXPLAYERS];   // (#7) consecutive blocked strafe attempts (0..2)
static int8_t  s_botWantStrafe[MAXPLAYERS];   // (#7) commanded a strafe last tic
// ── Two-tier goal stack: map-wide LTG + errand-as-NBG (OpenArena-mined,
// wave 3a). The explore planner only ever scored the CURRENT sector's portals
// and the errand died on every sector crossing, so bots orbited their spawn
// room by construction. Port of OA's committed long-term goal
// (BotGetItemLongTermGoal ai_dmnet.c:280 / BotChooseLTGItem be_ai_goal.c:1282):
// one map-wide target held ~390-520 tics (their 20s at 26 tics/s), scored
// desirability/travelTime over ALL pickups + far stale sectors; the existing
// errand machinery rides on top as the nearby-goal (NBG) layer, and when a
// detour or fight ends the movement RESUMES the committed LTG instead of
// re-rolling a neighbor portal. Two struct layers = two sets of statics, no
// heap. NN_BOTLTG=0 is the kill-switch back to the old single-slot brain
// (also the baseline leg of the roaming smoke); default ON.
static int32_t Bot_LtgFromEnv(void)
{
    const char *e = getenv("NN_BOTLTG");
    return (e && *e) ? Batoi(e) : 1;
}
static int32_t g_botLtgOn = Bot_LtgFromEnv();
// TEST KNOBS (wave 3b forensics; the native autolaunch host never calls
// Net_SeatBots, so the CPU skill is otherwise pinned at its g_netBotSkill
// default of 2 and the difficulty COLUMNS can't be exercised in a headless
// leg). NN_BOTSKILL forces the skill column 0..3; NN_BOTALERTCAP overrides the
// per-skill acquisition-radius cap so the [alert] acquired=0 proof is
// reachable in a short window. Both default OFF (-1 / 0). Read once at load,
// exactly like g_botLtgOn -- cross-peer identical (same env on every peer).
static int32_t Bot_SkillFromEnv(void)    { const char *e = getenv("NN_BOTSKILL");    return (e && *e) ? clamp(Batoi(e), 0, 3) : -1; }
static int32_t Bot_AlertCapFromEnv(void) { const char *e = getenv("NN_BOTALERTCAP"); return (e && *e) ? Batoi(e) : 0; }
static int32_t g_botSkillEnv = Bot_SkillFromEnv();
static int32_t g_botAlertCap = Bot_AlertCapFromEnv();

// NN_TESTFRAG=<seat>: host-only forced periodic frag of one seat so the DM
// respawn path (death CON -> VM_ResetPlayer -> P_ResetMultiPlayer ->
// P_MoveToRandomSpawnPoint) can be gated DETERMINISTICALLY without waiting on
// bots to actually kill each other. Arms lethal htextra (the blessed
// host-damage mechanism -- never writes extra directly), attributed to the
// host; the normal death->resetplayer flow then runs the respawn/roll. Inert
// unless the env is set (like NN_TESTKILL/NN_TESTEOL). Native harness only.
void Net_TestFragTick(void)
{
    static int s_seat = -2;
    if (s_seat == -2) { const char *e = getenv("NN_TESTFRAG"); s_seat = (e && *e) ? atoi(e) : -1; }
    if (s_seat < 0 || (unsigned)s_seat >= MAXPLAYERS)
        return;
    static int32_t s_last = -1000;
    if (movefifoplc - s_last < 180)   // ~7s cadence: die, respawn, settle, repeat
        return;
    auto const ps = g_player[s_seat].ps;
    if (ps == NULL || !g_player[s_seat].connected || ps->dead_flag
        || (unsigned)ps->i >= MAXSPRITES || sprite[ps->i].extra <= 0)
        return;
    s_last = movefifoplc;
    int const tgt = ps->i;
    actor[tgt].htextra  = (int16_t)(ps->max_player_health + 100);
    actor[tgt].htpicnum = SHOTSPARK1;
    actor[tgt].htowner  = (g_player[connecthead].ps != NULL) ? (int16_t)g_player[connecthead].ps->i : (int16_t)tgt;
    LOG_F(INFO, "[testfrag] armed lethal seat=%d spr=%d plc=%d", s_seat, tgt, (int)movefifoplc);
}
static int32_t s_botLtgX[MAXPLAYERS], s_botLtgY[MAXPLAYERS], s_botLtgZ[MAXPLAYERS];
static int16_t s_botLtgSect[MAXPLAYERS];     // target sector of the committed LTG
static int8_t  s_botLtgKind[MAXPLAYERS];     // 0 none, 1 roam anchor, 2 item
static int16_t s_botLtgItem[MAXPLAYERS];     // sprite index when kind==2
static int32_t s_botLtgUntil[MAXPLAYERS];    // movefifoplc commit deadline
static int8_t  s_botLtgFails[MAXPLAYERS];    // stall aborts of this LTG's body
static int8_t  s_botGoalIsLtg[MAXPLAYERS];   // current errand IS the LTG body:
                                             // survives sector crossings, resumes
                                             // after fights/detours
static vec2_t  s_botLtgAnchor[MAXPLAYERS];   // movement-watch anchor for the body:
                                             // cross-map routes legitimately walk
                                             // AWAY from the goal (around blocks),
                                             // so its stall test is "stopped
                                             // MOVING", not "stopped approaching"
static int8_t  s_botLtgLocal[MAXPLAYERS];    // explore beats owed at the LTG
                                             // destination: march, then sniff
                                             // around the arrival cluster before
                                             // the next cross-map commit
// ITEM RESPAWN-TIME MEMORY (their BotAddToAvoidGoals/BotAvoidGoalTime,
// be_ai_goal.c:1425/:1367): ring of items predicted absent until a tic.
// Stamped when an item LTG is CHOSEN; refreshed when the sprite is OBSERVED
// picked up (in respawn mode a taken item keeps its slot with cstat bit 32768
// -- the pending-respawn observable). Expiry rides g_itemRespawnTime, the CON
// RESPAWNITEMTIME gamevar the sim itself counts against (actors.cpp:4804).
#define BOT_IAVOID_N 8
static int16_t s_botItemAvoid[MAXPLAYERS][BOT_IAVOID_N];    // sprite idx, -1 free
static int32_t s_botItemAvoidTil[MAXPLAYERS][BOT_IAVOID_N]; // predicted respawn tic
// PER-EDGE AVOID-REACH (their avoidreach tries+TTL split, be_ai_move.c:90:
// AVOIDREACH_TIME 6s / AVOIDREACH_TRIES 4). Replaces the sector-wide
// 2400-tic blacklist as the PRIMARY stall response: a (fromSect,toSect)
// crossing only counts as dead after repeated failures inside a short
// window, so one snag no longer bans a whole room for 80 seconds. The old
// s_botDeadSect ring survives as the ESCALATION tier (5+ tries).
#define BOT_EAVOID_N 8
static int16_t s_botEdgeFrom[MAXPLAYERS][BOT_EAVOID_N];
static int16_t s_botEdgeTo[MAXPLAYERS][BOT_EAVOID_N];
static int8_t  s_botEdgeTries[MAXPLAYERS][BOT_EAVOID_N];
static int32_t s_botEdgeUntil[MAXPLAYERS][BOT_EAVOID_N];    // movefifoplc expiry
// CHASE LAST-SEEN (their lastenemyorigin, ai_dmnet.c:2147/:2296): position
// snapshot taken only while the target is VISIBLE; once sight breaks the
// bot navigates to the SNAPSHOT instead of wall-tracking the live position.
static int32_t s_botSeenX[MAXPLAYERS], s_botSeenY[MAXPLAYERS];
static int32_t s_botSeenZ[MAXPLAYERS];
static int16_t s_botSeenSect[MAXPLAYERS];
static int8_t  s_botSeenValid[MAXPLAYERS];
// WEDGE-SPOT ring: POSITIONS where a committed march died of zero movement.
// The per-edge ring above cannot learn these -- the E1L1 chair fields and
// alley pockets are INTRA-sector, so every failure attributes to a different
// (from,to) pair and the 3-try activation never fires (measured: 19 stall
// kills in one leg, no edge past tries=2, the bot re-marching through the
// same field to fresh targets all match). Tile-level cure, OA's avoid-spot
// concept at minimum size: stamp the spot on a certified wedge; the router
// and the LTG flood refuse to ENTER tiles near an active spot (leaving is
// always allowed, or a bot standing in one could never path out).
#define BOT_WSPOT_N 4
#define BOT_WSPOT_R 768                       // ~1.5 tiles
static int32_t s_botWspotX[MAXPLAYERS][BOT_WSPOT_N];
static int32_t s_botWspotY[MAXPLAYERS][BOT_WSPOT_N];
static int16_t s_botWspotSect[MAXPLAYERS][BOT_WSPOT_N];
static int32_t s_botWspotUntil[MAXPLAYERS][BOT_WSPOT_N];
// Roam telemetry: distinct sectors entered this level, per seat ([roam]),
// plus honesty meters -- tics spent effectively stationary (the wedge bill)
// and tics steered by nothing but the wander fallback (the idle bill).
static uint8_t s_botRoamBm[MAXPLAYERS][MAXSECTORS >> 3];
static int32_t s_botRoamCnt[MAXPLAYERS];
static int32_t s_botRoamLogPlc[MAXPLAYERS];
static int32_t s_botStillTics[MAXPLAYERS];    // alive tics with step < 16 units
static int32_t s_botIdleTics[MAXPLAYERS];     // tics on the wander fallback
static int8_t  s_botTeamLogged[MAXPLAYERS];  // one-shot [team] forensic per seat
// ── Inventory / jetpack state (netduke32-mined, 2026-08-18) ─────────────────
// SK_JETPACK is a TOGGLE edge-triggered through interface_toggle
// (P_HandleSharedKeys, sector.cpp:2699-2701): a held bit fires exactly once
// and then latches until released, so activation is a one-tic TAP guarded by
// a cooldown. s_botJetHold is the jet-nav engagement window; while it runs,
// the block below owns the vertical keys (JUMP=ascend / CROUCH=descend, the
// sim's own jetpack controls) toward the current goal's z.
static int16_t s_botJetHold[MAXPLAYERS];     // >0: jetpack vertical nav engaged
static int8_t  s_botJetCool[MAXPLAYERS];     // tics until another SK_JETPACK tap
// Forensics tallies for the [botinv] line: activations per seat, so gates can
// assert the inventory features actually fire.
static uint16_t s_botMedUses[MAXPLAYERS];    // SK_MEDKIT presses
static uint16_t s_botSterUses[MAXPLAYERS];   // SK_STEROIDS presses
static uint16_t s_botJetActs[MAXPLAYERS];    // jetpack ON taps
static int32_t  s_botInvLogPlc[MAXPLAYERS];  // [botinv] rate limiter (movefifoplc)

// Probe the straight WALK line to (tx,ty): can the feet take the direct path?
// Follow-until-clear steering hangs off this -- goal-directed turning is only
// allowed while the line is open; otherwise the wall-follow ladder keeps
// tracing the obstacle. Probing only at plan time was the v18a failure: the
// bot drifts, the line rotates into a fence, and steering grinds it there
// at ~3 units/tic with the stuck ladder thrashing against the goal turn.
// Sprite-AWARE knee-height ray fan: the longest open lane leaning toward
// the goal. Furniture fields (theater chairs, alley crates) give short rays
// down blocked rows and long rays down the aisles -- the player's read.
static int Bot_PickLane(DukePlayer_t *ps, int32_t gx, int32_t gy)
{
    int const gb = getangle(gx - ps->pos.x, gy - ps->pos.y);
    vec3_t knee = ps->pos;
    knee.z += (24 << 8);                    // under chair backs, over floor
    int32_t bestScore = -1; int bestDir = gb;
    for (int d = 0; d < 16; d++)
    {
        int const da = (d * 128) & 2047;
        hitdata_t lh = {};
        hitscan(&knee, ps->cursectnum, sintable[(da + 512) & 2047],
                sintable[da & 2047], 0, &lh, CLIPMASK0);
        int32_t len = klabs(lh.xyz.x - ps->pos.x) + klabs(lh.xyz.y - ps->pos.y);
        if (len > 3072) len = 3072;
        int const adist = klabs((((da - gb) + 1024) & 2047) - 1024);
        int32_t const w = 1024 - (adist * 3) / 4;    // goalward bias
        int32_t const sc = len * w;
        if (sc > bestScore) { bestScore = sc; bestDir = da; }
    }
    return bestDir;
}

// Core walk-line test from an arbitrary standpoint (feeds both live steering
// and NAV GRAPH construction). Wall ray is WALLS ONLY -- blocking furniture
// does not block WALKING (clipmove slides around sprite clip circles; with
// sprites in the mask every line through the theater chair field read
// "blocked" and the wall-follow orbited it forever). The foot march catches
// what the eye-level ray sails over: knee walls, parapets, raised sills
// (v18c: probe "clear", walk pinned at ~3 units/tic against a rooftop lip).
static int Bot_SectorIsDoor(int s);
static int Bot_LineWalkFrom(int16_t sect, int32_t x, int32_t y, int32_t z,
                            int32_t tx, int32_t ty)
{
    if ((unsigned)sect >= (unsigned)numsectors)
        return 0;
    int32_t const wd = klabs(tx - x) + klabs(ty - y);
    if (wd < 64)
        return 1;
    int const a = getangle(tx - x, ty - y);
    vec3_t sv = { x, y, z };
    hitdata_t h = {};
    hitscan(&sv, sect, sintable[(a + 512) & 2047],
            sintable[a & 2047], 0, &h, ((1) << 16));
    if (klabs(h.xyz.x - x) + klabs(h.xyz.y - y) + 384 < wd)
        return 0;
    int32_t const dx = tx - x, dy = ty - y;
    // Sample FINELY -- ~every 64 units -- so a STAIRCASE (small steps close
    // together) is walked step by step instead of read as one tall wall by a
    // coarse sample that straddles several steps. Per-step limit is the
    // player's real autostep (20<<8, premap.cpp:738); anything taller in a
    // single 64-unit sample is a true wall.
    int const steps = clamp((int)(wd >> 6), 1, 48);
    int16_t cs = sect;
    int32_t lastF = getflorzofslope(cs, x, y);
    for (int i = 1; i <= steps; i++)
    {
        int32_t const sx = x + (int32_t)(((int64_t)dx * i) / steps);
        int32_t const sy = y + (int32_t)(((int64_t)dy * i) / steps);
        updatesector(sx, sy, &cs);
        if (cs < 0)
            return 0;
        int32_t const f = getflorzofslope(cs, sx, sy);
        if (lastF - f > (20 << 8))      // z grows down: a step UP beyond autostep
            return 0;
        lastF = f;
    }
    return 1;
}
static int Bot_LineWalkable(DukePlayer_t *ps, int32_t tx, int32_t ty)
{
    return Bot_LineWalkFrom(ps->cursectnum, ps->pos.x, ps->pos.y, ps->pos.z, tx, ty);
}

// ── Bounded sparse layered navigation graph ─────────────────────────────────
// Integration note: oldnet implementations that add their own bot reset/lifecycle
// contract must clear the route/goal/last-seen/wedge fields declared around this
// graph; this phase deliberately does not implement lifecycle policy.
// XY cells are only a deterministic spatial index.  A walkable floor is a node
// keyed by (cell, sector), so overlapping Build sectors never collapse into the
// single z-blind tile that updatesector() happened to choose.  Nodes carry the
// sampled layer explicitly and edges are directed: climbing and dropping have
// different reachability rules.
#define NVG_TILE       512
#define NVG_MAXW       288
#define NVG_MAXH       288
#define NVG_MAXCELLS   (NVG_MAXW * NVG_MAXH)
#define NVG_MAXNODES   16384
#define NVG_MAXARCS    (NVG_MAXNODES * 8)
#define NVG_INVALID    UINT32_MAX
#define NVG_F_DOOR     1u
#define NVG_F_CRAWL    2u
#define NVG_MAX_ROUTE  64

struct NvgNode
{
    int32_t floorZ, ceilZ;
    uint32_t firstArc;
    int32_t cell;
    int16_t sector;
    uint8_t flags;
};
struct NvgArc { uint32_t to, next; };

static int32_t  s_nvgMinX, s_nvgMinY;
static int      s_nvgW, s_nvgH;
static int32_t  s_nvgStamp = -1;
static uint32_t s_nvgNodeCount, s_nvgArcCount;
static int      s_nvgReady;
static int32_t  s_nvgCellHead[NVG_MAXCELLS];
static int32_t  s_nvgCellNext[NVG_MAXNODES];
static NvgNode  s_nvgNode[NVG_MAXNODES];
static NvgArc   s_nvgArc[NVG_MAXARCS];
static uint32_t s_nvgSectorFirst[MAXSECTORS];
static uint32_t s_nvgSectorCount[MAXSECTORS];

static inline int32_t Nvg_CX(int tx) { return s_nvgMinX + tx * NVG_TILE + NVG_TILE / 2; }
static inline int32_t Nvg_CY(int ty) { return s_nvgMinY + ty * NVG_TILE + NVG_TILE / 2; }
static inline int32_t Nvg_NodeX(uint32_t node)
{
    return Nvg_CX(s_nvgNode[node].cell % s_nvgW);
}
static inline int32_t Nvg_NodeY(uint32_t node)
{
    return Nvg_CY(s_nvgNode[node].cell / s_nvgW);
}
static inline int Nvg_TileOf(int32_t x, int32_t y)
{
    int64_t const rx = (int64_t)x - s_nvgMinX, ry = (int64_t)y - s_nvgMinY;
    if (rx < 0 || ry < 0)
        return -1;
    int64_t const tx = rx / NVG_TILE, ty = ry / NVG_TILE;
    if (tx >= s_nvgW || ty >= s_nvgH)
        return -1;
    return (int)(ty * s_nvgW + tx);
}

// Does segment (x1,y1)-(x2,y2) cross a WALKING-blocking wall of sector s?
// Endpoint contact remains blocking.  If both orientation pairs are zero the
// segments are collinear, and closed overlap is required on BOTH projections:
// a wall elsewhere on the same infinite line must not block this edge.
static int Nvg_ClosedIntervalsOverlap(int32_t a1, int32_t a2,
                                      int32_t b1, int32_t b2)
{
    return max(min(a1, a2), min(b1, b2)) <= min(max(a1, a2), max(b1, b2));
}

static int Nvg_SegBlocked(int s, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    int const wend = sector[s].wallptr + sector[s].wallnum;
    for (int w = sector[s].wallptr; w < wend; w++)
    {
        if (wall[w].nextsector >= 0 && !(wall[w].cstat & 1))
            continue;
        int32_t const ax = wall[w].x, ay = wall[w].y;
        int32_t const bx = wall[wall[w].point2].x, by = wall[wall[w].point2].y;
        int64_t const d1 = (int64_t)(bx - ax) * (y1 - ay) - (int64_t)(by - ay) * (x1 - ax);
        int64_t const d2 = (int64_t)(bx - ax) * (y2 - ay) - (int64_t)(by - ay) * (x2 - ax);
        if ((d1 > 0 && d2 > 0) || (d1 < 0 && d2 < 0))
            continue;
        int64_t const d3 = (int64_t)(x2 - x1) * (ay - y1) - (int64_t)(y2 - y1) * (ax - x1);
        int64_t const d4 = (int64_t)(x2 - x1) * (by - y1) - (int64_t)(y2 - y1) * (bx - x1);
        if ((d3 > 0 && d4 > 0) || (d3 < 0 && d4 < 0))
            continue;
        if (d1 == 0 && d2 == 0 && d3 == 0 && d4 == 0)
        {
            bool const xOverlap = Nvg_ClosedIntervalsOverlap(ax, bx, x1, x2);
            bool const yOverlap = Nvg_ClosedIntervalsOverlap(ay, by, y1, y2);
            if (!xOverlap || !yOverlap)
                continue;
        }
        return 1;
    }
    return 0;
}

#if defined(NETNATIVE)
extern "C" int Net_TestNavCollinear(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                    int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    int64_t const d1 = (int64_t)(bx - ax) * (y1 - ay) - (int64_t)(by - ay) * (x1 - ax);
    int64_t const d2 = (int64_t)(bx - ax) * (y2 - ay) - (int64_t)(by - ay) * (x2 - ax);
    int64_t const d3 = (int64_t)(x2 - x1) * (ay - y1) - (int64_t)(y2 - y1) * (ax - x1);
    int64_t const d4 = (int64_t)(x2 - x1) * (by - y1) - (int64_t)(y2 - y1) * (bx - x1);
    if (d1 || d2 || d3 || d4)
        return -1;
    return Nvg_ClosedIntervalsOverlap(ax, bx, x1, x2)
        && Nvg_ClosedIntervalsOverlap(ay, by, y1, y2);
}
#endif

static int Nvg_AddArc(uint32_t from, uint32_t to)
{
    for (uint32_t a = s_nvgNode[from].firstArc; a != NVG_INVALID; a = s_nvgArc[a].next)
        if (s_nvgArc[a].to == to)
            return 1;
    if (s_nvgArcCount >= NVG_MAXARCS)
        return 0;
    s_nvgArc[s_nvgArcCount].to   = to;
    s_nvgArc[s_nvgArcCount].next = s_nvgNode[from].firstArc;
    s_nvgNode[from].firstArc     = s_nvgArcCount++;
    return 1;
}

static int Nvg_SectorPortalNear(int fromSector, int toSector,
                                int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    int const wend = sector[fromSector].wallptr + sector[fromSector].wallnum;
    for (int w = sector[fromSector].wallptr; w < wend; ++w)
    {
        if (wall[w].nextsector != toSector || (wall[w].cstat & 1))
            continue;
        int32_t const mx = (wall[w].x + wall[wall[w].point2].x) >> 1;
        int32_t const my = (wall[w].y + wall[wall[w].point2].y) >> 1;
        if (klabs(mx - ((x0 + x1) >> 1)) + klabs(my - ((y0 + y1) >> 1)) <= NVG_TILE)
            return 1;
    }
    return 0;
}

// Fine climb that never asks updatesector() to choose a layer.  Same-sector
// samples stay in that sector.  A portal edge changes sectors at the actual
// shared wall intersection, so overlapping polygons cannot switch early just
// because inside() is true in both layers.
static int Nvg_PortalFraction(int16_t fromSector, int16_t toSector,
                              int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                              int64_t *numOut, int64_t *denOut)
{
    int64_t bestNum = INT64_MAX, bestDen = 1;
    int const wend = sector[fromSector].wallptr + sector[fromSector].wallnum;
    int64_t const rx = (int64_t)x1 - x0, ry = (int64_t)y1 - y0;
    for (int w = sector[fromSector].wallptr; w < wend; ++w)
    {
        if (wall[w].nextsector != toSector || (wall[w].cstat & 1))
            continue;
        int64_t const qx = wall[w].x, qy = wall[w].y;
        int64_t const sx = (int64_t)wall[wall[w].point2].x - qx;
        int64_t const sy = (int64_t)wall[wall[w].point2].y - qy;
        int64_t den = rx * sy - ry * sx;
        if (den == 0)
            continue;
        int64_t num = (qx - x0) * sy - (qy - y0) * sx;
        int64_t unum = (qx - x0) * ry - (qy - y0) * rx;
        if (den < 0) { den = -den; num = -num; unum = -unum; }
        if (num < 0 || num > den || unum < 0 || unum > den)
            continue;
        if (bestNum == INT64_MAX || num * bestDen < bestNum * den)
            { bestNum = num; bestDen = den; }
    }
    if (bestNum == INT64_MAX)
        return 0;
    *numOut = bestNum; *denOut = bestDen;
    return 1;
}

static int Nvg_FineClimbLayered(int16_t fromSector, int16_t toSector,
                                int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    int32_t const dx = x1 - x0, dy = y1 - y0;
    int32_t const wd = klabs(dx) + klabs(dy);
    if (wd < 32)
        return 1;
    int64_t portalNum = 0, portalDen = 1;
    if (fromSector != toSector
        && !Nvg_PortalFraction(fromSector, toSector, x0, y0, x1, y1, &portalNum, &portalDen))
        return 0;
    int const steps = clamp((int)(wd >> 6), 1, 16);
    int32_t lastF = getflorzofslope(fromSector, x0, y0);
    for (int i = 1; i <= steps; ++i)
    {
        int32_t const sx = x0 + (int32_t)(((int64_t)dx * i) / steps);
        int32_t const sy = y0 + (int32_t)(((int64_t)dy * i) / steps);
        int16_t const cur = (fromSector == toSector || (int64_t)i * portalDen < portalNum * steps)
                            ? fromSector : toSector;
        if (inside(sx, sy, cur) != 1)
            return 0;
        int32_t const f = getflorzofslope(cur, sx, sy);
        int32_t const c = getceilzofslope(cur, sx, sy);
        if (!Bot_SectorIsDoor(cur) && f - c < (26 << 8))
            return 0;
        if (lastF - f > (20 << 8))
            return 0;
        lastF = f;
    }
    return 1;
}

static int Nvg_AddDirectedIfWalkable(uint32_t from, uint32_t to)
{
    NvgNode const &a = s_nvgNode[from];
    NvgNode const &b = s_nvgNode[to];
    int32_t const ax = Nvg_NodeX(from), ay = Nvg_NodeY(from);
    int32_t const bx = Nvg_NodeX(to),   by = Nvg_NodeY(to);
    if (a.sector != b.sector
        && !Nvg_SectorPortalNear(a.sector, b.sector, ax, ay, bx, by))
        return 1;                       // stacked floors are not portals
    if (a.sector != b.sector && !(a.flags & NVG_F_DOOR) && !(b.flags & NVG_F_DOOR))
    {
        // The vertical intersection of the two explicit sectors at the real
        // portal must fit a body.  Center-only floor tests miss closed/shallow
        // portals when either side slopes.
        int32_t const mx = (ax + bx) >> 1, my = (ay + by) >> 1;
        int32_t const af = getflorzofslope(a.sector, mx, my);
        int32_t const bf = getflorzofslope(b.sector, mx, my);
        int32_t const ac = getceilzofslope(a.sector, mx, my);
        int32_t const bc = getceilzofslope(b.sector, mx, my);
        if (min(af, bf) - max(ac, bc) < (26 << 8))
            return 1;
    }
    if (Nvg_SegBlocked(a.sector, ax, ay, bx, by))
        return 1;
    if (a.sector != b.sector && Nvg_SegBlocked(b.sector, ax, ay, bx, by))
        return 1;
    // Destination must fit a standing player unless it is an intentional crawl
    // node or a door (doors can be closed while the graph is built).
    if (!(b.flags & (NVG_F_DOOR | NVG_F_CRAWL)) && b.floorZ - b.ceilZ < (72 << 8))
        return 1;
    int32_t const rise = a.floorZ - b.floorZ;  // z grows down: positive is up
    if (rise > (20 << 8)
        && (rise > (200 << 8)
            || !Nvg_FineClimbLayered(a.sector, b.sector, ax, ay, bx, by)))
        return 1;
    return Nvg_AddArc(from, to);
}

static void Nvg_Disable(const char *why, int64_t a, int64_t b)
{
    s_nvgReady = 0;
    s_nvgW = s_nvgH = 0;
    s_nvgNodeCount = s_nvgArcCount = 0;
    LOG_F(ERROR, "nav: disabled (%s: %" PRId64 ", %" PRId64 ")", why, a, b);
}

static void Bot_NavBuild(void)
{
    uint64_t const buildStart = timerGetPerformanceCounter();
    s_nvgReady = 0;
    s_nvgNodeCount = s_nvgArcCount = 0;
    int32_t minx = INT32_MAX, miny = INT32_MAX, maxx = INT32_MIN, maxy = INT32_MIN;
    for (int w = 0; w < numwalls; ++w)
    {
        minx = min(minx, (int32_t)wall[w].x); maxx = max(maxx, (int32_t)wall[w].x);
        miny = min(miny, (int32_t)wall[w].y); maxy = max(maxy, (int32_t)wall[w].y);
    }
    if (minx > maxx)
        { Nvg_Disable("empty map", 0, 0); return; }

    int64_t const rawBaseX = (int64_t)minx - NVG_TILE;
    int64_t const rawBaseY = (int64_t)miny - NVG_TILE;
    int64_t const baseX = (rawBaseX / NVG_TILE) * NVG_TILE;
    int64_t const baseY = (rawBaseY / NVG_TILE) * NVG_TILE;
    int64_t const width = ((int64_t)maxx - baseX) / NVG_TILE + 2;
    int64_t const height = ((int64_t)maxy - baseY) / NVG_TILE + 2;
    if (baseX < INT32_MIN || baseX > INT32_MAX || baseY < INT32_MIN || baseY > INT32_MAX
        || width <= 0 || height <= 0 || width > NVG_MAXW || height > NVG_MAXH
        || width > NVG_MAXCELLS / height)
    {
        Nvg_Disable("grid dimensions", width, height);
        return;
    }
    s_nvgMinX = (int32_t)baseX; s_nvgMinY = (int32_t)baseY;
    s_nvgW = (int)width; s_nvgH = (int)height;
    int const total = s_nvgW * s_nvgH;
    for (int c = 0; c < total; ++c)
        s_nvgCellHead[c] = -1;

    // Deterministic node IDs: sector order, then cell row-major within the
    // sector's bounds.  No carried updatesector seed can make the chosen layer
    // depend on the prior tile.
    for (int s = 0; s < numsectors; ++s)
    {
        s_nvgSectorFirst[s] = s_nvgNodeCount;
        int32_t sminx = INT32_MAX, sminy = INT32_MAX, smaxx = INT32_MIN, smaxy = INT32_MIN;
        int const wend = sector[s].wallptr + sector[s].wallnum;
        for (int w = sector[s].wallptr; w < wend; ++w)
        {
            sminx = min(sminx, (int32_t)wall[w].x); smaxx = max(smaxx, (int32_t)wall[w].x);
            sminy = min(sminy, (int32_t)wall[w].y); smaxy = max(smaxy, (int32_t)wall[w].y);
        }
        int const tx0 = clamp((int)(((int64_t)sminx - s_nvgMinX - NVG_TILE / 2 + NVG_TILE - 1) / NVG_TILE), 0, s_nvgW - 1);
        int const ty0 = clamp((int)(((int64_t)sminy - s_nvgMinY - NVG_TILE / 2 + NVG_TILE - 1) / NVG_TILE), 0, s_nvgH - 1);
        int const tx1 = clamp((int)(((int64_t)smaxx - s_nvgMinX - NVG_TILE / 2) / NVG_TILE), 0, s_nvgW - 1);
        int const ty1 = clamp((int)(((int64_t)smaxy - s_nvgMinY - NVG_TILE / 2) / NVG_TILE), 0, s_nvgH - 1);
        for (int ty = ty0; ty <= ty1; ++ty)
        for (int tx = tx0; tx <= tx1; ++tx)
        {
            int const cell = ty * s_nvgW + tx;
            int32_t const x = Nvg_CX(tx), y = Nvg_CY(ty);
            if (inside(x, y, (int16_t)s) != 1)
                continue;
            int const isDoor = Bot_SectorIsDoor(s);
            int32_t const floorZ = getflorzofslope((int16_t)s, x, y);
            int32_t const ceilZ  = getceilzofslope((int16_t)s, x, y);
            int32_t const gap = floorZ - ceilZ;
            if (!isDoor && gap < (26 << 8))
                continue;
            if (s_nvgNodeCount >= NVG_MAXNODES)
                { Nvg_Disable("node capacity", s_nvgNodeCount + 1, NVG_MAXNODES); return; }
            uint32_t const n = s_nvgNodeCount++;
            s_nvgNode[n].floorZ  = floorZ;
            s_nvgNode[n].ceilZ   = ceilZ;
            s_nvgNode[n].firstArc = NVG_INVALID;
            s_nvgNode[n].cell    = cell;
            s_nvgNode[n].sector  = (int16_t)s;
            s_nvgNode[n].flags   = (uint8_t)((isDoor ? NVG_F_DOOR : 0)
                                             | (!isDoor && gap < (72 << 8) ? NVG_F_CRAWL : 0));
            s_nvgCellNext[n]     = s_nvgCellHead[cell];
            s_nvgCellHead[cell]  = (int32_t)n;
        }
        s_nvgSectorCount[s] = s_nvgNodeCount - s_nvgSectorFirst[s];
    }

    // Same-layer cell edges plus declared portal transitions only.  Trying every
    // node pair in adjacent cells is bounded (E1 cells have very few layers) and
    // avoids an auxiliary dense per-layer array.
    static int const dx2[2] = { 1, 0 };
    static int const dy2[2] = { 0, 1 };
    for (int ty = 0; ty < s_nvgH; ++ty)
    for (int tx = 0; tx < s_nvgW; ++tx)
    {
        int const cell = ty * s_nvgW + tx;
        for (int d = 0; d < 2; ++d)
        {
            int const nx = tx + dx2[d], ny = ty + dy2[d];
            if (nx >= s_nvgW || ny >= s_nvgH)
                continue;
            int const otherCell = ny * s_nvgW + nx;
            for (int32_t ai = s_nvgCellHead[cell]; ai >= 0; ai = s_nvgCellNext[ai])
            for (int32_t bi = s_nvgCellHead[otherCell]; bi >= 0; bi = s_nvgCellNext[bi])
            {
                if (s_nvgNode[ai].sector != s_nvgNode[bi].sector
                    && !Nvg_SectorPortalNear(s_nvgNode[ai].sector, s_nvgNode[bi].sector,
                                             Nvg_NodeX(ai), Nvg_NodeY(ai), Nvg_NodeX(bi), Nvg_NodeY(bi))
                    && !Nvg_SectorPortalNear(s_nvgNode[bi].sector, s_nvgNode[ai].sector,
                                             Nvg_NodeX(bi), Nvg_NodeY(bi), Nvg_NodeX(ai), Nvg_NodeY(ai)))
                    continue;
                if (!Nvg_AddDirectedIfWalkable((uint32_t)ai, (uint32_t)bi)
                    || !Nvg_AddDirectedIfWalkable((uint32_t)bi, (uint32_t)ai))
                {
                    Nvg_Disable("arc capacity", s_nvgArcCount + 1, NVG_MAXARCS);
                    return;
                }
            }
        }
    }
    // Cell centers can straddle a narrow Build sector: a real portal can
    // sit between centers whose containing sectors are A and C, leaving no
    // center in the thin B stair/door sector.  Add an explicit directed reach
    // for every declared portal wall, anchored at that wall's midpoint and the
    // nearest node in each sector.  These are still ordinary node arcs and use
    // explicit floor/ceiling/autostep gates; they cannot connect stacked sectors
    // because only wall.nextsector pairs are considered.
    for (int s = 0; s < numsectors; ++s)
    {
        int const wend = sector[s].wallptr + sector[s].wallnum;
        for (int w = sector[s].wallptr; w < wend; ++w)
        {
            int const ns = wall[w].nextsector;
            if (ns < 0 || (wall[w].cstat & 1))
                continue;
            int32_t const mx = (wall[w].x + wall[wall[w].point2].x) >> 1;
            int32_t const my = (wall[w].y + wall[wall[w].point2].y) >> 1;
            uint32_t a = NVG_INVALID, b = NVG_INVALID;
            int64_t ad = INT64_MAX, bd = INT64_MAX;
            for (uint32_t n = s_nvgSectorFirst[s]; n < s_nvgSectorFirst[s] + s_nvgSectorCount[s]; ++n)
            {
                int64_t const d = klabs(Nvg_NodeX(n) - mx) + klabs(Nvg_NodeY(n) - my);
                if (d < ad || (d == ad && n < a)) { ad = d; a = n; }
            }
            for (uint32_t n = s_nvgSectorFirst[ns]; n < s_nvgSectorFirst[ns] + s_nvgSectorCount[ns]; ++n)
            {
                int64_t const d = klabs(Nvg_NodeX(n) - mx) + klabs(Nvg_NodeY(n) - my);
                if (d < bd || (d == bd && n < b)) { bd = d; b = n; }
            }
            // No long teleports: each anchor must still be near this portal.
            if (a == NVG_INVALID || b == NVG_INVALID || ad > NVG_TILE * 2 || bd > NVG_TILE * 2)
                continue;
            int32_t const af = getflorzofslope(s, mx, my);
            int32_t const bf = getflorzofslope((int16_t)ns, mx, my);
            int32_t const ac = getceilzofslope(s, mx, my);
            int32_t const bc = getceilzofslope((int16_t)ns, mx, my);
            if (!Bot_SectorIsDoor(s) && !Bot_SectorIsDoor(ns)
                && min(af, bf) - max(ac, bc) < (26 << 8))
                continue;
            int32_t const rise = af - bf;
            if (rise > (20 << 8))
                continue;
            if (!Nvg_AddArc(a, b))
            {
                Nvg_Disable("portal arc capacity", s, ns);
                return;
            }
        }
    }

    for (uint32_t n = 0; n < s_nvgNodeCount; ++n)
    {
        if ((unsigned)s_nvgNode[n].cell >= (unsigned)total
            || (unsigned)s_nvgNode[n].sector >= (unsigned)numsectors)
        {
            Nvg_Disable("node audit", n, s_nvgNode[n].cell);
            return;
        }
        for (int32_t u = s_nvgCellNext[n]; u >= 0; u = s_nvgCellNext[u])
            if (s_nvgNode[u].sector == s_nvgNode[n].sector)
            {
                Nvg_Disable("duplicate node", n, u);
                return;
            }
        for (uint32_t a = s_nvgNode[n].firstArc; a != NVG_INVALID; a = s_nvgArc[a].next)
        {
            if (a >= s_nvgArcCount)
            {
                Nvg_Disable("arc index audit", a, s_nvgArcCount);
                return;
            }
            if (s_nvgArc[a].to >= s_nvgNodeCount)
            {
                Nvg_Disable("arc target audit", a, s_nvgArc[a].to);
                return;
            }
            for (uint32_t b = s_nvgArc[a].next; b != NVG_INVALID; b = s_nvgArc[b].next)
            {
                if (b >= s_nvgArcCount)
                {
                    Nvg_Disable("arc link audit", b, s_nvgArcCount);
                    return;
                }
                if (s_nvgArc[b].to == s_nvgArc[a].to)
                {
                    Nvg_Disable("duplicate arc", n, s_nvgArc[a].to);
                    return;
                }
            }
        }
    }
    s_nvgReady = 1;
    uint64_t const elapsedUsec = (timerGetPerformanceCounter() - buildStart) * 1000000u
                               / timerGetPerformanceFrequency();
    LOG_F(INFO, "nav: layered grid %dx%d nodes=%u arcs=%u audit=ok build=%" PRIu64 "us mem=%u",
          s_nvgW, s_nvgH, (unsigned)s_nvgNodeCount, (unsigned)s_nvgArcCount,
          elapsedUsec, (unsigned)(sizeof(s_nvgCellHead) + sizeof(s_nvgCellNext)
                                  + sizeof(s_nvgSectorFirst) + sizeof(s_nvgSectorCount)
                                  + sizeof(s_nvgNode) + sizeof(s_nvgArc)
                                  + NVG_MAXNODES * (sizeof(uint16_t) + 3 * sizeof(uint32_t))));
}

static void Bot_NavEnsure(void)
{
    int32_t const stamp = (int32_t)numwalls ^ ((int32_t)numsectors << 16)
                        ^ (numwalls > 0 ? wall[0].x + wall[0].y : 0);
    if (stamp != s_nvgStamp)
    {
        s_nvgStamp = stamp;
        Bot_NavBuild();
    }
}

void Net_BotBuildNavigation(void)
{
    // Called after the pristine board and slopes are loaded.  Every peer builds
    // the same graph, but only the host bot brain consumes it; no simulation RNG
    // is read and no world state is mutated.  Build unconditionally here: the
    // cheap runtime stamp is only a fallback, and two different maps can share
    // its wall/sector counts and first-wall checksum.
    s_nvgStamp = (int32_t)numwalls ^ ((int32_t)numsectors << 16)
               ^ (numwalls > 0 ? wall[0].x + wall[0].y : 0);
    Bot_NavBuild();
}

// Snap to a node without crossing layers.  Exact declared sector outranks all
// other candidates; then require z compatibility with the sampled ceiling/floor,
// rank XY distance, and use stable node ID as the final tiebreak.  A valid
// declared sector never falls through to an overlapping sector.
static uint32_t Nvg_Snap(int32_t x, int32_t y, int32_t z, int16_t declaredSector)
{
    if (!s_nvgReady)
        return NVG_INVALID;
    int const cell = Nvg_TileOf(x, y);
    if (cell < 0)
        return NVG_INVALID;
    int const tx = cell % s_nvgW, ty = cell / s_nvgW;
    bool const haveDeclared = (unsigned)declaredSector < (unsigned)numsectors;
    uint32_t best = NVG_INVALID;
    int64_t bestXY = INT64_MAX;
    int32_t bestZ = INT32_MAX, bestExact = 2;
    for (int r = 0; r <= 2; ++r)
    for (int oy = -r; oy <= r; ++oy)
    for (int ox = -r; ox <= r; ++ox)
    {
        if (r && klabs(ox) != r && klabs(oy) != r)
            continue;
        int const nx = tx + ox, ny = ty + oy;
        if (nx < 0 || ny < 0 || nx >= s_nvgW || ny >= s_nvgH)
            continue;
        for (int32_t ni = s_nvgCellHead[ny * s_nvgW + nx]; ni >= 0; ni = s_nvgCellNext[ni])
        {
            NvgNode const &n = s_nvgNode[ni];
            int const exact = haveDeclared && n.sector == declaredSector ? 0 : 1;
            // A valid declared sector is a hard layer boundary.  Only an invalid
            // declaration (off-map/free point) may rank sectors by z instead.
            if (haveDeclared && exact != 0)
                continue;
            // Duke positions are body/eye z rather than foot z.  Rank by the
            // nearest vertically compatible floor band: a normal body lives
            // within about 72 pixels above its declared sector's floor.  This
            // distinguishes overlapping floors even though both z values lie
            // inside a very tall lower sector volume.
            int32_t const aboveFloor = n.floorZ - z;
            int32_t zmiss = 0;
            if (aboveFloor < -(8 << 8)) zmiss = -aboveFloor;
            else if (aboveFloor > (96 << 8)) zmiss = aboveFloor - (96 << 8);
            if (z < n.ceilZ - (8 << 8) || zmiss > (72 << 8))
                continue;
            int64_t const ddx = (int64_t)Nvg_CX(nx) - x;
            int64_t const ddy = (int64_t)Nvg_CY(ny) - y;
            int64_t const xy = ddx * ddx + ddy * ddy;
            if (exact < bestExact || (exact == bestExact
                && (zmiss < bestZ || (zmiss == bestZ && (xy < bestXY
                || (xy == bestXY && (uint32_t)ni < best))))))
            {
                best = (uint32_t)ni; bestExact = exact; bestZ = zmiss; bestXY = xy;
            }
        }
    }
    return best;
}

#if defined(NETNATIVE)
extern "C" int Net_TestNavSnap(int32_t x, int32_t y, int32_t z, int16_t sectorNum)
{
    Bot_NavEnsure();
    uint32_t const n = Nvg_Snap(x, y, z, sectorNum);
    return n == NVG_INVALID ? -1 : (int)n;
}
#endif

static uint16_t s_nvgBfsSeen[NVG_MAXNODES];
static uint32_t s_nvgBfsParent[NVG_MAXNODES];
static uint32_t s_nvgBfsQueue[NVG_MAXNODES];
static uint32_t s_nvgBfsRev[NVG_MAXNODES];
static uint16_t s_nvgBfsGen;
static int Bot_AvoidEdgeActive(int k, int from, int to);
static int Bot_NearWedgeSpot(int k, int32_t x, int32_t y, int16_t sectorNum);
static int Bot_SegNearWedgeSpot(int k, int32_t x0, int32_t y0, int16_t sect0,
                               int32_t x1, int32_t y1, int16_t sect1);

static int Bot_NvgPath(int k, uint32_t fromNode, uint32_t destNode,
                       uint32_t *outNode, int maxOut)
{
    if (!s_nvgReady || fromNode >= s_nvgNodeCount || destNode >= s_nvgNodeCount || maxOut <= 0)
        return 0;
    if (++s_nvgBfsGen == 0) { Bmemset(s_nvgBfsSeen, 0, sizeof(s_nvgBfsSeen)); s_nvgBfsGen = 1; }
    int qh = 0, qt = 0;
    s_nvgBfsQueue[qt++] = fromNode;
    s_nvgBfsSeen[fromNode] = s_nvgBfsGen;
    s_nvgBfsParent[fromNode] = NVG_INVALID;
    uint32_t found = NVG_INVALID;
    while (qh < qt)
    {
        uint32_t const n = s_nvgBfsQueue[qh++];
        if (n == destNode) { found = n; break; }
        for (uint32_t a = s_nvgNode[n].firstArc; a != NVG_INVALID; a = s_nvgArc[a].next)
        {
            uint32_t const u = s_nvgArc[a].to;
            if (s_nvgBfsSeen[u] == s_nvgBfsGen)
                continue;
            if (k >= 0 && s_nvgNode[u].sector != s_nvgNode[n].sector
                && Bot_AvoidEdgeActive(k, s_nvgNode[n].sector, s_nvgNode[u].sector))
                continue;
            if (k >= 0 && (s_nvgNode[u].flags & NVG_F_CRAWL)
                && !(s_nvgNode[n].flags & NVG_F_CRAWL))
                continue;
            if (k >= 0
                && Bot_NearWedgeSpot(k, Nvg_NodeX(u), Nvg_NodeY(u), s_nvgNode[u].sector)
                && !Bot_NearWedgeSpot(k, Nvg_NodeX(n), Nvg_NodeY(n), s_nvgNode[n].sector))
                continue;
            s_nvgBfsSeen[u] = s_nvgBfsGen;
            s_nvgBfsParent[u] = n;
            s_nvgBfsQueue[qt++] = u;
        }
    }
    if (found == NVG_INVALID)
        return 0;
    int rn = 0;
    for (uint32_t n = found; s_nvgBfsParent[n] != NVG_INVALID; n = s_nvgBfsParent[n])
        s_nvgBfsRev[rn++] = n;            // full reconstruction: no truncation
    int cnt = 0;
    for (int i = rn - 1; i >= 0 && cnt < maxOut; --i)
        outNode[cnt++] = s_nvgBfsRev[i];
    return cnt;
}

#if defined(NETNATIVE)
extern "C" int Net_TestNavPath(int fromNode, int toNode)
{
    static uint32_t path[NVG_MAX_ROUTE];
    return Bot_NvgPath(-1, (uint32_t)fromNode, (uint32_t)toNode, path, ARRAY_SIZE(path));
}
#endif

static int16_t  s_ltgSectDist[MAXSECTORS];
static uint32_t s_ltgSectNode[MAXSECTORS];
static int Bot_NvgFloodSectDist(int k, uint32_t fromNode)
{
    if (!s_nvgReady || fromNode >= s_nvgNodeCount)
        return 0;
    for (int s = 0; s < numsectors; ++s)
        { s_ltgSectDist[s] = -1; s_ltgSectNode[s] = NVG_INVALID; }
    if (++s_nvgBfsGen == 0) { Bmemset(s_nvgBfsSeen, 0, sizeof(s_nvgBfsSeen)); s_nvgBfsGen = 1; }
    int qh = 0, qt = 0, depth = 0, found = 0;
    s_nvgBfsQueue[qt++] = fromNode;
    s_nvgBfsSeen[fromNode] = s_nvgBfsGen;
    while (qh < qt)
    {
        int const levelEnd = qt;
        for (; qh < levelEnd; ++qh)
        {
            uint32_t const n = s_nvgBfsQueue[qh];
            int const sct = s_nvgNode[n].sector;
            if (s_ltgSectDist[sct] < 0)
            {
                s_ltgSectDist[sct] = (int16_t)min(depth, 32000);
                s_ltgSectNode[sct] = n;
                ++found;
            }
            for (uint32_t a = s_nvgNode[n].firstArc; a != NVG_INVALID; a = s_nvgArc[a].next)
            {
                uint32_t const u = s_nvgArc[a].to;
                if (s_nvgBfsSeen[u] == s_nvgBfsGen)
                    continue;
                if (k >= 0 && s_nvgNode[u].sector != sct
                    && Bot_AvoidEdgeActive(k, sct, s_nvgNode[u].sector))
                    continue;
                if (k >= 0 && (s_nvgNode[u].flags & NVG_F_CRAWL)
                    && !(s_nvgNode[n].flags & NVG_F_CRAWL))
                    continue;
                if (k >= 0
                    && Bot_NearWedgeSpot(k, Nvg_NodeX(u), Nvg_NodeY(u), s_nvgNode[u].sector)
                    && !Bot_NearWedgeSpot(k, Nvg_NodeX(n), Nvg_NodeY(n), s_nvgNode[n].sector))
                    continue;
                s_nvgBfsSeen[u] = s_nvgBfsGen;
                s_nvgBfsQueue[qt++] = u;
            }
        }
        ++depth;
    }
    return found;
}

static uint32_t s_botRtNode[MAXPLAYERS][NVG_MAX_ROUTE];
static int8_t   s_botRouteLen[MAXPLAYERS], s_botRouteIdx[MAXPLAYERS];
static int32_t  s_botRouteDX[MAXPLAYERS], s_botRouteDY[MAXPLAYERS], s_botRouteDZ[MAXPLAYERS];
static int16_t  s_botRouteDSect[MAXPLAYERS];
static int16_t  s_botRouteCool[MAXPLAYERS];
static int8_t   s_botWpDoor[MAXPLAYERS];

// Central lifecycle contract. BODY is every state bit owned by one physical
// incarnation/seat use; LEVEL additionally clears all identities, coordinates,
// timestamps and telemetry whose meaning depends on the current board. Keep
// this helper draw-free: in particular it never touches s_botRng.
enum BotResetScope { BOT_RESET_BODY, BOT_RESET_LEVEL };
static void Bot_ResetSeat(int k, BotResetScope scope)
{
    if ((unsigned)k >= MAXPLAYERS)
        return;

    // Target, awareness, retaliation, aim and fire state.
    s_botTarget[k]       = -1;
    s_botMonTgt[k]       = -1;
    s_botPending[k]      = -1;
    s_botSeeStreak[k]    = 0;
    s_botLastWacked[k]   = -1;
    s_botSightTics[k]    = 400;
    s_botTargetHold[k]   = 0;
    s_botNoHitTics[k]    = 0;
    s_botLastTDist[k]    = INT32_MAX;
    s_botFireSight[k]    = 0;
    s_botTgtSX[k]        = 0;
    s_botTgtSY[k]        = 0;
    s_botTgtVX[k]        = 0;
    s_botTgtVY[k]        = 0;
    s_botTgtSnap[k]      = 0;
    s_botTgtVValid[k]    = 0;
    s_botAimDegrade[k]   = 0;
    s_botViewVel[k]      = 0;
    s_botThrWait[k]      = 0;
    s_botThrShoot[k]     = 0;
    s_botBurst[k]        = 0;
    s_botBreakFire[k]    = 0;
    s_botSeenX[k]        = 0;
    s_botSeenY[k]        = 0;
    s_botSeenZ[k]        = 0;
    s_botSeenSect[k]     = -1;
    s_botSeenValid[k]    = 0;

    // Body motion, bounce/lane/open/stuck/trap and fresh-anchor state.
    s_botLive[k]         = 0;
    s_botWasDead[k]      = 0;
    s_botSpawnRoam[k]    = 0;
    s_botStrafeDir[k]    = 0;
    s_botStrafeTic[k]    = 0;
    s_botStrafeFail[k]   = 0;
    s_botWantStrafe[k]   = 0;
    s_botWanderAng[k]    = 0;
    s_botThinkHold[k]    = 0;
    s_botLastPos[k]      = {};
    s_botStuckTics[k]    = 0;
    s_botStuckEpisodes[k]= 0;
    s_botBounceHold[k]   = 0;
    s_botBounceAng[k]    = 0;
    s_botTurnPref[k]     = 0;
    s_botOpenGrace[k]    = 0;
    s_botLaneAng[k]      = 0;
    s_botLaneHold[k]     = 0;
    s_botThreadFails[k]  = 0;
    s_botJumpCool[k]     = 0;
    s_botTrapTics[k]     = 0;
    s_botTrapAnchor[k]   = {};
    s_botTrapCool[k]     = 0;
    s_botTrapDir[k]      = 0;
    s_botTrapRounds[k]   = 0;

    // Goal, navigation, route and reachability verdict state.
    s_botNavX[k]         = 0;
    s_botNavY[k]         = 0;
    s_botNavZ[k]         = 0;
    s_botNavSect[k]      = -1;
    s_botNavOn[k]        = 0;
    s_botNavSeen[k]      = 0;
    s_botRouteFail[k]    = {};
    s_botRouteFail[k].target = -1;
    s_botLastSect[k]     = -1;
    s_botGoal[k]         = 0;
    s_botGoalX[k]        = 0;
    s_botGoalY[k]        = 0;
    s_botGoalZ[k]        = 0;
    s_botGoalSect[k]     = -1;
    s_botGoalTics[k]     = 0;
    s_botGoalDoor[k]     = 0;
    s_botGoalCrouch[k]   = 0;
    s_botGoalItem[k]     = -1;
    s_botGoalSeen[k]     = 0;
    s_botGoalNear[k]     = 0;
    s_botGoalStall[k]    = 0;
    s_botGoalIsLtg[k]    = 0;
    s_botRouteLen[k]     = 0;
    s_botRouteIdx[k]     = 0;
    s_botRouteDX[k]      = 0;
    s_botRouteDY[k]      = 0;
    s_botRouteDZ[k]      = 0;
    s_botRouteDSect[k]   = -1;
    s_botRouteCool[k]    = 0;
    s_botWpDoor[k]       = 0;
    for (int i = 0; i < NVG_MAX_ROUTE; i++)
        s_botRtNode[k][i] = NVG_INVALID;

    // Jet navigation is body-transient. Activation counters are level telemetry.
    s_botJetHold[k]      = 0;
    s_botJetCool[k]      = 0;

    if (scope != BOT_RESET_LEVEL)
        return;

    // Every board-indexed identity or sentinel is invalid on a new map.
    Bmemset(s_botVisitT[k], 0, sizeof(s_botVisitT[k]));
    for (int i = 0; i < BOT_DEAD_N; i++)
    {
        s_botDeadSect[k][i] = -1;
        s_botDeadCool[k][i] = 0;
    }
    for (int i = 0; i < BOT_IAVOID_N; i++)
    {
        s_botItemAvoid[k][i]    = -1;
        s_botItemAvoidTil[k][i] = 0;
    }
    for (int i = 0; i < BOT_EAVOID_N; i++)
    {
        s_botEdgeFrom[k][i]  = -1;
        s_botEdgeTo[k][i]    = -1;
        s_botEdgeTries[k][i] = 0;
        s_botEdgeUntil[k][i] = 0;
    }
    for (int i = 0; i < BOT_WSPOT_N; i++)
    {
        s_botWspotX[k][i]     = 0;
        s_botWspotY[k][i]     = 0;
        s_botWspotSect[k][i]  = -1;
        s_botWspotUntil[k][i] = 0;
    }
    s_botPrevSect[k]     = -1;
    s_botItemShun[k]     = -1;
    s_botLtgX[k]         = 0;
    s_botLtgY[k]         = 0;
    s_botLtgZ[k]         = 0;
    s_botLtgSect[k]      = -1;
    s_botLtgKind[k]      = 0;
    s_botLtgItem[k]      = -1;
    s_botLtgUntil[k]     = 0;
    s_botLtgFails[k]     = 0;
    s_botLtgAnchor[k]    = {};
    s_botLtgLocal[k]     = 0;
    Bmemset(s_botRoamBm[k], 0, sizeof(s_botRoamBm[k]));
    s_botRoamCnt[k]      = 0;
    s_botRoamLogPlc[k]   = 0;
    s_botStillTics[k]    = 0;
    s_botIdleTics[k]     = 0;
    s_botTeamLogged[k]   = 0;
    s_botJumps[k]        = 0;
    s_botMedUses[k]      = 0;
    s_botSterUses[k]     = 0;
    s_botJetActs[k]      = 0;
    s_botInvLogPlc[k]    = 0;
    s_botCamLogPlc[k]    = 0;
}

void Net_BotResetLevel(void)
{
    for (int k = 0; k < MAXPLAYERS; k++)
        Bot_ResetSeat(k, BOT_RESET_LEVEL);

    // The graph itself is rebuilt unconditionally by Net_BotBuildNavigation at
    // level entry. Clear only scratch identities that can survive between calls.
    s_nvgBfsGen = 0;
    Bmemset(s_nvgBfsSeen, 0, sizeof(s_nvgBfsSeen));
    Bmemset(s_nvgBfsParent, 0, sizeof(s_nvgBfsParent));
    Bmemset(s_nvgBfsQueue, 0, sizeof(s_nvgBfsQueue));
    for (int s = 0; s < MAXSECTORS; s++)
    {
        s_ltgSectDist[s] = -1;
        s_ltgSectNode[s] = NVG_INVALID;
    }
    s_botSepPlc = -1;
}

// Side-effect-free validity check for a retained/acquired player target. Callers
// decide how to clear state; this predicate only reads canonical roster/body
// state. Team filtering belongs here so every retention/materialization path uses
// the same definition as acquisition.
static bool Bot_IsLivePlayerTarget(int k, int target, DukePlayer_t const *bot,
                                   bool teamGame)
{
    if ((unsigned)k >= MAXPLAYERS || (unsigned)target >= MAXPLAYERS || target == k
        || bot == NULL || !g_player[target].connected)
        return false;
    auto const tp = g_player[target].ps;
    if (tp == NULL || (unsigned)tp->i >= MAXSPRITES
        || (unsigned)tp->cursectnum >= (unsigned)numsectors || tp->dead_flag)
        return false;
    auto const &sp = sprite[tp->i];
    return BotLivePlayerTarget({ k, target, MAXPLAYERS, 1, 1,
        tp->i, MAXSPRITES, tp->cursectnum, numsectors, tp->dead_flag,
        sp.statnum, MAXSTATUS, sp.picnum, APLAYER, sp.yvel, sp.extra,
        teamGame ? 1 : 0, bot->team, tp->team });
}

static bool Bot_IsLiveMonsterTarget(int target)
{
    if ((unsigned)target >= MAXSPRITES)
        return false;
    auto const &sp = sprite[target];
    return sp.statnum < MAXSTATUS && !(sp.cstat & 32768) && sp.extra > 0
        && (unsigned)sp.sectnum < (unsigned)numsectors && A_CheckEnemySprite(&sp);
}

// Clear only combat-owned state: disengagement must hand movement back to the
// roam/escort planner rather than resetting unrelated body/world memory.
static void Bot_ClearTargetState(int k)
{
    s_botTarget[k]       = -1;
    s_botMonTgt[k]       = -1;
    s_botPending[k]      = -1;
    s_botSeeStreak[k]    = 0;
    s_botLastWacked[k]   = -1;
    s_botSightTics[k]    = 400;
    s_botTargetHold[k]   = 0;
    s_botNoHitTics[k]    = 0;
    s_botLastTDist[k]    = INT32_MAX;
    s_botFireSight[k]    = 0;
    s_botTgtVValid[k]    = 0;
    s_botTgtVX[k]        = 0;
    s_botTgtVY[k]        = 0;
    s_botTgtSnap[k]      = 0;
    s_botAimDegrade[k]   = 0;
    s_botViewVel[k]      = 0;
    s_botThrWait[k]      = 0;
    s_botThrShoot[k]     = 0;
    s_botSeenValid[k]    = 0;
    s_botNavOn[k]        = 0;
    s_botNavSeen[k]      = 0;
    s_botRouteFail[k]    = {};
    s_botRouteFail[k].target = -1;
    s_botRouteLen[k]     = 0;
    s_botRouteIdx[k]     = 0;
    s_botRouteCool[k]    = 0;
    s_botWpDoor[k]       = 0;
}

static void Bot_SetCombatRouteResult(int k, int kind, int target, bool success)
{
    BotRouteStoreResult(s_botRouteFail[k], kind, target, success);
}

// Does the straight walk cross a crawl-height (duct) node? Bot_LineWalkable
// is deliberately CEILING-blind, so the straight-shot legs could tunnel a
// march into the vent system the layered router refuses. Endpoints inside
// ducts are exempt: leaving or a genuine duct goal is allowed; blind entry is not.
static int Bot_SegCrossesCrawl(int32_t x0, int32_t y0, int32_t z0, int16_t sect0,
                               int32_t x1, int32_t y1, int32_t z1, int16_t sect1)
{
    if (!s_nvgReady)
        return 0;
    uint32_t const end = Nvg_Snap(x1, y1, z1, sect1);
    if (end != NVG_INVALID && (s_nvgNode[end].flags & NVG_F_CRAWL))
        return 0;
    int32_t const dx = x1 - x0, dy = y1 - y0, dz = z1 - z0;
    int const steps = clamp((int)((klabs(dx) + klabs(dy)) >> 8), 1, 64);
    for (int i = 1; i < steps; ++i)
    {
        int16_t declared = sect0;
        if (sect0 != sect1 && (unsigned)sect1 < (unsigned)numsectors && i * 2 >= steps)
            declared = sect1;
        uint32_t const n = Nvg_Snap(x0 + (int32_t)(((int64_t)dx * i) / steps),
                                    y0 + (int32_t)(((int64_t)dy * i) / steps),
                                    z0 + (int32_t)(((int64_t)dz * i) / steps), declared);
        if (n != NVG_INVALID && (s_nvgNode[n].flags & NVG_F_CRAWL))
            return 1;
    }
    return 0;
}

static int Nvg_LineWalkSameSector(DukePlayer_t *ps, int32_t x1, int32_t y1, int16_t sectorNum)
{
    if (ps->cursectnum != sectorNum)
        return 0;
    if (Nvg_SegBlocked(sectorNum, ps->pos.x, ps->pos.y, x1, y1))
        return 0;
    return Nvg_FineClimbLayered(sectorNum, sectorNum, ps->pos.x, ps->pos.y, x1, y1);
}

static int Bot_Waypoint(int k, DukePlayer_t *ps, int32_t dx2, int32_t dy2, int32_t dz2,
                        int16_t dsect, int32_t *wx, int32_t *wy, int32_t *wz, int16_t *wsect)
{
    *wx = dx2; *wy = dy2; *wz = dz2; *wsect = dsect;
    s_botWpDoor[k] = 0;
    Bot_NavEnsure();
    if (!s_nvgReady)
        return 0;
    if (Nvg_LineWalkSameSector(ps, dx2, dy2, dsect)
        && !Bot_SegNearWedgeSpot(k, ps->pos.x, ps->pos.y, ps->cursectnum,
                                 dx2, dy2, dsect)
        && !Bot_SegCrossesCrawl(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum,
                                dx2, dy2, dz2, dsect))
    { s_botRouteLen[k] = 0; return 1; }
    if (s_botRouteCool[k] > 0)
        --s_botRouteCool[k];
    int const moved = klabs(s_botRouteDX[k] - dx2) + klabs(s_botRouteDY[k] - dy2)
                    + (klabs(s_botRouteDZ[k] - dz2) >> 4);
    int const needPath = s_botRouteIdx[k] >= s_botRouteLen[k] || moved > 1024
                       || s_botRouteDSect[k] != dsect;
    if (needPath && s_botRouteCool[k] == 0)
    {
        uint32_t const from = Nvg_Snap(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum);
        uint32_t const dest = Nvg_Snap(dx2, dy2, dz2, dsect);
        s_botRouteLen[k] = (int8_t)Bot_NvgPath(k, from, dest, s_botRtNode[k], NVG_MAX_ROUTE);
        s_botRouteIdx[k] = 0;
        s_botRouteDX[k] = dx2; s_botRouteDY[k] = dy2; s_botRouteDZ[k] = dz2;
        s_botRouteDSect[k] = dsect;
        s_botRouteCool[k] = 26;
    }
    if (s_botRouteLen[k] <= 0)
        return 0;

    while (s_botRouteIdx[k] < s_botRouteLen[k])
    {
        uint32_t const n = s_botRtNode[k][s_botRouteIdx[k]];
        if (n >= s_nvgNodeCount) { s_botRouteLen[k] = 0; return 0; }
        int32_t const dist = klabs(Nvg_NodeX(n) - ps->pos.x) + klabs(Nvg_NodeY(n) - ps->pos.y);
        bool const door = (s_nvgNode[n].flags & NVG_F_DOOR) != 0;
        // A door node is consumed only after entering its declared sector and
        // reaching the threshold.  The old generic <400 test spent it from the
        // approach side and immediately repathed/string-pulled into the jamb.
        if ((!door && dist < 400) || (door && ps->cursectnum == s_nvgNode[n].sector && dist < 128))
            ++s_botRouteIdx[k];
        else
            break;
    }
    if (s_botRouteIdx[k] >= s_botRouteLen[k])
        return 0;

    int pick = s_botRouteIdx[k];
    uint32_t const current = s_botRtNode[k][pick];
    // If routeIdx itself is a door, keep it as the waypoint.  Otherwise extend
    // only to the first future door and never beyond it.
    if (!(s_nvgNode[current].flags & NVG_F_DOOR))
    for (int ahead = 1; ahead <= 6; ++ahead)
    {
        int const i = s_botRouteIdx[k] + ahead;
        if (i >= s_botRouteLen[k])
            break;
        uint32_t const n = s_botRtNode[k][i];
        if (n >= s_nvgNodeCount)
            { s_botRouteLen[k] = 0; return 0; }
        int32_t const nx = Nvg_NodeX(n), ny = Nvg_NodeY(n);
        bool const door = (s_nvgNode[n].flags & NVG_F_DOOR) != 0;
        if (s_nvgNode[n].sector == ps->cursectnum
            && Nvg_LineWalkSameSector(ps, nx, ny, s_nvgNode[n].sector)
            && !Bot_SegNearWedgeSpot(k, ps->pos.x, ps->pos.y, ps->cursectnum,
                                     nx, ny, s_nvgNode[n].sector)
            && !Bot_SegCrossesCrawl(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum,
                                    nx, ny, s_nvgNode[n].floorZ, s_nvgNode[n].sector))
            pick = i;
        if (door)
            break;
    }
    uint32_t const picked = s_botRtNode[k][pick];
    *wx = Nvg_NodeX(picked); *wy = Nvg_NodeY(picked);
    *wz = s_nvgNode[picked].floorZ; *wsect = s_nvgNode[picked].sector;
    for (int i = s_botRouteIdx[k]; i < s_botRouteLen[k]; ++i)
    {
        uint32_t const n = s_botRtNode[k][i];
        if (n >= s_nvgNodeCount)
            { s_botRouteLen[k] = 0; return 0; }
        if (s_nvgNode[n].flags & NVG_F_DOOR)
            { s_botWpDoor[k] = 1; break; }
    }
    return 1;
}

#if defined(NETNATIVE)
extern "C" int Net_TestNavDoorRoute(int seat, int routeIdx, int playerDist,
                                     int sameDoorSector, int currentDoor, int futureDoor)
{
    if ((unsigned)seat >= MAXPLAYERS || routeIdx < 0 || routeIdx + 2 >= NVG_MAX_ROUTE)
        return 0;
    uint32_t door = NVG_INVALID, nearPlain = NVG_INVALID, farPlain = NVG_INVALID;
    for (uint32_t n = 0; n < s_nvgNodeCount; ++n)
    {
        if (door == NVG_INVALID && (s_nvgNode[n].flags & NVG_F_DOOR))
            door = n;
        if (!(s_nvgNode[n].flags & NVG_F_DOOR))
        {
            if (nearPlain == NVG_INVALID)
                nearPlain = n;
            if (door != NVG_INVALID && s_nvgNode[n].sector != s_nvgNode[door].sector
                && klabs(Nvg_NodeX(n) - Nvg_NodeX(door))
                 + klabs(Nvg_NodeY(n) - Nvg_NodeY(door)) > 2048)
                { farPlain = n; break; }
        }
    }
    if (door == NVG_INVALID || nearPlain == NVG_INVALID || farPlain == NVG_INVALID)
        return 0;

    uint32_t const first = currentDoor ? door : nearPlain;
    uint32_t const second = futureDoor ? door : farPlain;
    s_botRouteIdx[seat] = (int8_t)routeIdx;
    s_botRouteLen[seat] = (int8_t)(routeIdx + 3);
    s_botRtNode[seat][routeIdx] = first;
    s_botRtNode[seat][routeIdx + 1] = second;
    s_botRtNode[seat][routeIdx + 2] = farPlain;
    s_botRouteDX[seat] = Nvg_NodeX(farPlain);
    s_botRouteDY[seat] = Nvg_NodeY(farPlain);
    s_botRouteDZ[seat] = s_nvgNode[farPlain].floorZ;
    s_botRouteDSect[seat] = s_nvgNode[farPlain].sector;
    s_botRouteCool[seat] = 1;

    DukePlayer_t fake = {};
    fake.cursectnum = (currentDoor && sameDoorSector)
                      ? s_nvgNode[door].sector : s_nvgNode[nearPlain].sector;
    fake.pos.x = Nvg_NodeX(first) + playerDist;
    fake.pos.y = Nvg_NodeY(first);
    fake.pos.z = s_nvgNode[first].floorZ - (38 << 8);
    int32_t wx, wy, wz;
    int16_t ws;
    int const guided = Bot_Waypoint(seat, &fake, Nvg_NodeX(farPlain), Nvg_NodeY(farPlain),
                                    s_nvgNode[farPlain].floorZ, s_nvgNode[farPlain].sector,
                                    &wx, &wy, &wz, &ws);
    uint32_t const expected = s_botRtNode[seat][s_botRouteIdx[seat]];
    int const retained = guided && wx == Nvg_NodeX(expected) && wy == Nvg_NodeY(expected);
    return (s_botRouteIdx[seat] << 8) | (retained ? 2 : 0) | (s_botWpDoor[seat] ? 1 : 0);
}
#endif

static int Bot_IsPickup(int picnum)
{
    switch (tileGetMapping(picnum))
    {
    case COLA__: case SIXPAK__: case FIRSTAID__: case ATOMICHEALTH__:
    case SHIELD__: case STEROIDS__: case AIRTANK__: case JETPACK__:
    case HEATSENSOR__: case BOOTS__: case HOLODUKE__:
    case AMMO__: case BATTERYAMMO__: case DEVISTATORAMMO__: case RPGAMMO__:
    case GROWAMMO__: case CRYSTALAMMO__: case HBOMBAMMO__: case AMMOLOTS__:
    case SHOTGUNAMMO__: case FREEZEAMMO__:
    case FIRSTGUNSPRITE__: case SHOTGUNSPRITE__: case CHAINGUNSPRITE__:
    case RPGSPRITE__: case FREEZESPRITE__: case DEVISTATORSPRITE__:
    case SHRINKERSPRITE__: case TRIPBOMBSPRITE__: case GROWSPRITEICON__:
        return 1;
    default:
        return 0;
    }
}

static int Bot_SectorIsDoor(int s)
{
    switch (sector[s].lotag)
    {
    case ST_9_SLIDING_ST_DOOR: case ST_20_CEILING_DOOR: case ST_21_FLOOR_DOOR:
    case ST_22_SPLITTING_DOOR: case ST_23_SWINGING_DOOR: case ST_25_SLIDING_DOOR:
    case ST_26_SPLITTING_ST_DOOR: case ST_29_TEETH_DOOR:
        return 1;
    }
    return 0;
}

// --- impossible-exit ring (see the s_botDead* declarations) -------------------
// Mark a target sector the bot just failed to reach; it will be avoided by the
// explore planner until the cooldown decays (or the bot actually enters it,
// which proves it reachable and clears the mark). LRU-evicts the coolest slot.
static void Bot_MarkDeadExit(int k, int sect)
{
    if ((unsigned)sect >= (unsigned)numsectors)
        return;
    int slot = -1, lru = 0;
    for (int i = 0; i < BOT_DEAD_N; i++)
    {
        if (s_botDeadSect[k][i] == sect && s_botDeadCool[k][i] > 0) { slot = i; break; }
        if (s_botDeadCool[k][i] < s_botDeadCool[k][lru]) lru = i;
    }
    if (slot < 0) slot = lru;
    s_botDeadSect[k][slot] = (int16_t)sect;
    s_botDeadCool[k][slot] = 2400;   // ~80s at 30Hz; clear-on-entry cuts it short
}
static int Bot_DeadExitActive(int k, int sect)
{
    for (int i = 0; i < BOT_DEAD_N; i++)
        if (s_botDeadSect[k][i] == sect && s_botDeadCool[k][i] > 0)
            return 1;
    return 0;
}
static void Bot_ClearDeadExit(int k, int sect)
{
    for (int i = 0; i < BOT_DEAD_N; i++)
        if (s_botDeadSect[k][i] == sect)
            s_botDeadCool[k][i] = 0;
}

// --- item respawn-schedule ring (see the s_botItemAvoid* declarations) -------
// Stamp: this sprite is predicted absent until untilPlc. Same-slot refresh,
// else evict the slot whose prediction expires soonest (their avoidgoal ring).
static void Bot_ItemAvoidStamp(int k, int spr, int32_t untilPlc)
{
    int slot = -1, lru = 0;
    for (int i = 0; i < BOT_IAVOID_N; i++)
    {
        if (s_botItemAvoid[k][i] == spr) { slot = i; break; }
        if (s_botItemAvoidTil[k][i] < s_botItemAvoidTil[k][lru]) lru = i;
    }
    if (slot < 0) slot = lru;
    s_botItemAvoid[k][slot]    = (int16_t)spr;
    s_botItemAvoidTil[k][slot] = untilPlc;
}
// Predicted-respawn tic for a sprite, 0 = not tracked. A stamp impossibly far
// in the future means movefifoplc restarted (level change): treat as untracked.
static int32_t Bot_ItemAvoidUntil(int k, int spr)
{
    for (int i = 0; i < BOT_IAVOID_N; i++)
        if (s_botItemAvoid[k][i] == spr)
        {
            int32_t const til = s_botItemAvoidTil[k][i];
            if (til - movefifoplc > g_itemRespawnTime + 130)
                return 0;
            return til;
        }
    return 0;
}
static void Bot_ItemAvoidReset(int k)
{
    for (int i = 0; i < BOT_IAVOID_N; i++)
        { s_botItemAvoid[k][i] = -1; s_botItemAvoidTil[k][i] = 0; }
}

// --- per-edge avoid-reach ring (see the s_botEdge* declarations) -------------
// Mark a failed (fromSect -> toSect) crossing; returns the try count so the
// caller can escalate to the sector-wide dead ring after repeated failures.
// Their BotAddToAvoidReach shape (be_ai_move.c:581): tries++ while the window
// is still live, reset to 1 when it lapsed.
static int Bot_MarkAvoidEdge(int k, int from, int to)
{
    int32_t const plc = movefifoplc;
    int slot = -1, lru = 0;
    for (int i = 0; i < BOT_EAVOID_N; i++)
    {
        if (s_botEdgeFrom[k][i] == from && s_botEdgeTo[k][i] == to) { slot = i; break; }
        if (s_botEdgeUntil[k][i] < s_botEdgeUntil[k][lru]) lru = i;
    }
    if (slot < 0)
    {
        slot = lru;
        s_botEdgeFrom[k][slot]  = (int16_t)from;
        s_botEdgeTo[k][slot]    = (int16_t)to;
        s_botEdgeTries[k][slot] = 0;
    }
    else if (s_botEdgeUntil[k][slot] < plc || s_botEdgeUntil[k][slot] - plc > 400)
        s_botEdgeTries[k][slot] = 0;            // window lapsed (or plc restarted)
    if (s_botEdgeTries[k][slot] < 100)
        s_botEdgeTries[k][slot]++;
    s_botEdgeUntil[k][slot] = plc + 180;        // ~7s window (their 6s at 26 tics/s)
    return s_botEdgeTries[k][slot];
}
// An edge is only DEAD after 3+ failures inside the live window (their
// AVOIDREACH_TRIES gate, be_ai_move.c:772) -- one snag costs nothing.
static int Bot_AvoidEdgeActive(int k, int from, int to)
{
    int32_t const plc = movefifoplc;
    for (int i = 0; i < BOT_EAVOID_N; i++)
        if (s_botEdgeFrom[k][i] == from && s_botEdgeTo[k][i] == to
            && s_botEdgeTries[k][i] >= 3
            && s_botEdgeUntil[k][i] >= plc && s_botEdgeUntil[k][i] - plc <= 400)
            return 1;
    return 0;
}
static void Bot_ClearAvoidEdges(int k)          // total plan failure: deadlock break
{
    for (int i = 0; i < BOT_EAVOID_N; i++)
        { s_botEdgeUntil[k][i] = 0; s_botEdgeTries[k][i] = 0; s_botEdgeFrom[k][i] = s_botEdgeTo[k][i] = -1; }
}
static void Bot_ClearAvoidEdgeTo(int k, int sect)   // entered it: edges in are proven
{
    for (int i = 0; i < BOT_EAVOID_N; i++)
        if (s_botEdgeTo[k][i] == sect)
            { s_botEdgeUntil[k][i] = 0; s_botEdgeTries[k][i] = 0; }
}

// --- wedge-spot ring (layer-aware) -------------------------------------------
static void Bot_MarkWedgeSpot(int k, int32_t x, int32_t y, int16_t sectorNum)
{
    int32_t const plc = movefifoplc;
    int slot = 0;
    for (int i = 0; i < BOT_WSPOT_N; ++i)
    {
        if (s_botWspotUntil[k][i] >= plc && s_botWspotSect[k][i] == sectorNum
            && klabs(s_botWspotX[k][i] - x) + klabs(s_botWspotY[k][i] - y) < BOT_WSPOT_R)
            { slot = i; break; }
        if (s_botWspotUntil[k][i] < s_botWspotUntil[k][slot])
            slot = i;
    }
    s_botWspotX[k][slot] = x;
    s_botWspotY[k][slot] = y;
    s_botWspotSect[k][slot] = sectorNum;
    s_botWspotUntil[k][slot] = plc + 1560;
}
static int Bot_NearWedgeSpot(int k, int32_t x, int32_t y, int16_t sectorNum)
{
    int32_t const plc = movefifoplc;
    for (int i = 0; i < BOT_WSPOT_N; ++i)
        if (s_botWspotUntil[k][i] >= plc && s_botWspotUntil[k][i] - plc <= 2000
            && s_botWspotSect[k][i] == sectorNum
            && klabs(s_botWspotX[k][i] - x) + klabs(s_botWspotY[k][i] - y) < BOT_WSPOT_R)
            return 1;
    return 0;
}
static void Bot_ClearWedgeSpots(int k)
{
    for (int i = 0; i < BOT_WSPOT_N; ++i)
        { s_botWspotUntil[k][i] = 0; s_botWspotSect[k][i] = -1; }
}
static int Bot_SegNearWedgeSpot(int k, int32_t x0, int32_t y0, int16_t sect0,
                               int32_t x1, int32_t y1, int16_t sect1)
{
    int32_t const plc = movefifoplc;
    int32_t const dx = x1 - x0, dy = y1 - y0;
    int const steps = clamp((int)((klabs(dx) + klabs(dy)) >> 8), 1, 64);
    for (int step = 0; step <= steps; ++step)
    {
        int16_t const layer = (sect0 == sect1 || step * 2 < steps) ? sect0 : sect1;
        int32_t const sx = x0 + (int32_t)(((int64_t)dx * step) / steps);
        int32_t const sy = y0 + (int32_t)(((int64_t)dy * step) / steps);
        for (int i = 0; i < BOT_WSPOT_N; ++i)
            if (s_botWspotUntil[k][i] >= plc && s_botWspotUntil[k][i] - plc <= 2000
                && s_botWspotSect[k][i] == layer
                && klabs(s_botWspotX[k][i] - sx) + klabs(s_botWspotY[k][i] - sy) < BOT_WSPOT_R)
                return 1;
    }
    return 0;
}

// Roam telemetry: first entry into a sector this level bumps the distinct
// count (the [roam] meter -- the user-visible "bots roam deeper" number).
static void Bot_RoamStamp(int k, int sect)
{
    if ((unsigned)sect >= (unsigned)numsectors)
        return;
    if (!(s_botRoamBm[k][sect >> 3] & (1 << (sect & 7))))
    {
        s_botRoamBm[k][sect >> 3] |= (uint8_t)(1 << (sect & 7));
        s_botRoamCnt[k]++;
    }
}

// COOP target-find: the nearest enemy MONSTER in line of sight. In Cooperative
// the bot fights monsters and NEVER its human teammates (user 2026-08-12: "it
// should be trying to kill monsters, not players ... never revenge attack
// another player"). Brief pursuit memory holds the last enemy through short
// occlusion, mirroring the DM player-chase. Returns a sprite index, or -1.
static int Bot_AcquireMonster(int k, DukePlayer_t *ps)
{
    int best = -1;
    int32_t bestd = INT32_MAX;
    for (int i = headspritestat[STAT_ACTOR]; i >= 0; i = nextspritestat[i])
    {
        auto const &s = sprite[i];
        if (s.extra <= 0 || (s.cstat & 32768) || !A_CheckEnemySprite(&s)
            || (unsigned)s.sectnum >= (unsigned)numsectors
            || Bot_DeadExitActive(k, s.sectnum))
            continue;
        int32_t const d = klabs(s.x - ps->pos.x) + klabs(s.y - ps->pos.y) + (klabs(s.z - ps->pos.z) >> 2);
        if (d >= bestd)
            continue;
        if (!cansee(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum, s.x, s.y, s.z - (8 << 8), s.sectnum))
            continue;
        bestd = d; best = i;
    }
    if (best < 0)   // pursuit memory: keep the last enemy briefly through occlusion
    {
        int const m = s_botMonTgt[k];
        if (Bot_IsLiveMonsterTarget(m) && !Bot_DeadExitActive(k, sprite[m].sectnum)
            && s_botSightTics[k] < 130)
            best = m;
    }
    return best;
}

// Nearest HUMAN teammate (a connected, non-CPU, living player) to bot k. In
// coop the bot escorts the humans -- it navigates toward the nearest one when
// idle instead of exploring the whole level (user 2026-08-12: "navigate nearby
// the player until it sees an enemy"). Returns a player index, or -1.
static int Bot_NearestHuman(int k, DukePlayer_t *ps)
{
    int best = -1;
    int32_t bestd = INT32_MAX;
    extern int32_t g_netBotMask;
    for (int i = 0; i < MAXPLAYERS; i++)
    {
        if (i == k || (g_netBotMask & (1 << i)) || !g_player[i].connected || g_player[i].ps == NULL)
            continue;
        auto const hp = g_player[i].ps;
        if ((unsigned)hp->i >= MAXSPRITES || sprite[hp->i].extra <= 0 || hp->dead_flag)
            continue;
        int32_t const d = klabs(hp->pos.x - ps->pos.x) + klabs(hp->pos.y - ps->pos.y);
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

// Pick the exit: score every portal of the current sector by the staleness of
// what's behind it. The entry portal is the freshest by construction and gets
// a further 16x haircut -- it is chosen only when it is the ONLY way out.
// Feet-checked: portals needing a jump up are not exits (very little jumping),
// door sectors are exits regardless of their current gap (they open), and a
// crouch-height gap is an exit with the duck flag set (vents).
static int Bot_PlanExplore(int k, DukePlayer_t *ps)
{
    int const cs = ps->cursectnum;
    int32_t const plc = movefifoplc;
    int32_t const myFlorz = getflorzofslope((int16_t)cs, ps->pos.x, ps->pos.y);
    int const wend = sector[cs].wallptr + sector[cs].wallnum;
    int32_t bestScore = INT32_MIN;
    int bestW = -1, bestNs = -1, bestDoor = 0, bestCrouch = 0;
    for (int w = sector[cs].wallptr; w < wend; w++)
    {
        int const ns = wall[w].nextsector;
        if (ns < 0 || (unsigned)ns >= (unsigned)numsectors || (wall[w].cstat & 1))
            continue;
        int32_t const mx = (wall[w].x + wall[wall[w].point2].x) >> 1;
        int32_t const my = (wall[w].y + wall[wall[w].point2].y) >> 1;
        int const isDoor = Bot_SectorIsDoor(ns);
        int32_t const nf   = getflorzofslope((int16_t)ns, mx, my);
        int32_t const rise = myFlorz - nf;          // z grows down: >0 is a step UP
        if (rise > (21 << 8))
            continue;       // needs a jump -- and a DOOR on a raised sill is
                            // just as unreachable (E1L1 lobby doors: +64px)
        int crouch = 0;
        if (!isDoor)
        {
            int32_t const gap = nf - getceilzofslope((int16_t)ns, mx, my);
            if (gap < (26 << 8))
                continue;                           // sealed slit
            crouch = (gap < (52 << 8));
        }
        int32_t stamp = s_botVisitT[k][ns];
        if (stamp > plc)
            stamp = 0;                              // relaunch left future stamps
        int32_t score = plc - stamp;
        if (ns == s_botPrevSect[k])
            score >>= 4;
        // Prefer exits the feet can take directly from HERE (non-convex rooms
        // hide their far portals behind inner corners and fences); a blocked
        // one can still win, and follow-until-clear steering earns the rest.
        if (!Bot_LineWalkable(ps, mx, my))
            score >>= 6;
        // A crossing we recently could not make drops to the noise floor, so a
        // DIFFERENT exit wins -- but it is a demotion, not a veto, so the sole
        // way out of a room can still be taken rather than trapping the bot.
        // PRIMARY tier is the per-EDGE ring (3+ failures in a short window);
        // the sector-wide dead ring stays as the escalation tier behind it.
        int const avoided = (g_botLtgOn && Bot_AvoidEdgeActive(k, cs, ns))
                            || Bot_DeadExitActive(k, ns);
        if (avoided)
            score >>= 8;
        // Doors are ROAMING WAYPOINTS (user 2026-08-12): a modest bump so the
        // bot actively routes through them (they lead to fresh rooms) now that
        // it opens them on approach -- not so large it fixates or overrides a
        // much staler open exit.
        if (isDoor && !avoided)
            score += 400;
        score += (int32_t)(Bot_Rnd() & 255);        // tiebreak: twin bots split up
        if (score > bestScore)
            { bestScore = score; bestW = w; bestNs = ns; bestDoor = isDoor; bestCrouch = crouch; }
    }
    if (bestW < 0)
        return 0;
    s_botGoal[k]       = 1;
    s_botGoalX[k]      = (wall[bestW].x + wall[wall[bestW].point2].x) >> 1;
    s_botGoalY[k]      = (wall[bestW].y + wall[wall[bestW].point2].y) >> 1;
    s_botGoalSect[k]   = (int16_t)bestNs;
    s_botGoalZ[k]      = getflorzofslope((int16_t)bestNs, s_botGoalX[k], s_botGoalY[k]) - (20 << 8);
    s_botGoalDoor[k]   = (int8_t)bestDoor;
    s_botGoalCrouch[k] = (int8_t)bestCrouch;
    s_botGoalTics[k]   = 0;
    s_botGoalItem[k]   = -1;
    s_botGoalSeen[k]   = 0;
    s_botGoalIsLtg[k]  = 0;     // neighbor-portal errand: NBG-tier, dies on crossing
    return 1;
}

// Item errand: nearest live pickup in the current room or one portal over,
// same floor. Pickup itself is the sim's job -- walking over it is ours.
static int Bot_PlanItem(int k, DukePlayer_t *ps)
{
    int const cs = ps->cursectnum;
    int16_t sects[24];
    int ns = 0;
    sects[ns++] = (int16_t)cs;
    int const wend = sector[cs].wallptr + sector[cs].wallnum;
    for (int w = sector[cs].wallptr; w < wend && ns < 24; w++)
    {
        int const nx = wall[w].nextsector;
        if (nx < 0 || (unsigned)nx >= (unsigned)numsectors || (wall[w].cstat & 1))
            continue;
        int dup = 0;
        for (int q = 0; q < ns; q++)
            if (sects[q] == nx) { dup = 1; break; }
        if (!dup)
            sects[ns++] = (int16_t)nx;
    }
    int best = -1; int32_t bestd = INT32_MAX; int16_t bestSect = -1;
    for (int q = 0; q < ns; q++)
    {
        for (int j = headspritesect[sects[q]]; j >= 0; j = nextspritesect[j])
        {
            auto const &sp = sprite[j];
            if (!Bot_IsPickup(sp.picnum) || (sp.cstat & 32768) || j == s_botItemShun[k])
                continue;
            if (sp.statnum != STAT_DEFAULT && sp.statnum != STAT_ACTOR && sp.statnum != STAT_ZOMBIEACTOR)
                continue;
            if (klabs(sp.z - ps->pos.z) > (72 << 8))
                continue;                           // different floor: not this room
            int32_t const d = klabs(sp.x - ps->pos.x) + klabs(sp.y - ps->pos.y);
            if (d < bestd) { bestd = d; best = j; bestSect = sects[q]; }
        }
    }
    if (best < 0)
        return 0;
    // "If items are in a room, a SMALL CHANCE it tries to pick the item up"
    // means a nearby detour, not a quest: an errand across a giant sector's
    // furniture field times out, rolls the next item, and eats the whole
    // match in one room (v18e: theater bot ping-ponged between pickups 4300
    // units apart and never took any of the five walkable exits).
    if (bestd > 3000 || !Bot_LineWalkable(ps, sprite[best].x, sprite[best].y))
        return 0;
    s_botGoal[k]       = 2;
    s_botGoalX[k]      = sprite[best].x;
    s_botGoalY[k]      = sprite[best].y;
    s_botGoalZ[k]      = sprite[best].z;
    s_botGoalSect[k]   = bestSect;
    s_botGoalDoor[k]   = 0;
    s_botGoalCrouch[k] = 0;
    s_botGoalTics[k]   = 0;
    s_botGoalItem[k]   = (int16_t)best;
    s_botGoalSeen[k]   = 0;
    s_botGoalIsLtg[k]  = 0;     // nearby-item detour: the NBG layer by definition
    return 1;
}

// Every way a commit can end goes through here, so one grep ([ltgend]) shows
// the full LTG lifecycle per seat: why each march ended and after how many
// body failures -- the forensic that named the wedge-cluster ping-pong.
static void Bot_LtgEnd(int k, const char *why)
{
    if (s_botLtgKind[k] == 0)
        return;
    extern int32_t g_netForensics;
    if (g_netForensics)
        LOG_F(INFO, "[ltgend] seat=%d why=%s kind=%d sect=%d item=%d fails=%d plc=%d",
              k, why, (int)s_botLtgKind[k], (int)s_botLtgSect[k],
              (int)s_botLtgItem[k], (int)s_botLtgFails[k], (int)movefifoplc);
    s_botLtgKind[k] = 0;
}

// ── LTG planning: map-wide candidates, one commit ───────────────────────────
// Desirability stand-in for OA's fuzzy weight files (deliberately skipped --
// audit: data-file machinery, not concept): a static class weight, health
// scaled by need, weapons discounted when already owned via the errand's
// natural refusal (the sim just won't pick up what's full -- the one-slot
// shun catches that). prioritizeItems doubles resupply classes.
static int Bot_ItemDesire(DukePlayer_t *ps, int picnum, int prioritize)
{
    int const hurt = (sprite[ps->i].extra <= (ps->max_player_health >> 1));
    int w;
    switch (tileGetMapping(picnum))
    {
    case ATOMICHEALTH__:                      w = 900; break;
    case FIRSTAID__:                          w = hurt ? 900 : 300; break;
    case COLA__: case SIXPAK__:               w = hurt ? 500 : 150; break;
    case SHIELD__:                            w = (ps->inv_amount[GET_SHIELD] < 50) ? 550 : 200; break;
    case JETPACK__:                           w = 700; break;
    case FIRSTGUNSPRITE__: case SHOTGUNSPRITE__: case CHAINGUNSPRITE__:
    case RPGSPRITE__: case FREEZESPRITE__: case DEVISTATORSPRITE__:
    case SHRINKERSPRITE__: case TRIPBOMBSPRITE__: case GROWSPRITEICON__:
                                              w = 650; break;
    case AMMO__: case BATTERYAMMO__: case DEVISTATORAMMO__: case RPGAMMO__:
    case GROWAMMO__: case CRYSTALAMMO__: case HBOMBAMMO__: case AMMOLOTS__:
    case SHOTGUNAMMO__: case FREEZEAMMO__:    w = 400; break;
    default:                                  w = 300; break;
    }
    if (prioritize && w >= 400)
        w <<= 1;
    return w;
}

// Rough full-run ground speed for schedule math, map units per tic (measured
// from [bot1] traces: ~55-70 units/tic in the open). One 512-unit nav tile is
// therefore ~9 tics of travel; only the ORDER of magnitude matters against
// the 768-tic item respawn window.
#define BOT_TICS_PER_TILE 9

// Choose a new map-wide long-term goal. Items first (their BotChooseLTGItem:
// score = desirability / travel), schedule-filtered through the respawn ring
// -- an item that will be BACK by the time we arrive is a valid target
// (their avoidtime - traveltime > 0 skip, be_ai_goal.c:1367). When no item
// survives, a FAR stale sector becomes the roam anchor (the explore
// gradient's map-wide generalization: distance is a BONUS, not a cost, so
// the anchor pulls the bot out of its spawn orbit). Returns 1 with the
// s_botLtg* slots committed for ~390-520 tics.
static int Bot_PlanLtg(int k, DukePlayer_t *ps)
{
    extern int32_t g_netForensics;
    Bot_NavEnsure();
    if (!s_nvgReady)
        return 0;
    uint32_t const from = Nvg_Snap(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum);
    if (from == NVG_INVALID || Bot_NvgFloodSectDist(k, from) <= 0)
        return 0;
    int32_t const plc = movefifoplc;
    bool prioritize;
    {
        int const w = ps->curr_weapon;
        bool const lowAmmo = (unsigned)w < MAX_WEAPONS && ps->max_ammo_amount[w] > 0
                             && ps->ammo_amount[w] <= (ps->max_ammo_amount[w] >> 3);
        prioritize = (sprite[ps->i].extra <= (ps->max_player_health >> 1)) || lowAmmo;
    }
    // ---- pass 1: pickups, map-wide --------------------------------------
    int32_t bestScore = 0;
    int bestItem = -1, bestDist = 0;
    int scheduleBlocked = 0;    // candidates existed but ALL were respawn-avoided
    static int16_t const stats[3] = { STAT_DEFAULT, STAT_ACTOR, STAT_ZOMBIEACTOR };
    for (int si = 0; si < 3; si++)
    for (int j = headspritestat[stats[si]]; j >= 0; j = nextspritestat[j])
    {
        auto const &sp = sprite[j];
        if (!Bot_IsPickup(sp.picnum) || (unsigned)sp.sectnum >= (unsigned)numsectors)
            continue;
        if (j == s_botItemShun[k])
            continue;
        int const dist = s_ltgSectDist[sp.sectnum];
        // NBG DIVISION OF LABOR: Bot_PlanItem's detour leash is 3000 units
        // (~6 tiles), rolled every few thinks idle AND during the march --
        // anything closer than that is already served without a commit.
        // Letting near items into the LTG menu re-anchored bots to their
        // spawn cluster (the desirability/dist score makes a dist-4 item
        // unbeatable); the LTG exists to pull CROSS-MAP.
        if (dist < 7)
            continue;               // unreachable, or NBG-leash range
        // Respawn schedule: a sprite parked with cstat bit 32768 is PENDING
        // RESPAWN. Skip it only if it will still be gone on arrival; without
        // a ring stamp (we never saw it taken) assume the full window.
        int32_t const travel = dist * BOT_TICS_PER_TILE;
        int32_t until = Bot_ItemAvoidUntil(k, j);
        if ((sp.cstat & 32768) && until == 0)
            until = plc + g_itemRespawnTime;
        if (until > plc + travel)
            { scheduleBlocked++; continue; }
        int32_t const score = ((int32_t)Bot_ItemDesire(ps, sp.picnum, prioritize) << 8)
                              / dist + (int32_t)(Bot_Rnd() & 31);
        if (score > bestScore)
            { bestScore = score; bestItem = j; bestDist = dist; }
    }
    if (bestItem >= 0)
    {
        s_botLtgKind[k] = 2;
        s_botLtgItem[k] = (int16_t)bestItem;
        s_botLtgX[k]    = sprite[bestItem].x;
        s_botLtgY[k]    = sprite[bestItem].y;
        s_botLtgZ[k]    = sprite[bestItem].z;
        s_botLtgSect[k] = sprite[bestItem].sectnum;
        // Stamp on CHOICE (their BotAddToAvoidGoals at :1425): predicted absent
        // for one respawn window from now, so the next re-plan doesn't bounce
        // straight back whether we got it or failed to reach it.
        Bot_ItemAvoidStamp(k, bestItem, plc + g_itemRespawnTime);
    }
    else
    {
        // Deadlock escape (their ai_dmnet.c:319): everything worth having was
        // schedule-avoided -> forget the schedule rather than freeze.
        if (scheduleBlocked)
            Bot_ItemAvoidReset(k);
        // ---- pass 2: FAR stale sector as roam anchor --------------------
        int32_t bestR = INT32_MIN;
        int bestS = -1;
        for (int s = 0; s < numsectors; s++)
        {
            int const dist = s_ltgSectDist[s];
            if (dist < 4)
                continue;           // FAR anchors only: adjacency is the old orbit
            if (Bot_DeadExitActive(k, s))
                continue;
            int32_t stamp = s_botVisitT[k][s];
            if (stamp > plc)
                stamp = 0;          // relaunch left future stamps
            int32_t stale = plc - stamp;
            if (stale > 26 * 180)
                stale = 26 * 180;
            // Distance bonus CAPPED at mid-range: uncapped, the farthest
            // corner always won and the bot ping-ponged between the map's
            // two extremes down the same corridors (measured: sect 237<->
            // 270/271 all match). Past ~12 tiles staleness dominates, so
            // successive commits sweep DIFFERENT mid-distance clusters.
            int32_t const score = stale + min(dist, 12) * 96 + (int32_t)(Bot_Rnd() & 511);
            if (score > bestR)
                { bestR = score; bestS = s; }
        }
        if (bestS < 0)
            return 0;
        s_botLtgKind[k] = 1;
        s_botLtgItem[k] = -1;
        uint32_t const anchor = s_ltgSectNode[bestS];
        s_botLtgX[k]    = Nvg_NodeX(anchor);
        s_botLtgY[k]    = Nvg_NodeY(anchor);
        s_botLtgZ[k]    = s_nvgNode[anchor].floorZ - (20 << 8);
        s_botLtgSect[k] = s_nvgNode[anchor].sector;
        bestDist        = s_ltgSectDist[bestS];
    }
    s_botLtgUntil[k] = plc + 390 + (int32_t)(Bot_Rnd() % 131);   // ~15-20s commit
    s_botLtgFails[k] = 0;
    if (g_netForensics)
        LOG_F(INFO, "[ltg] seat=%d plan kind=%s sect=%d dist=%d plc=%d",
              k, s_botLtgKind[k] == 2 ? "item" : "roam",
              (int)s_botLtgSect[k], bestDist, (int)plc);
    return 1;
}

// Ensure a live LTG and (re)issue the errand that IS its body. The errand
// machinery below stays the single execution engine; this marks the errand
// goalIsLtg so it SURVIVES sector crossings and gets re-issued -- resumed --
// after every fight and detour until the commit deadline. Returns 0 when the
// feature is off or no goal can be planned (caller falls back to the old
// neighbor-portal explore).
static int Bot_LtgErrand(int k, DukePlayer_t *ps)
{
    if (!g_botLtgOn)
        return 0;
    int32_t const plc = movefifoplc;
    if (s_botLtgKind[k] == 2)
    {
        int const it = s_botLtgItem[k];
        if ((unsigned)it >= MAXSPRITES || !Bot_IsPickup(sprite[it].picnum))
            Bot_LtgEnd(k, "gone");              // deleted for good
        else if (sprite[it].cstat & 32768)
        {
            // OBSERVED taken (by anyone, us included) while committed: refresh
            // the schedule from this sighting and move on.
            Bot_ItemAvoidStamp(k, it, plc + g_itemRespawnTime);
            Bot_LtgEnd(k, "taken");
        }
    }
    if (s_botLtgKind[k] == 1 && ps->cursectnum == s_botLtgSect[k])
        Bot_LtgEnd(k, "arrive");                // anchor reached
    if (s_botLtgKind[k] != 0
        && (plc >= s_botLtgUntil[k] || s_botLtgUntil[k] - plc > 700))
        Bot_LtgEnd(k, "deadline");              // commit expired (or plc restarted)
    if (s_botLtgKind[k] == 0 && !Bot_PlanLtg(k, ps))
        return 0;
    s_botGoal[k]       = (s_botLtgKind[k] == 2) ? 2 : 1;
    s_botGoalX[k]      = s_botLtgX[k];
    s_botGoalY[k]      = s_botLtgY[k];
    s_botGoalZ[k]      = s_botLtgZ[k];
    s_botGoalSect[k]   = s_botLtgSect[k];
    s_botGoalDoor[k]   = 0;
    s_botGoalCrouch[k] = 0;
    s_botGoalTics[k]   = 0;
    s_botGoalItem[k]   = (s_botLtgKind[k] == 2) ? s_botLtgItem[k] : -1;
    s_botGoalSeen[k]   = 0;
    s_botGoalIsLtg[k]  = 1;
    return 1;
}

// The sector the current mesh route crosses into next -- the EDGE actually
// being attempted when a goal stalls. Cross-map LTG bodies fail at their
// immediate crossing, not at the (possibly far) goal sector, so the avoid
// mark must land on (cursectnum -> THIS), with the goal sector as fallback
// for straight-shot/off-mesh legs.
static int Bot_RouteNextSect(int k, DukePlayer_t *ps)
{
    for (int i = s_botRouteIdx[k]; i < s_botRouteLen[k] && i < s_botRouteIdx[k] + 6; ++i)
    {
        uint32_t const n = s_botRtNode[k][i];
        if (n >= s_nvgNodeCount)
            break;
        int const sct = s_nvgNode[n].sector;
        if (sct != ps->cursectnum)
            return sct;
    }
    return -1;
}

// DM preferred ENGAGE DISTANCE by OUR current weapon (netduke32 fdmatrix
// dukebot.cpp:73 collapsed to its their-weapon=PISTOL column, floored at 128
// exactly like their max(fdmatrix[..],128) at :798). The steering keeps its
// existing target heading and strafe; only the forward drive changes shape:
// approach when farther than ~1.5x band, strafe-hold within the band, give
// ground inside ~3/4 band. Contact weapons (128) effectively always press in.
static int16_t const s_botEngageDist[MAX_WEAPONS] = {
    128,   // KNEE        (contact)
    1024,  // PISTOL
    512,   // SHOTGUN
    512,   // CHAINGUN
    2560,  // RPG         (self-splash)
    512,   // HANDBOMB
    128,   // SHRINKER
    1536,  // DEVISTATOR
    128,   // TRIPBOMB
    128,   // FREEZE
    2560,  // HANDREMOTE  (pipebomb detonator in hand)
    128,   // GROW
    512,   // FLAMETHROWER (not in their matrix; short-range spray)
};

// PROJECTILE SPEED by OUR current weapon, Build units per tic, for aim leading
// (audit item 6c). 0 = HITSCAN weapon: no leading, the shot lands instantly
// (pistol/chaingun/shotgun/knee/tripbomb/flamethrower). Non-zero = the fired
// projectile's travel speed, so the lead is aimPt = tgt + (dist/projSpeed)*tgtVel
// (their VectorMA(origin,(dist/wi.speed)*speed,dir,bestorigin), ai_dmq3.c:3480).
// The live values are CON-data-driven (g_tile[aplWeaponShoots[w][k]].proj->vel),
// which IS reachable at runtime but adds NULL-check + per-player-index failure
// modes; this is the classic Duke3D constant table instead -- deterministic on
// every peer regardless of CON load, and only the MAGNITUDE scales the lead
// (the leading gate asserts the aim angle CHANGED, not an exact intercept).
static int16_t const s_botProjSpeed[MAX_WEAPONS] = {
    0,     // KNEE        (contact / hitscan)
    0,     // PISTOL      (hitscan)
    0,     // SHOTGUN     (hitscan)
    0,     // CHAINGUN    (hitscan)
    644,   // RPG         (classic rocket velocity)
    0,     // HANDBOMB    (thrown ballistic arc -- no linear lead)
    768,   // SHRINKER    (SHRINKSPARK)
    644,   // DEVISTATOR  (fires RPG-class projectiles)
    0,     // TRIPBOMB    (placed)
    1024,  // FREEZE      (FREEZEBLAST -- fast, bounces)
    0,     // HANDREMOTE  (pipebomb detonator)
    768,   // GROW        (GROWSPARK / expander)
    0,     // FLAMETHROWER (short spray)
};

static input_t Bot_GetInput(int k)
{
    input_t in = {};
    auto const ps = g_player[k].ps;

    // Separation telemetry (forensics only): min pairwise player distance
    // every ~20s names the remaining bottleneck -- travel (never close) vs
    // conversion (close but missing).
    {
        extern int32_t g_netForensics;
        if (g_netForensics && (movefifoplc & 511) == 0 && movefifoplc != s_botSepPlc)
        {
            s_botSepPlc = movefifoplc;
            int32_t minSep = INT32_MAX; int a, b;
            TRAVERSE_CONNECT(a)
            {
                auto const pa = g_player[a].ps;
                if (pa == NULL || pa->dead_flag) continue;
                TRAVERSE_CONNECT(b)
                {
                    if (b <= a) continue;
                    auto const pb = g_player[b].ps;
                    if (pb == NULL || pb->dead_flag) continue;
                    int32_t const s = klabs(pa->pos.x - pb->pos.x) + klabs(pa->pos.y - pb->pos.y)
                                      + (klabs(pa->pos.z - pb->pos.z) >> 4);
                    if (s < minSep) minSep = s;
                }
            }
            int sct[5] = { -1, -1, -1, -1, -1 };
            int na = 0;
            TRAVERSE_CONNECT(a)
            {
                if (na >= 5) break;
                sct[na++] = (g_player[a].ps != NULL) ? g_player[a].ps->cursectnum : -1;
            }
            EM_ASM({ console.log('[botsep] plc=' + $0 + ' minsep=' + $1 + ' sect=' + $2 + ',' + $3 + ',' + $4 + ',' + $5 + ',' + $6); },
                   movefifoplc, minSep, sct[0], sct[1], sct[2], sct[3], sct[4]);
            // Jump audit: cumulative SK_JUMP presses per seat. The user's
            // metric -- "jumping around constantly makes the bot seem like an
            // idiot" -- should read near-flat outside water/stuck escapes.
            EM_ASM({ console.log('[botact] plc=' + $0 + ' jumps=' + $1 + ',' + $2 + ',' + $3 + ',' + $4 + ',' + $5); },
                   movefifoplc, s_botJumps[0], s_botJumps[1], s_botJumps[2], s_botJumps[3], s_botJumps[4]);
        }
    }
    // The pump can ask for a column BEFORE the level exists (seated at
    // relaunch, entry still loading): ps->i/cursectnum are garbage then, and
    // cansee() on a garbage sector walks broken lists and crashes. Neutral
    // input until the seat is live in a real sector.
    if (ps == NULL || (unsigned)ps->i >= MAXSPRITES
        || (unsigned)ps->cursectnum >= (unsigned)numsectors)
        return in;

    if (sprite[ps->i].extra <= 0 || ps->dead_flag)
    {
        s_botWasDead[k] = 1;
        // Respawn wants SK_OPEN: the CON death state gates resetplayer on
        // ifhitspace (gameexec.cpp:3348), NOT fire -- fire-only presses left
        // every dead bot a corpse forever (measured dead_flag>1100 with the
        // resetplayer branch fixed and live; the user's decaying matches).
        if ((Bot_Rnd() & 15) == 0)
            in.bits |= BIT(SK_OPEN) | BIT(SK_FIRE);
        return in;
    }
    // First live tic for this seat: seed the trap machinery from REALITY.
    // Zero-init left trapDir pointing at Build angle 0 (due EAST) and the
    // trap anchor at the map origin -- the first displacement check then
    // computed a garbage "escape heading" from the origin vector, and a bot
    // that got pinned before real travel held that compass line into a wall
    // forever (the user's "doing that east thing", live 2026-08-10).
    if (!s_botLive[k])
    {
        // The authoritative reset paths should have run already. Keep this
        // first-live fallback centralized too: no ad-hoc target fragments.
        Bot_ResetSeat(k, BOT_RESET_BODY);
        s_botLive[k]       = 1;
        s_botTrapAnchor[k] = ps->pos.xy;
        s_botTrapDir[k]    = (int16_t)(fix16_to_int(ps->q16ang) & 2047);
        s_botBounceAng[k]  = s_botTrapDir[k];
    }
    if (s_botWasDead[k])
    {
        // A respawn is a new physical incarnation. Run the same BODY contract as
        // seat reuse/localbot toggles, but preserve the private RNG cadence: the
        // historical respawn path consumed exactly one wander draw here.
        int16_t const respawnWander = (int16_t)(Bot_Rnd() & 2047);
        Bot_ResetSeat(k, BOT_RESET_BODY);
        s_botWanderAng[k]  = respawnWander;
        s_botLive[k]       = 1;
        s_botTrapAnchor[k] = ps->pos.xy;
        s_botTrapDir[k]    = (int16_t)(fix16_to_int(ps->q16ang) & 2047);
        s_botBounceAng[k]  = s_botTrapDir[k];
    }

    int const skill = clamp(g_botSkillEnv >= 0 ? g_botSkillEnv : g_netBotSkill, 0, 3);
    // COOP: the bot is a teammate. It fights MONSTERS and is blind to human
    // players -- no player target, no player revenge (user 2026-08-12: a coop
    // bot "should never try to revenge attack another player"). Everything
    // player-targeting below is gated on !botCoop; the coop branch hunts the
    // nearest enemy sprite instead.
    bool const botCoop = (g_gametypeFlags[ud.coop] & GAMETYPE_COOP) != 0;
    // TEAM game (TDM): seats carry ps->team (set from pteam at spawn,
    // premap.cpp:1849, re-synced per tic in game.cpp; the same field the
    // friendly-damage null and frag credit compare against). The DM
    // acquisition scan and the retaliation/revenge lock below are gated on
    // it -- without the gate TDM bots hunted TEAMMATES all match, every shot
    // nulled (audit item 7, bug-level). Pure DM: flag false, zero change.
    bool const botTeamGame = !botCoop && (g_gametypeFlags[ud.coop] & GAMETYPE_TDM) != 0;
    // LOW-RESOURCE ITEM PRIORITY (netduke32 dukebot.cpp:771 shape, our
    // fields): badly hurt, or nearly dry on the current weapon -> a pickup
    // errand outranks CLOSING on the combat target. It is a priority flip,
    // not a flee: facing and firing at a visible target stay untouched; only
    // the movement bias hands the body to the errand machinery below.
    bool prioritizeItems = false;
    {
        int const w = ps->curr_weapon;
        bool const lowAmmo = (unsigned)w < MAX_WEAPONS && ps->max_ammo_amount[w] > 0
                             && ps->ammo_amount[w] <= (ps->max_ammo_amount[w] >> 3);
        prioritizeItems = (sprite[ps->i].extra <= (ps->max_player_health >> 1)) || lowAmmo;
    }
    // Skill knobs, one column per Duke difficulty the host picked:
    //   0 Piece Of Cake / 1 Let's Rock / 2 Come Get Some / 3 Damn I'm Good.
    // turn cap (ang units/tic), fire gate (max aim-off to shoot), aim wobble
    // amplitude, reaction hold (tics between retarget decisions). Come Get Some
    // (2) is the tuned "hard" default; Damn I'm Good (3) is the new hardest
    // tier -- snappiest tracking, widest fire gate, least wobble, fastest hold.
    static int const turnCap[4]  = { 40, 64, 84, 104 };  // CGS 104->84 (v39: bots snap
                                                          // onto a strafing player slower --
                                                          // "the bot killed me far easier");
                                                          // DIG restores the 104 snap.
    static int const fireGate[4] = { 56, 96, 160, 200 };
    static int const wobble[4]   = { 96, 48, 24, 12 };   // CGS 16->24; DIG 12
    static int const holdMax[4]  = { 30, 16, 6, 4 };
    // LOS reaction delay: a newly-seen player must hold line of sight this
    // many tics before the bot locks on -- until then it keeps roaming (user:
    // "spend more time roaming and not notice players by LOS so quickly").
    // reactTics ALSO gates the FIRE decision now (audit item 6a): fire only
    // after the target has held continuous sight this long (s_botFireSight).
    static int const reactTics[4] = { 60, 42, 26, 16 };  // ~2.0 / 1.4 / 0.87 / 0.53 s
    // ── Difficulty COLUMNS added by wave 3b (audit "Skill columns to ADD"):
    //  ALERTNESS -- acquisition RADIUS cap (Build units, the acquisition scan's
    //   own klabs(dx)+klabs(dy)+(dz>>2) metric). Their 900+alertness*4000 shape
    //   (ai_dmq3.c:3081) scaled to Build: a visible player FARTHER than this is
    //   NOT noticed (we used to acquire at any distance with LOS). Higher skill
    //   sees farther. E1L1's long street exceeds the low tiers, so the cap
    //   actually bites there. NN_BOTALERTCAP overrides all four for testing.
    static int const alertRadius[4] = { 8000, 14000, 22000, 34000 };
    //  AIM_SKILL tier -- 0 disables projectile LEADING and predictive aim (the
    //   dumb tier just faces the raw target); >=1 enables linear leading (#3)
    //   (their aim_skill>0.4 linear-prediction gate, ai_dmq3.c:3474). It also
    //   scales the always-on aim-error FLOOR below.
    static int const aimLead[4]  = { 0, 1, 1, 1 };
    //  FIRETHROTTLE -- fire duty cycle out of 256 (their CHARACTERISTIC_
    //   FIRETHROTTLE random>throttle wait model, ai_dmq3.c:3643). Higher =
    //   shoots more of the time; low tiers pulse the trigger like a human
    //   instead of holding a continuous beam.
    static int const fireThrottle[4] = { 150, 190, 220, 245 };
    int const alertR = (g_botAlertCap > 0) ? g_botAlertCap : alertRadius[skill];

    // Crossing into a new sector invalidates the plotted portal: re-plan now.
    // It also feeds the room memory: stamp where we are, remember where we
    // came from, and retire a portal errand the moment it is consumed.
    if (ps->cursectnum != s_botLastSect[k])
    {
        s_botPrevSect[k]  = s_botLastSect[k];
        s_botLastSect[k]  = ps->cursectnum;
        s_botThinkHold[k] = 0;
        s_botVisitT[k][ps->cursectnum] = movefifoplc;
        s_botThreadFails[k] = 0;    // real progress: the field is behind us
        Bot_ClearDeadExit(k, ps->cursectnum);   // reached it -> it was reachable
        Bot_ClearAvoidEdgeTo(k, ps->cursectnum);// crossings INTO it proven good
        Bot_RoamStamp(k, ps->cursectnum);       // [roam] distinct-sector meter
        if (s_botGoal[k] == 1)
        {
            if (!s_botGoalIsLtg[k])
                s_botGoal[k] = 0;   // neighbor-portal errand consumed: pick fresh
            else if (ps->cursectnum == s_botGoalSect[k])
            {
                s_botGoal[k]     = 0;   // LTG roam anchor REACHED
                Bot_LtgEnd(k, "arrive");// next commit re-planned fresh...
                s_botLtgLocal[k] = 2;   // ...after two LOCAL explore beats:
                                        // march there, then sniff around the
                                        // destination cluster like a player
            }
            // else: the errand IS the committed LTG body -- crossing rooms is
            // its JOB, the march continues (the old discard here was the
            // spawn-orbit mechanism the two-tier stack exists to cure)
        }
    }
    else if ((movefifoplc & 31) == 0)
        s_botVisitT[k][ps->cursectnum] = movefifoplc;   // lingering ages a room too
    if (s_botJumpCool[k] > 0)
        s_botJumpCool[k]--;
    for (int di = 0; di < BOT_DEAD_N; di++)      // age out impossible-exit avoidance
        if (s_botDeadCool[k][di] > 0)
            s_botDeadCool[k][di]--;
    // [roam] telemetry: cumulative DISTINCT sectors entered this level, one
    // line per seat per ~10s -- the user-visible depth meter the smoke gates
    // on (and knob-independent, so baseline and test legs read the same way).
    {
        extern int32_t g_netForensics;
        if (g_netForensics
            && (movefifoplc - s_botRoamLogPlc[k] >= 260 || movefifoplc < s_botRoamLogPlc[k]))
        {
            s_botRoamLogPlc[k] = movefifoplc;
            Bot_RoamStamp(k, ps->cursectnum);   // count the spawn room too
            LOG_F(INFO, "[roam] seat=%d visited=%d plc=%d x=%d y=%d sect=%d goal=%d ltg=%d still=%d idle=%d clr=%d",
                  k, (int)s_botRoamCnt[k], (int)movefifoplc,
                  (int)ps->pos.x, (int)ps->pos.y, (int)ps->cursectnum,
                  (int)s_botGoal[k], (int)s_botLtgKind[k],
                  (int)s_botStillTics[k], (int)s_botIdleTics[k],
                  (int)((getflorzofslope(ps->cursectnum, ps->pos.x, ps->pos.y)
                         - getceilzofslope(ps->cursectnum, ps->pos.x, ps->pos.y)) >> 8));
        }
    }

    // RETALIATION (user 2026-08-10: "if a bot is hit, it should change its
    // targets to lock onto the new target"). Runs EVERY tic, ahead of the
    // reaction-gated think block, so a wound re-locks instantly instead of
    // waiting out the roam cadence. A NEW wound is a change in wackedbyactor to
    // a live player that isn't already the target -- so repeated hits from the
    // same attacker don't thrash, but a fresh attacker steals the lock.
    {
        int const wa = ps->wackedbyactor;
        if (botCoop)
        {
            // Coop retaliation targets a MONSTER attacker only. A hit from a
            // human teammate (stray shot, splash) NEVER makes the bot turn on a
            // player -- it stays on monsters and roaming.
            if (wa != s_botLastWacked[k] && wa != s_botMonTgt[k]
                && Bot_IsLiveMonsterTarget(wa)
                && !Bot_DeadExitActive(k, sprite[wa].sectnum))
            {
                s_botMonTgt[k]     = (int16_t)wa;   // lock the monster that hit us
                s_botTargetHold[k] = 0;
                s_botSightTics[k]  = 0;
                s_botFireSight[k]  = 0;             // (#1) fire gate restarts on the new lock
                s_botTgtVValid[k]  = 0;             // (#2) new target: no velocity baseline yet
                s_botAimDegrade[k] = 0;
                s_botNoHitTics[k]  = 0;
                s_botLastTDist[k]  = INT32_MAX;
                s_botSpawnRoam[k]  = 0;
                s_botThinkHold[k]  = (int16_t)(holdMax[skill] + (Bot_Rnd() % holdMax[skill]));
            }
            if ((unsigned)wa < MAXSPRITES)
                s_botLastWacked[k] = (int16_t)wa;
        }
        else if ((unsigned)wa < MAXSPRITES && sprite[wa].picnum == APLAYER
            && (unsigned)sprite[wa].yvel < MAXPLAYERS && sprite[wa].yvel != k)
        {
            int const atk = sprite[wa].yvel;
            if (wa != s_botLastWacked[k] && atk != s_botTarget[k]
                && Bot_IsLivePlayerTarget(k, atk, ps, botTeamGame))
            {
                s_botTarget[k]     = (int8_t)atk;   // hard-lock onto the attacker NOW
                s_botTargetHold[k] = 0;
                s_botSightTics[k]  = 0;             // treat as freshly sighted
                s_botFireSight[k]  = 0;            // (#1) but still earn the fire window by SIGHT
                s_botTgtVValid[k]  = 0;            // (#2) new target: no velocity baseline yet
                s_botAimDegrade[k] = 0;
                s_botNoHitTics[k]  = 0;
                s_botLastTDist[k]  = INT32_MAX;
                s_botPending[k]    = -1;            // cancel any half-built acquisition
                {
                    extern int32_t g_netForensics;  // native lock trace (see [tgt] above)
                    if (g_netForensics)
                        LOG_F(INFO, "[tgt] seat=%d locked %d plc=%d (retaliate)",
                              k, atk, (int)movefifoplc);
                }
                s_botSpawnRoam[k]  = 0;            // stop dispersing -- fight back
                s_botThinkHold[k]  = (int16_t)(holdMax[skill] + (Bot_Rnd() % holdMax[skill]));
            }
            s_botLastWacked[k] = (int16_t)wa;
        }
    }

    // Reacquire target only when the hold expires (reaction time).
    if (--s_botThinkHold[k] <= 0)
    {
        s_botThinkHold[k] = (int16_t)(holdMax[skill] + (Bot_Rnd() % holdMax[skill]));
        if (botCoop)
        {
            // COOP acquisition: hunt the nearest visible MONSTER, plot a mesh
            // route to it, and drop the room errand so the fight is prosecuted.
            // No monster in sight -> patrol via the explore planner (players are
            // never hunted). This whole branch is player-blind by construction.
            int const oldMon = s_botMonTgt[k];
            int const mon = Bot_AcquireMonster(k, ps);
            if (mon != oldMon)
            {
                Bot_SetCombatRouteResult(k, BOT_TARGET_NONE, -1, false);
                s_botFireSight[k] = 0;
                s_botTgtVValid[k] = 0;
                s_botSeenValid[k] = 0;
            }
            s_botMonTgt[k] = (int16_t)mon;
            s_botTarget[k] = -1;              // never a player target in coop
            s_botNavOn[k]  = 0;
            if (mon >= 0)
            {
                Bot_NavEnsure();
                uint32_t const from = Nvg_Snap(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum);
                uint32_t const to   = Nvg_Snap(sprite[mon].x, sprite[mon].y,
                                               sprite[mon].z, sprite[mon].sectnum);
                static uint32_t probe[4];
                if (from != NVG_INVALID && to != NVG_INVALID
                    && (from == to || Bot_NvgPath(k, from, to, probe, ARRAY_SIZE(probe)) > 0))
                {
                    s_botNavOn[k]   = 1;
                    s_botNavSeen[k] = 0;
                    s_botNavX[k]    = sprite[mon].x;
                    s_botNavY[k]    = sprite[mon].y;
                    s_botNavZ[k]    = sprite[mon].z;
                    s_botNavSect[k] = sprite[mon].sectnum;
                }
                Bot_SetCombatRouteResult(k, 2, mon, s_botNavOn[k] != 0);
                // A monster to fight: drop the errand -- unless resources are
                // low and the errand IS the resupply (the priority flip).
                if (!(prioritizeItems && s_botGoal[k] == 2))
                    s_botGoal[k] = 0;
            }
            else
            {
                Bot_SetCombatRouteResult(k, 0, -1, false);
                // No monster in sight: ESCORT the humans -- but only APPROACH
                // DIRECTLY when the bot can actually reach the nearest human
                // (line of sight, i.e. the same room). If the human is in
                // ANOTHER room, the bot ROAMS via the explore planner, which
                // threads portals to find them, instead of beelining into the
                // wall between the rooms (user 2026-08-12: "if the bot isn't in
                // the same room ... go into roam mode to find that player,
                // rather than trying to go directly to them").
                int const human = Bot_NearestHuman(k, ps);
                bool reachable = false;
                if (human >= 0)
                    reachable = cansee(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum,
                                       g_player[human].ps->pos.x, g_player[human].ps->pos.y,
                                       g_player[human].ps->pos.z, g_player[human].ps->cursectnum);
                if (reachable)
                {
                    // FORMATION SLOT: a bit BEHIND and to one side of the player,
                    // NOT their exact position -- the bot escorts without
                    // crowding the player's face (user 2026-08-12: "it should be
                    // following either behind me or to the side ... getting in my
                    // face"). The slot rides with the player's facing; twin bots
                    // take opposite sides. Head to it only when out of position;
                    // once in the slot, hold station (no explore-away drift).
                    auto const     hp   = g_player[human].ps;
                    int const      pang = fix16_to_int(hp->q16ang) & 2047;
                    int32_t const  fdx  = sintable[(pang + 512) & 2047];  // player forward x
                    int32_t const  fdy  = sintable[pang & 2047];          // player forward y
                    int const      back = 1100;                           // trail this far behind
                    int const      side = 512 * ((k & 1) ? 1 : -1);       // offset to one side
                    int32_t const  fx   = hp->pos.x - ((fdx * back) >> 14) + ((fdy * side) >> 14);
                    int32_t const  fy   = hp->pos.y - ((fdy * back) >> 14) - ((fdx * side) >> 14);
                    int32_t const  fd   = klabs(fx - ps->pos.x) + klabs(fy - ps->pos.y);
                    if (fd > 900)   // out of formation: move into the follow slot
                    {
                        s_botGoalX[k]      = fx;
                        s_botGoalY[k]      = fy;
                        s_botGoalZ[k]      = hp->pos.z;
                        s_botGoalSect[k]   = hp->cursectnum;
                        s_botGoal[k]       = 1;
                        s_botGoalDoor[k]   = 0;
                        s_botGoalCrouch[k] = 0;
                        s_botGoalTics[k]   = 0;
                        s_botGoalItem[k]   = -1;
                        s_botGoalSeen[k]   = 0;
                        s_botGoalIsLtg[k]  = 0;     // escort slot: never an LTG body
                    }
                    else
                        s_botGoal[k] = 0;   // in the slot: hold station near the player
                }
                else if (s_botGoal[k] == 0 && !Bot_PlanExplore(k, ps))    // other room / none: ROAM to find
                    s_botWanderAng[k] = (int16_t)(Bot_Rnd() & 2047);
            }
            // Strafe seed at THINK cadence (kept for RNG-sequence parity; the
            // combat model below re-derives strafe per-tic when a target is up).
            if ((Bot_Rnd() & 3) == 0)
                s_botStrafeDir[k] = (Bot_Rnd() & 1) ? 1 : -1;
            if (s_botTurnPref[k] == 0 || (Bot_Rnd() & 63) == 0)
                s_botTurnPref[k] = (Bot_Rnd() & 1) ? 1 : -1;
        }
        else {
        // Fruitless-fixation breaker: ~10s on one target with no kill means it
        // is not actually reachable (measured pathology: every bot pinned on
        // the rooftop-spawn host, pistols eating the ledge wall forever while
        // ground-level enemies walked past each other). Rotate off it.
        int const avoid = (s_botTargetHold[k] > 300) ? s_botTarget[k] : -1;
        // Revenge bias: whoever just shot this bot counts as 4x closer.
        // Damaged pairs CONVERGE -- the measured failure of every prior config
        // was bots orbiting the map without ever committing to one fight.
        int revenge = -1;
        {
            int const wa = ps->wackedbyactor;
            if ((unsigned)wa < MAXSPRITES && sprite[wa].picnum == APLAYER
                && (unsigned)sprite[wa].yvel < MAXPLAYERS)
            {
                revenge = sprite[wa].yvel;
                if (!Bot_IsLivePlayerTarget(k, revenge, ps, botTeamGame))
                    revenge = -1;
            }
        }
        // FIND before FIGHT (user directive): acquisition needs LINE OF SIGHT
        // -- or revenge, because you know who just shot you. The old
        // omniscient nearest-player pick beelined every bot across the map
        // through walls; nothing about it read as patrolling a level. A
        // sighted target sticks for ~5s of pursuit memory after LOS breaks,
        // then the bot returns to its room routine. Height penalty stays x1:
        // falling is free in Duke, a floor apart is CLOSE.
        int best = -1, heard = -1; int32_t bestd = INT32_MAX, heardd = INT32_MAX; int i;
        int bestSeen = 0;
        TRAVERSE_CONNECT(i)
        {
            if (i == k || i == avoid || !Bot_IsLivePlayerTarget(k, i, ps, botTeamGame))
                continue;
            auto const cp = g_player[i].ps;
            // TDM TEAM FILTER (audit item 7, bug-level): teammates are not
            // targets -- not as locks, not as "heard" hunting hints. Their
            // damage is nulled (Net_ApplyClientHit / A_IncurDamage), so every
            // tic spent hunting one accomplished exactly nothing.
            if (botTeamGame && cp->team == ps->team)
            {
                extern int32_t g_netForensics;
                if (g_netForensics && !s_botTeamLogged[k])
                {
                    s_botTeamLogged[k] = 1;     // one-shot proof the filter ran
                    LOG_F(INFO, "[team] seat=%d skipped teammate %d", k, i);
                }
                continue;
            }
            int32_t d = klabs(cp->pos.x - ps->pos.x) + klabs(cp->pos.y - ps->pos.y)
                        + (klabs(cp->pos.z - ps->pos.z) >> 2);
            int seen = 1;
            if (i == revenge)
                d >>= 2;
            else if (!cansee(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum,
                             cp->pos.x, cp->pos.y, cp->pos.z, cp->cursectnum))
            {
                // Unseen but close counts as HEARD -- a hunting hint, not a lock.
                if (d < heardd) { heardd = d; heard = i; }
                continue;
            }
            // ALERTNESS RADIUS CAP (audit item 8): a VISIBLE player farther than
            // this skill's acquisition radius is not noticed -- we used to lock
            // on at any distance the moment LOS existed (their squaredist >
            // Square(900+alertness*4000) skip, ai_dmq3.c:3081). Revenge is
            // exempt: a bullet in the back needs no eyeball range check.
            if (i != revenge && d > alertR)
            {
                extern int32_t g_netForensics;
                if (g_netForensics)
                    LOG_F(INFO, "[alert] seat=%d tgt=%d dist=%d acquired=0", k, i, (int)d);
                continue;
            }
            if (i == revenge)
                seen = cansee(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum,
                              cp->pos.x, cp->pos.y, cp->pos.z, cp->cursectnum);
            // Known-unreachable elevation: a target whose sector we already gave
            // up on ([unreach] below) is not re-locked -- the bot stops fixating
            // on an enemy up a ledge and looks for a reachable one. Revenge is
            // exempt (a bullet in the back overrides the range/reach filters).
            if (i != revenge && Bot_DeadExitActive(k, cp->cursectnum))
                continue;
            if (d < bestd) { bestd = d; best = i; bestSeen = seen; }
        }
        // Pursuit memory: with the last-seen chase live the bot walks to a
        // SNAPSHOT, so the window stretches to ~10s (their chase_time,
        // ai_dmnet.c:2296) -- the touch check below usually ends it sooner.
        // Old wall-tracking pursuit keeps its shorter 130-tic leash.
        if (best < 0 && s_botTarget[k] >= 0 && s_botTarget[k] != avoid
            && Bot_IsLivePlayerTarget(k, s_botTarget[k], ps, botTeamGame)
            && !Bot_DeadExitActive(k, g_player[s_botTarget[k]].ps->cursectnum)
            && s_botSightTics[k] < (g_botLtgOn ? 260 : 130))
            best = s_botTarget[k];      // chase the last sighting briefly
        // REACTION / AWARENESS: don't lock onto a NEWLY-seen player the instant
        // LOS opens -- it must hold sight for reactTics (tracked per-tic below)
        // first, so the bot keeps roaming a beat before it notices. Revenge
        // (just been shot) and the already-held target skip this.
        if (best >= 0 && best != revenge && best != s_botTarget[k])
        {
            if (s_botPending[k] != best) { s_botPending[k] = (int8_t)best; s_botSeeStreak[k] = 0; }
            if (s_botSeeStreak[k] < reactTics[skill])
                best = (Bot_IsLivePlayerTarget(k, s_botTarget[k], ps, botTeamGame)
                        && !Bot_DeadExitActive(k, g_player[s_botTarget[k]].ps->cursectnum)
                        && s_botSightTics[k] < 130) ? s_botTarget[k] : -1;
            else
                s_botPending[k] = -1;   // awareness met: promote to a real lock
        }
        if (best != s_botTarget[k])
        {
            s_botTargetHold[k] = 0;
            s_botNoHitTics[k]  = 0;
            s_botLastTDist[k]  = INT32_MAX;
            s_botSightTics[k]  = 0;
            s_botFireSight[k]  = 0;         // (#1) fresh lock: sight window restarts
            s_botTgtVValid[k]  = 0;         // (#2) drop the old target's velocity baseline
            s_botAimDegrade[k] = 0;
            // Native-visible lock trace (the [aim] decoupling line is EM_ASM,
            // wasm-only): every NEW player lock, so the TDM smoke can assert
            // ZERO teammate acquisitions and the DM legs still show combat.
            extern int32_t g_netForensics;
            if (g_netForensics && best >= 0)
            {
                LOG_F(INFO, "[tgt] seat=%d locked %d plc=%d", k, best, (int)movefifoplc);
                LOG_F(INFO, "[alert] seat=%d tgt=%d dist=%d acquired=1", k, best, (int)bestd);
            }
        }
        s_botTarget[k] = (int8_t)best;
        // Plot the route: ROUTABLE means the MESH says so. The old portal
        // BFS ignored feet (+64px door sills passed its cstat-only filter)
        // and handed out unreachable waypoints -- the last wall-standers
        // were bots frozen at exactly those spots (both user screenshots).
        s_botNavOn[k] = 0;
        if (best >= 0 && g_player[best].ps != NULL)
        {
            auto const tps = g_player[best].ps;
            // Chase destination: the LIVE position only while the target is in
            // sight; once sight breaks, the LAST-SEEN snapshot -- routing to
            // the live position through walls was the omniscient wall-tracking
            // the audit called out (item 5).
            int32_t rx = tps->pos.x, ry = tps->pos.y, rz = tps->pos.z;
            int16_t rsect = tps->cursectnum;
            if (g_botLtgOn && s_botSeenValid[k] && s_botSightTics[k] > 0)
                { rx = s_botSeenX[k]; ry = s_botSeenY[k]; rz = s_botSeenZ[k]; rsect = s_botSeenSect[k]; }
            Bot_NavEnsure();
            uint32_t const from = Nvg_Snap(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum);
            uint32_t const to   = Nvg_Snap(rx, ry, rz, rsect);
            static uint32_t probe[4];
            if (from != NVG_INVALID && to != NVG_INVALID
                && (from == to || Bot_NvgPath(k, from, to, probe, ARRAY_SIZE(probe)) > 0))
            {
                s_botNavOn[k]   = 1;
                s_botNavSeen[k] = 0;
                s_botNavX[k]    = rx;
                s_botNavY[k]    = ry;
                s_botNavZ[k]    = rz;
                s_botNavSect[k] = rsect;
            }
        }
        // Store the newest route verdict; the per-input-tic block below owns
        // elapsed failure time. A think pass never advances the counter itself.
        Bot_SetCombatRouteResult(k, best >= 0 ? 1 : 0, best, s_botNavOn[k] != 0);
        if (best >= 0 && (bestSeen || s_botNavOn[k])
            && !(prioritizeItems && s_botGoal[k] == 2))
            s_botGoal[k] = 0;           // a fight we can PROSECUTE: drop the errand
                                        // (kept when low resources make the
                                        // ITEM errand outrank closing distance)
        else if (s_botGoal[k] == 0)
        {
            // Nobody in sight: run the ROOM ROUTINE. A close unseen enemy
            // is worth half the coin flips (walk toward the noise); a far
            // one still pulls a quarter of them -- pure staleness patrol is
            // ANTI-convergent (another bot's fresh trail reads as "recently
            // visited, skip"), and v18h measured the fleet mutually avoiding
            // itself to minsep ~12000 and zero fights for a full window. The
            // hunt is an ERRAND down a real route, not a target lock, so the
            // wall-hump class stays dead.
            int const huntRoll = (heardd < 10000) ? (int)(Bot_Rnd() & 1)
                                                  : ((Bot_Rnd() & 3) == 0);
            if (heard >= 0 && huntRoll && g_player[heard].ps != NULL)
            {
                // Hunt errand straight at the heard player's AREA; the mesh
                // resolver walks it, and unroutable just times out instead
                // of freezing at a doorway it cannot climb.
                s_botGoalX[k]      = g_player[heard].ps->pos.x;
                s_botGoalY[k]      = g_player[heard].ps->pos.y;
                s_botGoalZ[k]      = g_player[heard].ps->pos.z;
                s_botGoal[k]       = 1;
                s_botGoalSect[k]   = g_player[heard].ps->cursectnum;
                s_botGoalDoor[k]   = 0;
                s_botGoalCrouch[k] = 0;
                s_botGoalTics[k]   = 0;
                s_botGoalItem[k]   = -1;
                s_botGoalSeen[k]   = 0;
                s_botGoalIsLtg[k]  = 0;     // hunt detour: NBG-tier, not the march
            }
            else
            {
                // Goal ladder, OA shape: nearby-item detour (NBG) on the same
                // 1-in-4 roll as before; otherwise the COMMITTED map-wide LTG
                // -- which, because it re-issues here after every fight and
                // detour, is exactly "resume the march"; the old one-room
                // explore survives as the off-mesh fallback, and total plan
                // failure clears the avoid-reach ring (their movement-failure
                // deadlock break, ai_dmnet.c:2200).
                int const wantItem = ((Bot_Rnd() & 3) == 0);
                int planned = (wantItem && Bot_PlanItem(k, ps));
                // Destination sniff: explore beats owed at a just-reached LTG
                // run BEFORE the next cross-map commit -- the room routine
                // works the arrival cluster the way it used to work the
                // spawn cluster, then the march resumes.
                if (!planned && g_botLtgOn && s_botLtgLocal[k] > 0
                    && Bot_PlanExplore(k, ps))
                    { s_botLtgLocal[k]--; planned = 1; }
                if (!planned
                    && !Bot_LtgErrand(k, ps) && !Bot_PlanExplore(k, ps))
                {
                    if (g_botLtgOn)
                        Bot_ClearAvoidEdges(k);
                    s_botWanderAng[k] = (int16_t)(Bot_Rnd() & 2047);   // portal-less void
                }
            }
        }
        // NBG detour DURING the march (their BotNearbyGoal check every ~1s
        // while seeking the LTG, ai_dmnet.c:2306): a leashed nearby pickup
        // may preempt the committed body; Bot_PlanItem's own 3000-unit
        // walk-line leash keeps it a detour, and the march RESUMES from the
        // picker the moment the detour resolves.
        else if (s_botGoalIsLtg[k] && (Bot_Rnd() & 7) == 0)
            Bot_PlanItem(k, ps);
        // Strafe seed at THINK cadence (kept for RNG-sequence parity with the
        // pre-3b brain -- the fixed bot seed makes the roam trajectory sensitive
        // to Bot_Rnd ordering, and dropping this diverged roaming badly; the
        // combat model below OWNS the per-tic strafe once a target is up).
        if ((Bot_Rnd() & 3) == 0)
            s_botStrafeDir[k] = (Bot_Rnd() & 1) ? 1 : -1;
        if (s_botTurnPref[k] == 0 || (Bot_Rnd() & 63) == 0)
            s_botTurnPref[k] = (Bot_Rnd() & 1) ? 1 : -1;
        }   // end DM (player-target) acquisition branch
        // Low-resource resupply: while a fight is on, errand planning above is
        // suppressed -- but with prioritizeItems set the bot WANTS a nearby
        // health/ammo detour to bias movement toward (Bot_PlanItem's own
        // walk-line + 3000-unit leash keeps it a detour, not a quest).
        if (prioritizeItems && s_botGoal[k] == 0
            && (s_botTarget[k] >= 0 || (botCoop && s_botMonTgt[k] >= 0)))
            Bot_PlanItem(k, ps);
    }

    // UNREACHABLE COMBAT TARGET: the latest route result is sampled at think
    // cadence, but elapsed failure time advances exactly once per generated bot
    // input tic. Success/no target/target change reset it immediately. The same
    // state machine serves DM players and coop monsters.
    {
        int const kind = botCoop ? 2 : 1;
        int const target = botCoop ? s_botMonTgt[k] : s_botTarget[k];
        bool const live = botCoop ? Bot_IsLiveMonsterTarget(target)
                                  : Bot_IsLivePlayerTarget(k, target, ps, botTeamGame);
        bool const disengage = BotRouteFailureTick(s_botRouteFail[k], kind, target, live);

        if (disengage)
        {
            int const tsect = botCoop ? sprite[target].sectnum
                                      : g_player[target].ps->cursectnum;
            if ((unsigned)tsect < (unsigned)numsectors)
                Bot_MarkDeadExit(k, tsect);
            extern int32_t g_netForensics;
            if (g_netForensics)
                LOG_F(INFO, "[unreach] seat=%d dropped %s=%d sect=%d failedTics=%d plc=%d",
                      k, botCoop ? "mon" : "tgt", target, tsect,
                      (int)s_botRouteFail[k].failedTics, (int)movefifoplc);
            Bot_ClearTargetState(k);
            s_botThinkHold[k] = 0; // next input replans roam/escort immediately
        }
    }

    // If the current identity/body stopped being a legal target, release it
    // before any target coordinate is read. This also covers invalidation between
    // think passes (disconnect, death/corpse, team change, sprite reuse).
    if (botCoop)
    {
        if (s_botMonTgt[k] >= 0 && !Bot_IsLiveMonsterTarget(s_botMonTgt[k]))
            Bot_ClearTargetState(k);
    }
    else if (s_botTarget[k] >= 0
             && !Bot_IsLivePlayerTarget(k, s_botTarget[k], ps, botTeamGame))
        Bot_ClearTargetState(k);

    // Stuck detection: intending to move but the feet aren't (walls, doors,
    // ledges). Trip the wall-bounce: hard random turn held for a while, press
    // OPEN (doors are everywhere in Duke) and occasionally JUMP (ledges).
    int32_t const stepped = klabs(ps->pos.x - s_botLastPos[k].x) + klabs(ps->pos.y - s_botLastPos[k].y);
    s_botLastPos[k] = ps->pos.xy;
    // ── STRAFE MODEL (audit item 8): flip-on-BLOCKED + time-gated interval,
    // replacing the old 1-in-4-per-think dice roll. (a) if we commanded a
    // strafe last tic and the body barely displaced, the strafe lane is walled
    // -- invert and retry (their 2-attempt flip, ai_dmq3.c:2818); after 2
    // blocked flips stand down a beat so it can't oscillate in place. (b) else
    // change direction only past a time-gated interval (~0.4s + skill jitter)
    // AND on a rare roll (their strafechange_time + random>0.935, :2786). Sets
    // only the strafe SIGN; the wave-1c engage-band forward drive picks the
    // forward magnitude independently, so the two compose. NOTE: our movement
    // is one composed world-velocity vector, so "blocked" is detected as a
    // near-total pin (stepped tiny) rather than an isolated lateral failure.
    // Gated on an ACTIVE combat target: strafe only matters while fighting
    // (s_botStrafeDir feeds the canHit/engage-band movement only), and running
    // this per tic while ROAMING would consume Bot_Rnd every tic and shift the
    // fixed-seed roam trajectory -- measured: host path 63k vs 177k, guest
    // wedged. Confined to combat, the roam Bot_Rnd stream (and thus roaming)
    // is byte-identical to the pre-3b brain.
    static int const strafeIntervalTics[4] = { 16, 14, 12, 10 };  // 0.4 + (1-skill)*0.2 s
    if (s_botTarget[k] >= 0 || (botCoop && s_botMonTgt[k] >= 0))
    {
        if (s_botStrafeDir[k] == 0)
            s_botStrafeDir[k] = (Bot_Rnd() & 1) ? 1 : -1;
        if (s_botWantStrafe[k] && stepped < 24)
        {
            if (s_botStrafeFail[k] < 2)
            {
                s_botStrafeDir[k]  = (int8_t)-s_botStrafeDir[k];   // blocked: invert, try the other side
                s_botStrafeFail[k]++;
                s_botStrafeTic[k]  = 0;
            }
            // else: both sides walled -- leave it; the forward/nav drive escapes
        }
        else
        {
            if (stepped >= 24)
                s_botStrafeFail[k] = 0;                            // moving again: reset the ladder
            int const strInt = strafeIntervalTics[skill] + ((skill >= 3) ? (int)(Bot_Rnd() & 5) : 0);
            if (++s_botStrafeTic[k] > strInt && (Bot_Rnd() & 15) == 0) // ~6% ~ their random>0.935
            {
                s_botStrafeDir[k] = (int8_t)-s_botStrafeDir[k];
                s_botStrafeTic[k] = 0;
            }
        }
    }
    if (stepped < 16)
    {
        s_botStillTics[k]++;            // honesty meter: total stationary time
        if (++s_botStuckTics[k] > 10)
        {
            s_botStuckTics[k] = 0;
            if (s_botBounceHold[k] > 0)
            {
                // A bounce is already steering: do NOT re-arm it. Stuck trips
                // recur every ~10 tics while bounces hold 15-50, so re-arming
                // on every trip locked bots in PERMANENT bounce mode -- the
                // navigator never steered again (bot 1's trace: pinned at one
                // xy for a full 3 minutes, bounce never expiring). Keep
                // the rotation finish; if still stuck after it expires, the
                // door/bounce ladder below runs fresh. NO hop here at all:
                // the re-trip fires every ~10 tics while cornered, and any
                // jump wired to it becomes a metronome (measured 35 presses
                // per 512 tics -- the user's "jumping around constantly").
            }
            else if (s_botOpenGrace[k] == 0)
            {
                // First response to a blockage: if what is AHEAD fronts a
                // door sector, press open and KEEP PUSHING -- doors take
                // tics to swing. Pushing into arbitrary masonry for 26 tics
                // per cycle was the last deliberate wall-lean left (a bot
                // nose-first into the street wall reads as broken even when
                // it recovers); non-door blockages go straight to the
                // bounce/lane ladder next trip.
                int doorish = 0;
                {
                    hitdata_t dh = {};
                    int const da = fix16_to_int(ps->q16ang) & 2047;
                    hitscan(&ps->pos, ps->cursectnum, sintable[(da + 512) & 2047],
                            sintable[da & 2047], 0, &dh, CLIPMASK0);
                    if (dh.wall >= 0 && klabs(dh.xyz.x - ps->pos.x)
                                        + klabs(dh.xyz.y - ps->pos.y) < 1024)
                    {
                        int const dns = wall[dh.wall].nextsector;
                        if ((unsigned)dns < (unsigned)numsectors && Bot_SectorIsDoor(dns))
                            doorish = 1;
                        // Switch-faced walls count: pressing them IS the move.
                        if (wall[dh.wall].lotag > 0)
                            doorish = 1;
                    }
                    else if (dh.sprite >= 0)
                        doorish = 1;        // sprite blocker: press is harmless,
                                            // and the break-fire below handles it
                }
                s_botOpenGrace[k]  = doorish ? 26 : 1;
                in.bits |= BIT(SK_OPEN);
                // Blocker-clearing fire ONLY when a sprite is actually in the
                // way. The old unconditional burst re-armed on every stuck
                // trip: a cornered bot hosed the same wall for minutes, and
                // collateral broke lights/screens map-wide ("some things are
                // blacked out" + "the bots are absolute retards", live).
                hitdata_t hit = {};
                int const a = fix16_to_int(ps->q16ang) & 2047;
                hitscan(&ps->pos, ps->cursectnum, sintable[(a + 512) & 2047],
                        sintable[a & 2047], 0, &hit, CLIPMASK1);
                if (hit.sprite >= 0
                    && klabs(sprite[hit.sprite].x - ps->pos.x)
                       + klabs(sprite[hit.sprite].y - ps->pos.y) < 2048)
                    s_botBreakFire[k] = 8;
                else if (hit.wall >= 0
                         && klabs(hit.xyz.x - ps->pos.x) + klabs(hit.xyz.y - ps->pos.y) < 2048)
                {
                    // A breakable pane/grate/forcefield between us and the
                    // route is an exit with a health bar: shoot it open
                    // ("if it can destroy something to get to an exit").
                    switch (tileGetMapping(wall[hit.wall].overpicnum))
                    {
                    case GLASS__: case GLASS2__: case BIGFORCE__: case W_FORCEFIELD__:
                        s_botBreakFire[k] = 8;
                        break;
                    }
                }
            }
            else
            {
                // Door try spent and still stuck: WALL-FOLLOW, don't dice-roll.
                // Random bounce angles re-rolled a fresh direction every trip
                // and bots milled inside their spawn rooms for entire 5-minute
                // probes (sector telemetry: 3 of 5 players never changed
                // sector). Rotating a CONSISTENT 90 degrees per trip rounds
                // furniture and doorframes like a maze-following bug; the
                // knee-high blockers (theater seats, rails) get a jump only
                // once the trap REPEATS -- episode 2+ -- never as a habit.
                s_botOpenGrace[k]  = 0;
                if (s_botStuckEpisodes[k] < 6)
                    s_botStuckEpisodes[k]++;
                // FURNITURE-FIELD LANE PICK: repeated stuck trips while the
                // wall probe calls the goal line clear mean the blockage is
                // SPRITES (theater chair grids, alley crates) -- geometry the
                // wall-follow cannot trace and clip-slide will not thread at
                // speed (measured: full drive, ~2.5 units/tic net). Do what a
                // player does: look down the open lanes (sprite-AWARE knee-
                // height ray fan) and commit to the longest one that still
                // leans toward the goal -- the E1L1 aisles light up exactly
                // this way.
                if (s_botThreadFails[k] < 8)
                    s_botThreadFails[k]++;
                if ((s_botGoal[k] != 0 || s_botNavOn[k]) && s_botThreadFails[k] >= 2)
                {
                    int32_t const gx = s_botNavOn[k] ? s_botNavX[k] : s_botGoalX[k];
                    int32_t const gy = s_botNavOn[k] ? s_botNavY[k] : s_botGoalY[k];
                    s_botLaneAng[k]    = (int16_t)Bot_PickLane(ps, gx, gy);
                    s_botLaneHold[k]   = 60;
                    s_botBounceHold[k] = 0;         // the lane IS the recovery
                    s_botThinkHold[k]  = 1;
                }
                else
                {
                // Short bounces whenever ANY navigation is live (target route
                // or room errand): long blind rotations steered AWAY from a
                // perfectly good goal for seconds at a time.
                s_botBounceHold[k] = (int16_t)((s_botNavOn[k] || s_botGoal[k] != 0)
                                     ? 15 + (Bot_Rnd() & 15)
                                     : 20 + (Bot_Rnd() & 31) + 16 * s_botStuckEpisodes[k]);
                s_botBounceAng[k]  = (int16_t)((fix16_to_int(ps->q16ang) + s_botTurnPref[k] * 512) & 2047);
                s_botGoalSeen[k] = 0;   // heading changed: re-probe on expiry
                s_botNavSeen[k]  = 0;
                if (s_botStuckEpisodes[k] >= 3 && s_botJumpCool[k] == 0)
                {
                    in.bits |= BIT(SK_JUMP);   // the LAST rung, and rate-limited
                    s_botJumpCool[k] = 78;
                }
                s_botThinkHold[k]  = 1;   // fresh route right after this bounce
                }
            }
        }
    }
    else if (s_botStuckTics[k] > 0)
        s_botStuckTics[k]--;
    else if (s_botStuckEpisodes[k] > 0 && (Bot_Rnd() & 63) == 0)
        s_botStuckEpisodes[k]--;    // moving freely again: forget old traps
    if (s_botOpenGrace[k] > 0)
    {
        s_botOpenGrace[k]--;
        if ((s_botOpenGrace[k] & 7) == 0)
            in.bits |= BIT(SK_OPEN);            // re-press while the grace runs
    }
    // ACTIVE wedge escape (LTG layer; the recovery ladder itself is
    // untouched): standing INSIDE a stamped wedge spot without moving means
    // the walk/crouch escapes are beaten -- keep tapping JUMP on the standard
    // cooldown until the body breaks free. Chairs are knee-high; jumping onto
    // them is the player's move, and the duct block below still strips jumps
    // under low ceilings. With a goal live the ladder's lane branch owns the
    // stuck trips and its bounce-jump rung never fires, so without this the
    // one-shot jump at stall-kill time (~every 200 tics) was the only jump
    // pressure a wedged march ever produced (measured: 50s frozen).
    if (g_botLtgOn && stepped < 16 && s_botJumpCool[k] == 0
        && Bot_NearWedgeSpot(k, ps->pos.x, ps->pos.y, ps->cursectnum))
    {
        in.bits |= BIT(SK_JUMP);
        s_botJumpCool[k] = 78;
    }

    // Errand upkeep: arrival, timeout, and the DOOR/vent etiquette on approach.
    if (s_botGoal[k] != 0)
    {
        int32_t const goalDist = klabs(s_botGoalX[k] - ps->pos.x) + klabs(s_botGoalY[k] - ps->pos.y);
        // PROGRESS WATCH: an explore/hunt errand that stops closing on its goal
        // is an impossible exit -- across a pit, up a ledge, behind a locked
        // door. Abandon in ~3s (and remember the sector) instead of waiting out
        // the 13s timeout below; the staleness gradient would otherwise lure the
        // bot straight back to the one door it can't take.
        if (s_botGoalTics[k] == 0)
            { s_botGoalNear[k] = goalDist; s_botGoalStall[k] = 0; s_botLtgAnchor[k] = ps->pos.xy; }
        else if (s_botGoalIsLtg[k])
        {
            // LTG-body watch: MOVEMENT-based, both goal kinds. The approach
            // test below false-positives on every cross-map march (routes
            // round buildings, distance-to-goal plateaus for whole legs); a
            // healthy march never stops MOVING, so the anchor test catches
            // exactly the grinds (measured: a body wedged in the E1L1 chair
            // field at ~1.5 units/tic held its full cap with zero aborts,
            // while the old test killed clean marches every ~150 tics).
            if (klabs(ps->pos.x - s_botLtgAnchor[k].x)
                + klabs(ps->pos.y - s_botLtgAnchor[k].y) >= 384)
                { s_botLtgAnchor[k] = ps->pos.xy; s_botGoalStall[k] = 0; }
            else if (++s_botGoalStall[k] > 96)
            {
                // PRIMARY response is the per-EDGE mark on the crossing
                // actually being attempted (the route's next sector, not the
                // possibly-distant goal): short TTL, dead only after repeated
                // failures. The old sector-wide 2400-tic ban survives as the
                // ESCALATION tier once one edge racks up 5+ tries.
                int to = Bot_RouteNextSect(k, ps);
                if (to < 0) to = s_botGoalSect[k];
                int const tries = Bot_MarkAvoidEdge(k, ps->cursectnum, to);
                if (tries >= 5 && (unsigned)s_botGoalSect[k] < (unsigned)numsectors)
                    Bot_MarkDeadExit(k, s_botGoalSect[k]);
                s_botRouteLen[k]  = 0;          // force a fresh path next leg
                s_botRouteCool[k] = 0;
                if (++s_botLtgFails[k] >= 2)
                {
                    Bot_LtgEnd(k, "stallkill"); // LTG survives ONE grind (resume
                                                // from wherever recovery moved
                                                // us); the second kills it
                    if ((unsigned)s_botGoalSect[k] < (unsigned)numsectors)
                        s_botVisitT[k][s_botGoalSect[k]] = movefifoplc;
                    // A FAILED item commit re-arms its ring stamp from the
                    // failure, not the choice: the choice-time stamp expired
                    // right as the local rotation returned, and a seat parked
                    // in an item-dense cluster cycled the same three
                    // unreachable pickups all match (measured: the E1L1
                    // cinema 154<->302<->211 triangle). Blocking failures a
                    // full window forces the next plan OUTWARD.
                    if (s_botGoal[k] == 2 && (unsigned)s_botGoalItem[k] < MAXSPRITES)
                        Bot_ItemAvoidStamp(k, s_botGoalItem[k],
                                           movefifoplc + g_itemRespawnTime);
                    // Two zero-movement stalls back to back is a certified
                    // WEDGE. Response is ACTIVE, never idle -- the first cut
                    // paused the picker ~120 tics here to free the recovery
                    // ladder's jump rung, and the coordinator's probe metrics
                    // caught the bill: guest path length 0.65x baseline, the
                    // old brain never idles so every pause was lost ground.
                    // Now: (1) stamp the SPOT so the router and the flood
                    // stop feeding marches back into this pocket, and (2) do
                    // what a player does in a chair field -- JUMP (one tap,
                    // existing cooldown; the duct-crouch block still strips
                    // jumps where the ceiling is low). Planning continues
                    // the same tic; the next commit routes around the spot.
                    Bot_MarkWedgeSpot(k, ps->pos.x, ps->pos.y, ps->cursectnum);
                    if (s_botJumpCool[k] == 0)
                    {
                        in.bits |= BIT(SK_JUMP);
                        s_botJumpCool[k] = 78;
                    }
                }
                {
                    extern int32_t g_netForensics;
                    if (g_netForensics)
                        LOG_F(INFO, "[ltgdrop] seat=%d stall edge=%d->%d tries=%d ltgfails=%d plc=%d",
                              k, (int)ps->cursectnum, to, tries,
                              (int)s_botLtgFails[k], (int)movefifoplc);
                }
                s_botGoal[k] = 0;
            }
        }
        else if (goalDist + 256 < s_botGoalNear[k])
            { s_botGoalNear[k] = goalDist; s_botGoalStall[k] = 0; }   // real approach
        else if (s_botGoal[k] == 1 && (unsigned)s_botGoalSect[k] < (unsigned)numsectors
                 && ++s_botGoalStall[k] > 96)
        {
            if (g_botLtgOn)
            {
                // One-room errand stall: same per-edge primary / sector-tier
                // escalation split as the LTG body above.
                int const tries = Bot_MarkAvoidEdge(k, ps->cursectnum, s_botGoalSect[k]);
                if (tries >= 5)
                    Bot_MarkDeadExit(k, s_botGoalSect[k]);
            }
            else
                Bot_MarkDeadExit(k, s_botGoalSect[k]);
            s_botVisitT[k][s_botGoalSect[k]] = movefifoplc;   // reset its staleness too
            s_botGoal[k] = 0;
        }
        // A committed LTG body legitimately outlives the one-room errand
        // budget (a cross-map march is many rooms); its own commit deadline
        // (~390-520 tics, checked at re-issue) plus the stall watch above
        // bound it, so the hard cap only backstops a slow-but-moving crawl.
        if (s_botGoal[k] != 0 && ++s_botGoalTics[k] > (s_botGoalIsLtg[k] ? 600 : 390))
        {
            // 15s on one errand is a locked door / unreachable shelf: mark it
            // satisfied so the gradient stops wanting it, and move on. EVERY
            // timed-out portal gets stamped, door or not -- an unstamped
            // unreachable neighbor stays the stalest thing in the room and
            // the planner re-picks it forever (the seat-pinned loop).
            if (s_botGoal[k] == 2)
            {
                s_botItemShun[k] = s_botGoalItem[k];
                // Timed-out item LTG: block it a full window from NOW (see
                // the failure re-stamp in the stall branch above).
                if (g_botLtgOn && s_botGoalIsLtg[k]
                    && (unsigned)s_botGoalItem[k] < MAXSPRITES)
                    Bot_ItemAvoidStamp(k, s_botGoalItem[k],
                                       movefifoplc + g_itemRespawnTime);
            }
            else if ((unsigned)s_botGoalSect[k] < (unsigned)numsectors)
                s_botVisitT[k][s_botGoalSect[k]] = movefifoplc;
            if (s_botGoalIsLtg[k])
                Bot_LtgEnd(k, "timeout");       // spent: plan a fresh commit
            s_botGoal[k] = 0;
        }
        else if (s_botGoal[k] == 2)
        {
            int const it = s_botGoalItem[k];
            if ((unsigned)it >= MAXSPRITES || (sprite[it].cstat & 32768)
                || !Bot_IsPickup(sprite[it].picnum))
            {
                // Taken (ideally by us): errand done. OBSERVED pickup is the
                // respawn ring's refresh point -- from here the schedule knows
                // when this sprite pops back (cstat bit 32768 = pending).
                if (g_botLtgOn && (unsigned)it < MAXSPRITES && (sprite[it].cstat & 32768))
                    Bot_ItemAvoidStamp(k, it, movefifoplc + g_itemRespawnTime);
                if (s_botGoalIsLtg[k])
                {
                    Bot_LtgEnd(k, "got");       // LTG satisfied: next think re-plans
                    s_botLtgLocal[k] = 2;       // destination sniff (see crossing)
                }
                s_botGoal[k] = 0;
            }
            else if (goalDist < 512 && s_botGoalTics[k] > 60)
            {
                // Standing ON it and nothing happened -- full health/ammo, the
                // sim refuses. Shun it and get back to the room routine.
                s_botItemShun[k] = (int16_t)it;
                if (s_botGoalIsLtg[k])
                    Bot_LtgEnd(k, "shun");
                s_botGoal[k]     = 0;
            }
        }
        else if (goalDist < 384 && s_botGoalSect[k] < 0)
            s_botGoal[k] = 0;                   // free waypoint (heard-them walk) reached
        // (NO 2D-touch arrival for roam anchors: Build maps stack sectors --
        // E1L1's rooftops overlap the street in 2D, and a touch test here
        // "arrived" every time the bot walked UNDERNEATH its anchor, without
        // stamping it visited -- the same roof then won every re-plan, and
        // the whole leg burned commuting under it. Arrival is ENTERING the
        // goal sector (the crossing handler), with the body timeout as the
        // circles-nearby backstop.)
        if (s_botGoal[k] != 0 && s_botGoalDoor[k] && goalDist < 1600 && (movefifoplc & 7) == 0)
            in.bits |= BIT(SK_OPEN);            // doors get PRESSED, not bumped
        if (s_botGoal[k] != 0 && s_botGoalCrouch[k] && goalDist < 1024)
            in.bits |= BIT(SK_CROUCH);          // duck into the vent as we arrive
    }

    // Post-respawn roam window: target-blind AND trigger-blind, so freshly
    // spawned bots scatter instead of re-joining the spawn-cluster bloodbath
    // (all DM spawns on this map are mutually visible; without this nobody
    // lives long enough to walk anywhere -- measured spread: 220 units).
    if (s_botSpawnRoam[k] > 0)
        s_botSpawnRoam[k]--;
    if (s_botTarget[k] >= 0 && s_botTargetHold[k] < 32000)
        s_botTargetHold[k]++;
    int const t = (s_botSpawnRoam[k] > 0) ? -1 : s_botTarget[k];
    auto const tp = (t >= 0 && Bot_IsLivePlayerTarget(k, t, ps, botTeamGame))
                  ? g_player[t].ps : NULL;
    // Unified target COORDINATES: a player (DM/TDM) or a monster sprite (coop).
    // Every aim/face/fire computation below reads tgX/tgY/tgZ/tgSect instead of
    // tp-> so one path serves both modes; hasTgt means "there is a target". The
    // fire-solution hit test keys on whatever the ray actually strikes
    // (ray.sprite), so the target's own sprite index is not needed here.
    int32_t tgX = 0, tgY = 0, tgZ = 0;
    int tgSect = -1;
    bool hasTgt = false;
    if (botCoop)
    {
        int const ms = s_botMonTgt[k];
        if (s_botSpawnRoam[k] <= 0 && Bot_IsLiveMonsterTarget(ms)
            && !Bot_DeadExitActive(k, sprite[ms].sectnum))
        {
            hasTgt = true;
            tgX = sprite[ms].x; tgY = sprite[ms].y; tgZ = sprite[ms].z - (8 << 8);
            tgSect = sprite[ms].sectnum;
        }
        else
            s_botMonTgt[k] = -1;   // dead / gone: forget it
    }
    else if (tp != NULL)
    {
        hasTgt = true;
        tgX = tp->pos.x; tgY = tp->pos.y; tgZ = tp->pos.z; tgSect = tp->cursectnum;
    }
    bool const seesTarget = (hasTgt && (unsigned)tgSect < (unsigned)numsectors
                             && cansee(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum,
                                       tgX, tgY, tgZ, tgSect));
    // Pursuit-memory clock: acquisition is sight-gated, so how long ago the
    // target was SEEN decides when the chase is called off (think block). Ticks
    // for the coop monster target too, feeding Bot_AcquireMonster's memory.
    if (s_botTarget[k] >= 0 || (botCoop && s_botMonTgt[k] >= 0))
    {
        if (seesTarget)
            s_botSightTics[k] = 0;
        else if (s_botSightTics[k] < 30000)
            s_botSightTics[k]++;
    }
    // (#1) FIRE-SIDE REACTION GATE feed: tics the CURRENT target has been
    // CONTINUOUSLY visible. Fire is gated on this >= reactTics[skill] below, so
    // a bot no longer discharges the same tic LOS opens (their enemysight_time
    // reaction window, BotCheckAttack ai_dmq3.c:3637). Resets the instant sight
    // breaks; the acquisition/retaliation locks above zero it on a target swap.
    if (hasTgt && seesTarget)
        { if (s_botFireSight[k] < 30000) s_botFireSight[k]++; }
    else
        s_botFireSight[k] = 0;
    // (#2) ENEMY-VELOCITY MODEL + direction-change penalty. Snapshot the
    // target's (x,y) about every 15 tics while visible -> a per-tic velocity
    // estimate (their enemyvelocity remembered every 0.5s, ai_dmq3.c:3406). A
    // reversal (dot(newvel,oldvel) < 0) or an abrupt jink degrades THIS seat's
    // aim for ~1s -- feeds the leading (#3) and widens the tracking wobble
    // (their aim_accuracy *= 0.7 when the enemy changed direction, :3423). tgX/
    // tgY here are still the LIVE visible coords (the last-seen chase below only
    // rewrites them once sight is lost, and this samples only while seen).
    if (s_botAimDegrade[k] > 0)
        s_botAimDegrade[k]--;
    if (hasTgt && seesTarget)
    {
        int32_t const dt = movefifoplc - s_botTgtSnap[k];
        if (s_botTgtVValid[k] && dt >= 15 && dt <= 60)
        {
            int32_t const nvx   = (tgX - s_botTgtSX[k]) / dt;   // Build units / tic
            int32_t const nvy   = (tgY - s_botTgtSY[k]) / dt;
            int32_t const moved = klabs(tgX - s_botTgtSX[k]) + klabs(tgY - s_botTgtSY[k]);
            int64_t const dot   = (int64_t)nvx * s_botTgtVX[k] + (int64_t)nvy * s_botTgtVY[k];
            if (moved > 400 && (dot < 0
                    || klabs(nvx - s_botTgtVX[k]) + klabs(nvy - s_botTgtVY[k]) > 96))
                s_botAimDegrade[k] = 30;   // ~1.15s of worse aim after the jink
            s_botTgtVX[k] = nvx; s_botTgtVY[k] = nvy;
            s_botTgtSX[k] = tgX; s_botTgtSY[k] = tgY; s_botTgtSnap[k] = movefifoplc;
        }
        else if (!s_botTgtVValid[k] || dt > 60 || dt < 0)
        {
            // (re)seed the baseline: first sight, the snapshot went stale, or a
            // movefifoplc reset (level change) put dt negative
            s_botTgtSX[k] = tgX; s_botTgtSY[k] = tgY; s_botTgtSnap[k] = movefifoplc;
            s_botTgtVX[k] = 0;   s_botTgtVY[k] = 0;   s_botTgtVValid[k] = 1;
        }
    }
    // CHASE THE LAST-SEEN POSITION (their lastenemyorigin model,
    // ai_dmnet.c:2147/:2296). Snapshot the target's position only WHILE
    // visible; the moment sight breaks, movement/navigation runs at the
    // SNAPSHOT -- not the live position through walls, which was pursuit
    // omniscience for the whole memory window. Reaching the snapshot with
    // nobody there ends the chase on the spot (their touch give-up), which
    // hands the body straight back to the committed LTG. Applies to the DM
    // player-chase AND the coop monster-chase: both ride this one unified
    // tgX/tgY path, so the split brain would cost more than it saved --
    // coop's own monster-hunt priority and shorter 130-tic memory are
    // untouched. AIM stays on the LIVE position for a short beat after
    // losing sight (tgAim*, ~50 tics), then follows the movement heading.
    int32_t const tgAimX = tgX, tgAimY = tgY;   // live coords for the aim tail
    if (hasTgt && seesTarget)
    {
        s_botSeenX[k]     = tgX; s_botSeenY[k] = tgY; s_botSeenZ[k] = tgZ;
        s_botSeenSect[k]  = (int16_t)tgSect;
        s_botSeenValid[k] = 1;
    }
    else if (!hasTgt)
        s_botSeenValid[k] = 0;
    if (g_botLtgOn && hasTgt && !seesTarget && s_botSeenValid[k])
    {
        tgX = s_botSeenX[k]; tgY = s_botSeenY[k]; tgZ = s_botSeenZ[k];
        tgSect = s_botSeenSect[k];
        if (klabs(tgX - ps->pos.x) + klabs(tgY - ps->pos.y) < 600)
        {
            // Stood where they were LAST SEEN and they are not here: the
            // trail is cold. Drop the chase; the think block's next pass
            // resumes the LTG march ("lost him" feeds the roaming layer).
            s_botSeenValid[k] = 0;
            s_botSightTics[k] = 30000;          // memory spent
            if (botCoop) s_botMonTgt[k] = -1; else s_botTarget[k] = -1;
            hasTgt = false;
        }
    }
    // Awareness build-up for a PENDING (not-yet-locked) candidate: it must
    // hold line of sight for reactTics before the acquisition block promotes
    // it. Lose sight -> awareness resets, so the bot only reacts to players
    // it has actually watched for a beat.
    if (s_botPending[k] >= 0 && s_botPending[k] < MAXPLAYERS && s_botPending[k] != k)
    {
        auto const pend = g_player[s_botPending[k]].ps;
        if (Bot_IsLivePlayerTarget(k, s_botPending[k], ps, botTeamGame)
            && !Bot_DeadExitActive(k, pend->cursectnum)
            && cansee(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum,
                      pend->pos.x, pend->pos.y, pend->pos.z, pend->cursectnum))
        {
            if (s_botSeeStreak[k] < 30000) s_botSeeStreak[k]++;
        }
        else { s_botSeeStreak[k] = 0; s_botPending[k] = -1; }
    }

    // "Can SEE" is not "can HIT": cansee() threads through masked walls
    // (chain-link fences, window bars) that hitscan bullets terminate on. The
    // dominant idiot-mode measured live: a bot at the rooftop fence, target
    // visible on the far side at the same height (pitch pinned at 100), hosing
    // pellets into the mesh forever -- 98% of ALL bot fire ended on walls with
    // zero frags across two 3-minute probes, and the collateral shot out
    // lights/screens map-wide. Fire only when the ACTUAL fire solution
    // (current angle + pitch) lands on a player or near the target.
    bool canHit  = false;
    int  ffMate  = -1;   // (#6) FF-veto: teammate the fire solution would cross (-1 none)
    if (seesTarget)
    {
        int const fireAng   = fix16_to_int(ps->q16ang) & 2047;
        int const fireHoriz = fix16_to_int(ps->q16horiz);
        int32_t const vx = sintable[(fireAng + 512) & 2047];
        int32_t const vy = sintable[fireAng & 2047];
        hitdata_t ray = {};
        hitscan(&ps->pos, ps->cursectnum, vx, vy, (100 - fireHoriz) << 5, &ray, CLIPMASK1);
        int32_t const distT  = klabs(tgX - ps->pos.x) + klabs(tgY - ps->pos.y);
        int32_t const rayLen = klabs(ray.xyz.x - ps->pos.x) + klabs(ray.xyz.y - ps->pos.y);
        // FRIENDLY-FIRE LINE VETO (audit item 7 / 6): the fire solution's FIRST
        // sprite hit being a teammate means the shot would strike a friend
        // crossing the line to the real enemy behind them (their BotSameTeam
        // trace check in BotCheckAttack, ai_dmq3.c:3690). Same team test as the
        // TDM acquisition filter. Recorded now, vetoes the FIRE bit below; DM
        // (no teams) never trips it. canHit stays true so tracking is unchanged.
        if (botTeamGame && ray.sprite >= 0 && sprite[ray.sprite].picnum == APLAYER
            && (unsigned)sprite[ray.sprite].yvel < MAXPLAYERS
            && sprite[ray.sprite].yvel != k)
        {
            auto const rmp = g_player[sprite[ray.sprite].yvel].ps;
            if (rmp != NULL && rmp->team == ps->team)
                ffMate = sprite[ray.sprite].yvel;
        }
        // Fire solution lands on a valid victim: a player in DM, ANY enemy
        // monster in coop (the shot damages whatever monster it hits).
        if (ray.sprite >= 0 && (botCoop ? A_CheckEnemySprite(&sprite[ray.sprite])
                                        : sprite[ray.sprite].picnum == APLAYER))
            canHit = true;
        else if (rayLen + 256 < distT)
            canHit = false;   // ray died SHORT of the target: fence/wall between
                              // (256 = torso slack only; the old 1024 let a
                              // target one THIN WALL away count as hittable --
                              // the bot fired into masonry, canHit kept
                              // resetting the no-hit breaker, and the chase
                              // branch drove it face-first into the wall
                              // indefinitely: the user's screenshot)
        else
        {
            // Ray reaches the target's range (or beyond): decide by how far
            // the fire line passes from the target laterally. Endpoint
            // proximity was WRONG here -- a clean near-miss ends on a wall far
            // BEHIND the target and must still count as hittable (v1 gated on
            // it and bots stopped firing entirely: 16 discharges in 3 min).
            int64_t cross = (int64_t)(tgX - ps->pos.x) * vy
                          - (int64_t)(tgY - ps->pos.y) * vx;
            if (cross < 0) cross = -cross;
            canHit = (int32_t)(cross >> 14) < 1024;
        }
        // Fence-camping breaker, PURSUIT-SAFE: only a bot that is unhittable
        // AND not closing distance rotates away -- a bot descending toward
        // its target is unhittable the whole way down and must keep coming.
        if (canHit || distT + 64 < s_botLastTDist[k])
            s_botNoHitTics[k] = 0;
        else if (++s_botNoHitTics[k] > 60)
        {
            s_botTargetHold[k] = 1000;
            s_botNoHitTics[k]  = 0;
        }
        s_botLastTDist[k] = distT;
    }

    // Steering priority: wall-bounce > visible chase > blind homing/wander.
    int wantAng;
    bool chaseTgt = false;      // movement heading is AT the combat target
                                // (arms the DM engage-distance band below)
    if (s_botTrapTics[k] > 104 && !(s_botGoal[k] != 0 && s_botGoalSeen[k] > 0))
    {
        // Hard-trapped: hold the escape axis (the way we came in is the way
        // out of a duct) -- outranks bounce, which only knows walls. An
        // errand with a CLEAR walk line outranks the trap heading though:
        // walking the errand IS the escape, and real displacement resets
        // the trap clock on its own.
        wantAng = s_botTrapDir[k];
    }
    else if (s_botBounceHold[k] > 0)
    {
        s_botBounceHold[k]--;
        wantAng = s_botBounceAng[k];
        // (No rhythmic hop here anymore: the wall-follow rotation does the
        // work, and the metronome jumping was THE tell the user called out.)
    }
    else if (s_botLaneHold[k] > 0)
    {
        s_botLaneHold[k]--;
        wantAng = s_botLaneAng[k];      // committed to the open lane
    }
    else if (hasTgt && (canHit || seesTarget || s_botNavOn[k])
             && !(prioritizeItems && s_botGoal[k] == 2))
    {
        // Toward the target when the shot can land; along the ROUTE (first
        // portal midpoint) when it cannot -- homing at an unhittable target
        // grinds fences forever, the measured no-encounter equilibrium.
        // (With prioritizeItems + a live ITEM errand this branch stands down:
        // the errand branch below drives the BODY to the pickup while the
        // aim/fire path -- keyed on seesTarget/canHit, not on wantAng --
        // keeps facing and shooting the target. Their :841/:1128 behavior.)
        if (!canHit && s_botNavOn[k])
        {
            // Follow-until-clear (chase flavor): straight at the waypoint
            // only while the walk line is open, else hold the follow heading.
            int32_t nx2 = s_botNavX[k], ny2 = s_botNavY[k], nz2 = s_botNavZ[k];
            int16_t nsect2 = s_botNavSect[k];
            int guided2 = 0;
            if ((unsigned)tgSect < (unsigned)numsectors)
                guided2 = Bot_Waypoint(k, ps, tgX, tgY, tgZ, (int16_t)tgSect,
                                       &nx2, &ny2, &nz2, &nsect2);
            if (s_botWpDoor[k] && (movefifoplc & 7) == 0
                && klabs(nx2 - ps->pos.x) + klabs(ny2 - ps->pos.y) < 1600)
                in.bits |= BIT(SK_OPEN);
            if (guided2)
            {
                s_botNavSeen[k] = 1;
                wantAng = getangle(nx2 - ps->pos.x, ny2 - ps->pos.y);
            }
            else
            {
                if ((movefifoplc & ((s_botNavSeen[k] < 0) ? 31 : 15)) == 0 || s_botNavSeen[k] == 0)
                {
                    int const wasBlocked = (s_botNavSeen[k] < 0);
                    s_botNavSeen[k] = Bot_LineWalkable(ps, nx2, ny2) ? 1 : -1;
                    if (s_botNavSeen[k] < 0 && !wasBlocked && s_botBounceHold[k] == 0)
                        s_botBounceAng[k] = (int16_t)(fix16_to_int(ps->q16ang) & 2047);
                }
                wantAng = (s_botNavSeen[k] > 0)
                          ? getangle(nx2 - ps->pos.x, ny2 - ps->pos.y)
                          : s_botBounceAng[k];
            }
        }
        else
        {
            wantAng  = getangle(tgX - ps->pos.x, tgY - ps->pos.y);   // face it (keep shooting)
            // Only chase-ADVANCE a target the mesh can reach. A brief no-route
            // blip (route recompute, target skimming an edge tile) still lets
            // the bot close for ~8 tics; a SUSTAINED no-route (ledge above) it
            // holds + shoots, never grinding toward -- until the [unreach] drop.
            if (s_botNavOn[k] || s_botRouteFail[k].failedTics <= 8)
                chaseTgt = true;
            if (!seesTarget)
                wantAng = (wantAng + (int)(Bot_Rnd() % 129) - 64) & 2047;
        }
    }
    else if (s_botGoal[k] != 0)
    {
        // Follow-until-clear (errand flavor) -- the v18a grinder's cure.
        // Straight at the goal only while the walk line is open; blocked
        // means HOLD the current heading and let stuck trips rotate it a
        // consistent 90 degrees -- tracing the obstacle like a maze bug
        // until the line opens again. Steering back at the goal every tic
        // while blocked was the measured thrash: +-700 ang units of
        // oscillation at max turn rate, ~3 units/tic net drift, zero exits.
        // ROUTE FIRST (OpenArena-style): mesh-guided waypoints get DIRECT
        // steering every tic; the blocked-line crutch survives only for the
        // off-mesh raw fallback.
        int32_t sx2, sy2, sz2;
        int16_t ssect2;
        int const guided = Bot_Waypoint(k, ps, s_botGoalX[k], s_botGoalY[k], s_botGoalZ[k],
                                        s_botGoalSect[k], &sx2, &sy2, &sz2, &ssect2);
        if (s_botWpDoor[k] && (movefifoplc & 7) == 0
            && klabs(sx2 - ps->pos.x) + klabs(sy2 - ps->pos.y) < 1600)
            in.bits |= BIT(SK_OPEN);        // mid-route door: press through it
        if (guided)
        {
            s_botGoalSeen[k] = 1;
            wantAng = getangle(sx2 - ps->pos.x, sy2 - ps->pos.y);
        }
        else
        {
            if ((movefifoplc & ((s_botGoalSeen[k] < 0) ? 31 : 15)) == 0 || s_botGoalSeen[k] == 0)
            {
                int const wasBlocked = (s_botGoalSeen[k] < 0);
                s_botGoalSeen[k] = Bot_LineWalkable(ps, sx2, sy2) ? 1 : -1;
                if (s_botGoalSeen[k] < 0 && !wasBlocked && s_botBounceHold[k] == 0)
                    s_botBounceAng[k] = (int16_t)(fix16_to_int(ps->q16ang) & 2047);
            }
            wantAng = (s_botGoalSeen[k] > 0)
                      ? getangle(sx2 - ps->pos.x, sy2 - ps->pos.y)
                      : s_botBounceAng[k];
        }
    }
    else
    {
        wantAng = s_botWanderAng[k];
        s_botIdleTics[k]++;             // honesty meter: wander-fallback time
    }

    // AIM / MOVE DECOUPLING (user directive 2026-08-10): keep the MOVEMENT
    // heading (wantAng: nav route / roam / circle), but FACE the target when
    // the bot is engaging one it can SEE -- so it tracks-and-shoots while it
    // keeps moving around the same way, instead of turning to walk straight at
    // the enemy. During recovery (trap / bounce / lane) aim stays on the move
    // heading: the bot needs to look where it is escaping to. Movement is
    // rotated by moveAng at the exit block (not by facing), so facing is now
    // free to point at the aim target without dragging the body with it.
    int const moveAng = wantAng;
    // FACE THE ENEMY WHENEVER FIGHTING (user 2026-08-10: "face that person while
    // moving at all times"). No longer suppressed by the trap/bounce/lane
    // recovery states -- during a fight the body still escapes on moveAng, but
    // the HEAD stays locked on the target. "Recently seen" (s_botSightTics small)
    // holds the track through brief occlusion (strafing behind a pillar) and
    // through the wall-follow, and releases only once the target has been out of
    // sight long enough (~3s) that this is navigation, not combat.
    // With the last-seen chase live the face-lock tail shortens to ~50 tics
    // of LIVE tracking after sight breaks (their 2s aim memory), then the
    // head follows the movement heading toward the snapshot; tgAim* holds
    // the live coordinates for exactly that tail.
    bool const engaging = (hasTgt && (seesTarget || canHit
                                      || s_botSightTics[k] < (g_botLtgOn ? 50 : 90)));
    int const rawAng = getangle(tgAimX - ps->pos.x, tgAimY - ps->pos.y);
    int       aimAng = engaging ? rawAng : moveAng;
    // (#3) PROJECTILE LEADING: aim where the target WILL be, not where it is --
    // aimPt = tgt + (dist/projSpeed)*tgtVel (their VectorMA(origin,(dist/
    // wi.speed)*speed,dir,bestorigin), ai_dmq3.c:3480). Projectile weapons only
    // (s_botProjSpeed>0; hitscan never leads), and gated by the AIM_SKILL tier
    // so the dumbest column fires straight. Uses the velocity estimate (#2).
    if (engaging && seesTarget && aimLead[skill]
        && (unsigned)ps->curr_weapon < MAX_WEAPONS && s_botProjSpeed[ps->curr_weapon] > 0
        && (s_botTgtVX[k] || s_botTgtVY[k]))
    {
        int32_t const projS  = s_botProjSpeed[ps->curr_weapon];
        int32_t const ldist  = klabs(tgAimX - ps->pos.x) + klabs(tgAimY - ps->pos.y);
        int32_t const flight = ldist / projS;               // tics for the shot to arrive
        int const     leadAng = getangle(tgAimX + flight * s_botTgtVX[k] - ps->pos.x,
                                          tgAimY + flight * s_botTgtVY[k] - ps->pos.y);
        if (leadAng != rawAng)
        {
            extern int32_t g_netForensics;
            if (g_netForensics)
                LOG_F(INFO, "[lead] seat=%d w=%d rawang=%d leadang=%d",
                      k, (int)ps->curr_weapon, rawAng, leadAng);
            aimAng = leadAng;
        }
    }
    int diff = (((aimAng - fix16_to_int(ps->q16ang)) + 1024) & 2047) - 1024;
    // Tracking: SMALL wobble while the target is visible (the old +-48 at
    // default skill was +-8 degrees of permanent miss -- "the bots can't aim",
    // live-reported), and double the turn rate when far off so they snap on.
    // Medium wobble halved 8->4: 48 honest canHit discharges converted only 2
    // sprite terminations in 5 minutes -- +-8 ang units is +-1.4 degrees of
    // permanent miss at range even with the shotgun cone.
    static int const trackWobble[4] = { 22, 8, 12, 6 };  // v39: CGS 5->12 (~+-2 deg of
                                                       // permanent miss). Bots melted the
                                                       // player with near-perfect tracking
                                                       // once facing was made continuous;
                                                       // loosen so a fight is survivable.
                                                       // DIG 6: tightest track, the deadly tier.
    // Coop aim is a notch WORSE than deathmatch (user 2026-08-12: "its targeting
    // should be slightly worse than deathmatch") -- the bot is a helper, not a
    // threat, so widen the tracking wobble ~+10 units (~+1.8 deg of jitter).
    int twob = trackWobble[skill] + (botCoop ? 10 : 0);
    if (s_botAimDegrade[k] > 0)
        twob += twob >> 1;   // (#2) post-jink penalty ~1.5x wobble (their aim_accuracy *= 0.7)
    // (#4) DISTANCE-SCALED HITSCAN ACCURACY -- DISABLED 2026-08-18 (user: bots
    // aim "really bad" at Come Get Some). It widened the tracking wobble up to
    // +60% at point-blank -- exactly where DM fights happen -- turning close
    // brawls into whiff-fests. The base trackWobble[] is already tuned for a
    // survivable fight; adding the point-blank penalty on top regressed the
    // accuracy the pre-3b brain had. Left here (no-op) as the anchor if a
    // gentler curve is ever wanted.
    int const aimErr = seesTarget ? (int)(Bot_Rnd() % (2 * twob + 1)) - twob
                                  : (int)(Bot_Rnd() % (2 * wobble[skill] + 1)) - wobble[skill];
    int const cap = (seesTarget && klabs(diff) > turnCap[skill]) ? turnCap[skill] * 2 : turnCap[skill];
    // (#5) SECOND-ORDER VIEW MODEL while ENGAGING: a spring-damper toward the
    // aim instead of a hard clamp, so the head visibly OVERSHOOTS a fast angle
    // change then settles (their velocity term + 0.45*(1-factor) damping "over
    // reaction view model", ai_main.c:817). Deterministic (Bot_Rnd-free model);
    // turnCap stays the hard step ceiling; the wobble rides on top as steering
    // noise. Roam/nav turning keeps the crisp direct clamp -- momentum in a
    // navigation turn would oscillate the body's heading and regress roaming.
    // (#5) SECOND-ORDER OVERSHOOT VIEW MODEL -- DISABLED 2026-08-18 (user: bots
    // aim "really bad" / "shooting to the right"). It was underdamped
    // (|lambda|~0.87), so the aim swung PAST the target and a shot fired
    // mid-swing landed to the side -- a directional miss, worst on strafing
    // targets. Accuracy beats head-feel for a combat bot: turn CRISPLY toward
    // the aim like the pre-3b brain that "was working" (the wobble still rides
    // on top as honest steering noise). s_botViewVel kept zeroed for the reset
    // contract; re-enable with proper (critical) damping if head-feel is wanted.
    s_botViewVel[k] = 0;
    in.q16avel = fix16_from_int(clamp(diff + aimErr, -cap, cap));

    int32_t const dist2d = hasTgt ? klabs(tgX - ps->pos.x) + klabs(tgY - ps->pos.y) : INT32_MAX;

    // VERTICAL AIM via the sim's OWN aim keys: raw q16horz deltas fought the
    // auto-centering and OSCILLATED (measured: shot-time horiz swinging
    // 59..127 -- pistols into the floor, 384 shots 0 hits). Holding
    // SK_AIM_UP/DOWN moves pitch at the sim's rate with centering suspended,
    // so overshoot is structurally impossible; SK_CENTER_VIEW levels back
    // off-combat.
    {
        int const curHoriz = fix16_to_int(ps->q16horiz);
        if (seesTarget && dist2d > 256)
        {
            int32_t const dz = tgZ - ps->pos.z;   // eye-to-eye; z grows down
            int const wantHoriz = clamp(100 - (int)(((int64_t)dz * 16) / max(dist2d, 256)), 60, 140);
            if (curHoriz < wantHoriz - 6)
                in.bits |= BIT(SK_AIM_UP);
            else if (curHoriz > wantHoriz + 6)
                in.bits |= BIT(SK_AIM_DOWN);
        }
        else if (klabs(curHoriz - 100) > 10)
            in.bits |= BIT(SK_CENTER_VIEW);
    }
    // MOVEMENT AS A WORLD VELOCITY VECTOR (decoupled from facing). Pick a
    // forward magnitude along the MOVE heading and a strafe magnitude
    // perpendicular to the AIM, then compose them into world vel directly.
    // Forward-along-move + strafe-around-aim is exactly "keep moving the same
    // way, but orbit/track the enemy you're looking at" (user directive).
    int fwdSpd = 0, strSpd = 0;   // strSpd signed: >0 left, <0 right
    bool run = false;
    // Magnitudes trimmed ~15% (user: "move slightly too fast"). 80 was full
    // human run; 68 is a hair under.
    // DM ENGAGE-DISTANCE BAND (capability 4): when the movement heading is AT
    // a hittable target, the forward drive follows the per-weapon preferred
    // distance instead of the old fixed 2048/8192 ladder -- approach beyond
    // ~1.5x band, strafe-hold inside the band, back off inside ~3/4 band
    // (netduke32's fightPos pull toward fdmatrix range, dukebot.cpp:796-848).
    // Strafe keeps the existing s_botStrafeDir orbit. Coop and every recovery
    // state (bounce/lane/trap -- chaseTgt false) keep the old ladder.
    if (!botCoop && canHit && chaseTgt && (unsigned)ps->curr_weapon < MAX_WEAPONS)
    {
        int32_t const band = s_botEngageDist[ps->curr_weapon];
        int32_t const lo   = band - (band >> 2);
        int32_t const hi   = band + (band >> 1);
        if (dist2d > hi)
        {
            if (dist2d <= 8192) { fwdSpd = 48; strSpd = s_botStrafeDir[k] * 28; run = true; } // press in
            else                { fwdSpd = 68; run = true; }                                   // close from afar
        }
        else if (dist2d < lo)            { fwdSpd = -34; strSpd = s_botStrafeDir[k] * 40; }    // give ground
        else                             { fwdSpd = 0;   strSpd = s_botStrafeDir[k] * 40; }    // hold the band
    }
    else if (canHit && dist2d <= 2048)   { fwdSpd = 20; strSpd = s_botStrafeDir[k] * 40; }         // knife: orbit
    else if (canHit && dist2d <= 8192)   { fwdSpd = 48; strSpd = s_botStrafeDir[k] * 28; run = true; } // press
    else if (s_botBounceHold[k] > 0 && klabs(diff) > 150) { fwdSpd = 34; }                          // wall half-stride
    else                                 { fwdSpd = 68; run = true; }                               // roam
    // DM APPROACH WEAVE (user 2026-08-18: "in DM the bots shouldn't run
    // directly at me like coop -- more strafing; although they should always
    // face their target"). This SUPERSEDES the earlier "keep the same movement"
    // directive for DM combat. Whenever a DM bot is closing on a target it can
    // shoot (canHit, advancing, not mid wall-escape), split the forward drive
    // into forward + a STRONG lateral strafe so the approach WEAVES instead of
    // beelining -- most visible on the old "close from afar"/"press" branches
    // that carried little or no strafe. Facing is decoupled and already locked
    // on the target (aimAng=rawAng while engaging), so it strafes WHILE aiming +
    // shooting. Coop monster-hunt (botCoop) keeps the straight charge. The
    // strafe side rides s_botStrafeDir (3b flip-on-blocked + timed reversal) so
    // it zig-zags rather than orbiting one way.
    if (!botCoop && canHit && fwdSpd > 0 && s_botBounceHold[k] == 0)
    {
        if (s_botStrafeDir[k] == 0)
            s_botStrafeDir[k] = (Bot_Rnd() & 1) ? 1 : -1;
        int const drive = fwdSpd;
        fwdSpd = (drive * 5) >> 3;                                              // ~0.62 forward
        strSpd = s_botStrafeDir[k] * max((int)klabs(strSpd), (drive * 3) >> 2); // ~0.75 lateral (>= any existing)
        run = true;
    }
    // UNREACHABLE TARGET (ledge/level the mesh can't route to): never advance
    // toward it -- the canHit "press"/orbit branches above would still walk the
    // bot into the wall under it. Hold ground (fwdSpd 0) and keep the strafe, so
    // it juke-and-shoots in place instead of grinding until the [unreach] drop.
    if (hasTgt && !s_botNavOn[k] && s_botRouteFail[k].failedTics > 8 && fwdSpd > 0)
        fwdSpd = 0;
    // Remember whether we commanded a strafe this tic: next tic's flip-on-
    // blocked (above) checks it against the realized displacement.
    s_botWantStrafe[k] = (strSpd != 0);
    if (run)        in.bits    |= BIT(SK_RUN);
    if (fwdSpd > 0) in.extbits |= BIT(EK_MOVE_FORWARD);
    if (fwdSpd < 0) in.extbits |= BIT(EK_MOVE_BACKWARD);   // engage-band back-off
    if (strSpd) in.extbits |= BIT(strSpd > 0 ? EK_STRAFE_LEFT : EK_STRAFE_RIGHT);
    if (fwdSpd || strSpd)
    {
        int const ma = moveAng & 2047;                                   // body goes here
        int const sa = (aimAng + (strSpd >= 0 ? 512 : 1536)) & 2047;     // strafe ±90° off aim
        int const s  = klabs(strSpd);
        // world vx/vy at angle A, magnitude M: mulscale9(M, sin[A+2560]) / [A+2048]
        // -- the exact convention dukeFillInputForTic uses (game.cpp:6590).
        int32_t const vx = mulscale9(fwdSpd, sintable[(ma + 2560) & 2047])
                         + mulscale9(s,      sintable[(sa + 2560) & 2047]);
        int32_t const vy = mulscale9(fwdSpd, sintable[(ma + 2048) & 2047])
                         + mulscale9(s,      sintable[(sa + 2048) & 2047]);
        in.fvel = (int16_t)clamp(vx, -0x7ff0, 0x7ff0);
        in.svel = (int16_t)clamp(vy, -0x7ff0, 0x7ff0);
    }

    // Fire discipline: the gate uses TRUE aim error (wobble is steering noise,
    // not trigger noise) and halves with distance so long shots need real aim.
    // NO burst cadence: a 24-on/8-off trigger starved every slow weapon cycle
    // (shotgun ~30 tics; measured 11 discharges in 8 minutes) -- the aim gate
    // already modulates fire naturally as the wobble swings.
    // Hard-trap detector: an external push (the E1L1 vent conveyor) can pin a
    // bot with ZERO net displacement while the whole stuck->door->bounce
    // ladder cycles forever -- measured: pinned at one xy for entire probes,
    // rotation free, NO sprite blocker in reach. The canonical trap is a
    // conveyor pressing the bot into a SEALED breakable grate (a WALL, which
    // the sprite-gated break-fire refuses). Escalation: bounded wall-break
    // volleys every ~2s; if ~15s of that opens nothing, give up on this
    // target and roam away -- no return of the map-wide wall-hosing.
    {
        int32_t const trapDisp = klabs(ps->pos.x - s_botTrapAnchor[k].x)
                               + klabs(ps->pos.y - s_botTrapAnchor[k].y);
        if (trapDisp >= 64)
        {
            s_botTrapDir[k]    = (int16_t)getangle(ps->pos.x - s_botTrapAnchor[k].x,
                                                   ps->pos.y - s_botTrapAnchor[k].y);
            s_botTrapTics[k]   = 0;
            s_botTrapAnchor[k] = ps->pos.xy;
            if (trapDisp >= 512)
                s_botTrapRounds[k] = 0;   // genuinely free again
        }
        else if ((in.fvel || in.svel) && ++s_botTrapTics[k] > 104)
        {
            // Reset only on DISPLACEMENT: pinned bots alternate move/turn
            // tics, and resetting on quiet tics would keep this from ever
            // tripping. Escape kit: CROUCH (crawl-height ducts block standing
            // movement in every direction -- the E1L1 vent pin), no jumping
            // (cancels the crouch), volley along the entry axis, and if
            // nothing opens, walk away.
            in.bits |= BIT(SK_CROUCH);
            in.bits &= ~BIT(SK_JUMP);
            // After 3 full escape ladders with no displacement the trap is
            // hopeless (the vent pocket defeats walk/jump/crouch/volleys):
            // go DORMANT -- no more periodic gunfire from the duct all match.
            // The bot still counts as trapped for the join-yield preference.
            if (s_botTrapRounds[k] < 3 && s_botTrapCool[k] == 0)
            {
                s_botBreakFire[k] = 8;
                s_botTrapCool[k]  = 52;
                in.bits |= BIT(SK_OPEN);
            }
            if (s_botTrapTics[k] > 390)
            {
                if (s_botTrapRounds[k] < 3)
                    s_botTrapRounds[k]++;
                s_botTargetHold[k] = 1000;
                s_botSpawnRoam[k]  = 300;
                s_botTrapTics[k]   = 0;
            }
        }
        if (s_botTrapCool[k] > 0)
            s_botTrapCool[k]--;
    }

    // Environmental reflexes ("expand to other interactive options"):
    // swimming is the one sanctioned jump; warp elevators want an OPEN press
    // to run; crawl-height sectors get a proactive duck instead of a 4s trap
    // ladder discovery.
    {
        int const cls = sector[ps->cursectnum].lotag;
        if (cls == ST_2_UNDERWATER)
        {
            if (ps->airleft < 26 * 12 || (Bot_Rnd() & 7) == 0)
                in.bits |= BIT(SK_JUMP);        // surface: air outranks errands
        }
        else
        {
            if (cls == ST_15_WARP_ELEVATOR && (movefifoplc & 63) == 0)
                in.bits |= BIT(SK_OPEN);
            int32_t const clr = getflorzofslope(ps->cursectnum, ps->pos.x, ps->pos.y)
                              - getceilzofslope(ps->cursectnum, ps->pos.x, ps->pos.y);
            // Threshold 52 -> 72 (the jetpack block's own crawl-space bound):
            // 56-71-pixel pockets are too low to WALK standing yet were above
            // the old duck reflex -- a bot standing in one had every step
            // ceiling-blocked, and only the hard-trap's transient crouch
            // inched it ~64 units per 104 tics (measured: minutes pinned at
            // one x in clr=60 and clr=68 pockets, the frozen probe pairs).
            if (clr < (72 << 8))
            {
                in.bits |= BIT(SK_CROUCH);
                in.bits &= ~BIT(SK_JUMP);       // ducts: jumping just grinds the ceiling
            }
        }
    }

    // JETPACK VERTICAL NAV (capability 2; netduke32 dukebot.cpp:1024-1177
    // mined). Strictly OUT of water: underwater the surface rule above owns
    // SK_JUMP -- "air outranks errands" stays supreme, and the jetpack state
    // machine never runs there. While flying, the sim's own controls apply:
    // SK_JUMP ascends, SK_CROUCH descends (their ASCEND/DESCEND :1161-1177).
    {
        bool const inWater = (sector[ps->cursectnum].lotag == ST_2_UNDERWATER);
        if (s_botJetCool[k] > 0)
            s_botJetCool[k]--;
        // Goal z of the CURRENT movement objective: the combat target, the
        // item errand's sprite, or the errand portal's floor (with a body
        // height off it). z grows DOWN, so a goal ABOVE us gives dz > 0.
        int32_t goalZ = 0;
        bool haveGoalZ = false;
        if (hasTgt)
            { goalZ = tgZ; haveGoalZ = true; }
        else if (s_botGoal[k] == 2 && (unsigned)s_botGoalItem[k] < MAXSPRITES)
            { goalZ = sprite[s_botGoalItem[k]].z; haveGoalZ = true; }
        else if (s_botGoal[k] != 0 && (unsigned)s_botGoalSect[k] < (unsigned)numsectors)
            { goalZ = s_botGoalZ[k]; haveGoalZ = true; }
        int32_t const dz = haveGoalZ ? ps->pos.z - goalZ : 0;   // >0: goal ABOVE
        // No flying in crawl spaces: mirrors their (truefz-truecz) <= 72<<8
        // descend clause (:1101) and keeps this block from fighting the duct
        // crouch above and the hard-trap crouch escape.
        bool const canFlyHere = !inWater && (ps->truefz - ps->truecz) > (72 << 8);
        // ARM: fuel to spare (their >106 gate, :1028), the goal far ABOVE
        // (their 48<<8 threshold shape, :1102/:1108), and the ground game
        // failing -- the errand stall watch, the escalating bounce ladder, or
        // the hard-trap clock say we are pinned against rising geometry.
        if (!inWater && ps->inv_amount[GET_JETPACK] > 106
            && s_botJetHold[k] == 0 && !ps->jetpack_on
            && haveGoalZ && dz > (48 << 8) && canFlyHere
            && (s_botGoalStall[k] > 40 || s_botStuckEpisodes[k] >= 2
                || s_botTrapTics[k] > 60))
            s_botJetHold[k] = 260;              // ~8.7s engagement window
        if (s_botJetHold[k] > 0)
        {
            s_botJetHold[k]--;
            bool const fuelOut = (ps->inv_amount[GET_JETPACK] < 106);
            if (!ps->jetpack_on)
            {
                // Not airborne yet: TAP the toggle while the reason holds.
                if (haveGoalZ && dz > (32 << 8) && !fuelOut && canFlyHere)
                {
                    if (s_botJetCool[k] == 0)
                    {
                        in.bits |= BIT(SK_JETPACK);
                        s_botJetCool[k] = 12;
                        s_botJetActs[k]++;
                    }
                }
                else
                    s_botJetHold[k] = 0;        // reason gone before liftoff
            }
            else if (haveGoalZ && dz > (16 << 8) && !fuelOut && canFlyHere)
            {
                in.bits |= BIT(SK_JUMP);        // ASCEND toward the goal z
                in.bits &= ~BIT(SK_CROUCH);
            }
            else
            {
                // Near goal z / goal below / fuel low: DESCEND, and once the
                // floor is close, tap the toggle OFF (their :1161-1169).
                in.bits &= ~BIT(SK_JUMP);
                in.bits |= BIT(SK_CROUCH);
                if (ps->truefz <= ps->pos.z + (72 << 8) && s_botJetCool[k] == 0)
                {
                    in.bits |= BIT(SK_JETPACK); // land: toggle off
                    s_botJetCool[k] = 12;
                    s_botJetHold[k] = 0;
                }
            }
        }
        else if (ps->jetpack_on && !inWater)
        {
            // Jetpack running with NO jet-nav engaged (window expired, or the
            // sim left it on across an event): land it -- descend and toggle
            // off near the floor, so the bot never hovers aimlessly.
            in.bits &= ~BIT(SK_JUMP);
            in.bits |= BIT(SK_CROUCH);
            if (ps->truefz <= ps->pos.z + (72 << 8) && s_botJetCool[k] == 0)
            {
                in.bits |= BIT(SK_JETPACK);
                s_botJetCool[k] = 12;
            }
        }
    }

    // INVENTORY USE (capability 1; netduke32 dukebot.cpp:1195-1199). Verified
    // against OUR P_HandleSharedKeys: SK_MEDKIT (sector.cpp:3159) heals from
    // inv_amount[GET_FIRSTAID] directly and SK_STEROIDS (sector.cpp:2762)
    // starts a dose only at a full 400 bottle -- both plain edge-triggered SK_
    // bits, so a one-tic press IS what a human's key does. Cadences ride
    // Bot_Rnd, the per-seat private RNG -- never the sim's krand.
    {
        // Medkit: hurt below ~2/3 max health, 1-in-25 per tic (their :1195).
        if (ps->inv_amount[GET_FIRSTAID] > 0
            && ps->last_extra < (ps->max_player_health * 2) / 3
            && (Bot_Rnd() % 25) == 0)
        {
            in.bits |= BIT(SK_MEDKIT);
            s_botMedUses[k]++;
        }
        // Steroids: DM only, a visible locked target at close quarters (or
        // caught with the knee out), full bottle, 1-in-100 per tic (:1198).
        if (!botCoop && hasTgt && seesTarget
            && ps->inv_amount[GET_STEROIDS] >= 400
            && (dist2d <= 2560 || ps->curr_weapon == KNEE_WEAPON)
            && (Bot_Rnd() % 100) == 0)
        {
            in.bits |= BIT(SK_STEROIDS);
            s_botSterUses[k]++;
        }
        // Forensics: per-seat activation tallies (medkit/steroids/jetpack), at
        // most one line per seat per ~2s -- lets gates assert the features
        // fire. LOG_F lands in the native harness logs (EM_ASM is a no-op
        // there, compat.h:18) and in the wasm console via stderr.
        extern int32_t g_netForensics;
        if (g_netForensics
            && (in.bits & (BIT(SK_MEDKIT) | BIT(SK_STEROIDS) | BIT(SK_JETPACK)))
            && (movefifoplc - s_botInvLogPlc[k] >= 60
                || movefifoplc < s_botInvLogPlc[k]))   // plc restarted (level change)
        {
            s_botInvLogPlc[k] = movefifoplc;
            LOG_F(INFO, "[botinv] plc=%d seat=%d med=%d ster=%d jet=%d",
                  (int)movefifoplc, k, (int)s_botMedUses[k],
                  (int)s_botSterUses[k], (int)s_botJetActs[k]);
        }
    }

    // PROACTIVE DOOR OPENING, EVERY MODE (user 2026-08-12: "the bot needs the
    // ability to open doors in all modes, and doors should be a waypoint for
    // roaming"). Probe the FACING direction -- which tracks the move heading
    // while roaming -- for a door sector or a switch-tagged wall within reach,
    // and tap OPEN. The bot now opens doors ON APPROACH along its route instead
    // of only after bumping into them (the stuck path), so a closed door is a
    // passable waypoint rather than a wall. Rate-limited to a tap, not a hold.
    if ((movefifoplc & 7) == 0 && (unsigned)ps->cursectnum < (unsigned)numsectors)
    {
        int const da = fix16_to_int(ps->q16ang) & 2047;
        hitdata_t dh = {};
        hitscan(&ps->pos, ps->cursectnum, sintable[(da + 512) & 2047], sintable[da & 2047], 0, &dh, CLIPMASK0);
        if (dh.wall >= 0)
        {
            int32_t const dd  = klabs(dh.xyz.x - ps->pos.x) + klabs(dh.xyz.y - ps->pos.y);
            int const     dns = wall[dh.wall].nextsector;
            if (dd < 1536 && (((unsigned)dns < (unsigned)numsectors && Bot_SectorIsDoor(dns))
                              || wall[dh.wall].lotag > 0))
                in.bits |= BIT(SK_OPEN);
        }
    }

    int gate = fireGate[skill];
    if (dist2d > 8192)  gate >>= 1;
    if (dist2d > 20000) gate >>= 1;
    (void)s_botBurst;
    bool const aimReady = canHit && klabs(diff) < max(gate, 16);
    // (#1) FIRE-SIDE REACTION GATE: the lock, the facing and the tracking all
    // ran already, but the TRIGGER waits until the target has held continuous
    // sight for reactTics (their enemysight_time reaction window, BotCheckAttack
    // ai_dmq3.c:3637). A bot no longer discharges the same tic LOS opens.
    bool const reactReady = s_botFireSight[k] >= reactTics[skill];
    // (#8) FIRETHROTTLE duty cycle: pulse the trigger via Bot_Rnd rather than
    // a continuous beam (their CHARACTERISTIC_FIRETHROTTLE wait/shoot windows,
    // :3643). Higher throttle column -> shoots more of the time. Only consumed
    // when a shot is actually on the table (canHit), so the duty stays coherent.
    bool throttleOK = true;
    if (canHit)
    {
        if (s_botThrWait[k] > 0)        { s_botThrWait[k]--;  throttleOK = false; }
        else if (s_botThrShoot[k] > 0)  { s_botThrShoot[k]--; throttleOK = true;  }
        else if ((int)(Bot_Rnd() & 255) > fireThrottle[skill])
            { s_botThrWait[k]  = (int16_t)(4 + ((256 - fireThrottle[skill]) >> 4)); throttleOK = false; }
        else
            { s_botThrShoot[k] = (int16_t)(8 + (fireThrottle[skill] >> 5));         throttleOK = true;  }
    }
    // (#6) FRIENDLY-FIRE LINE VETO: a teammate on the fire solution suppresses
    // the shot (ffMate was resolved in the canHit trace; DM has no teams so it
    // is always -1 and this is a no-op there).
    bool const fire = aimReady && reactReady && throttleOK && ffMate < 0;
    if (fire)
        in.bits |= BIT(SK_FIRE);
    {
        extern int32_t g_netForensics;
        // Fire-decision trace: emitted on every tic a shot is on the table
        // (canHit). fired=1 is structurally impossible below the reaction floor,
        // which is exactly what the smoke asserts.
        if (g_netForensics && canHit)
            LOG_F(INFO, "[aimr] seat=%d sightTics=%d react=%d fired=%d",
                  k, (int)s_botFireSight[k], (int)reactTics[skill], (int)fire);
        if (g_netForensics && aimReady && reactReady && throttleOK && ffMate >= 0)
            LOG_F(INFO, "[ffveto] seat=%d blocked teammate=%d", k, ffMate);
    }
    // Blocker-clearing burst ("if an item is in the way of exiting a room,
    // destroy it"): fires along the facing while stuck, only when no player
    // is visible and outside the post-spawn pacifist window. Bullets against
    // walls/doors are harmless; barrels and crates stop blocking.
    if (!fire && s_botBreakFire[k] > 0 && s_botSpawnRoam[k] == 0)
    {
        s_botBreakFire[k]--;
        in.bits |= BIT(SK_FIRE);
    }

    // WORLD-SPACE VELOCITIES: P_ProcessInput consumes input.fvel/svel as
    // vel.x/vel.y DIRECTLY (player.cpp ~5903). The brain computes LOCAL
    // forward/strafe; it MUST be rotated into world components EXACTLY as the
    // human sampler does (game.cpp dukeFillInputForTic), or two things break:
    //   (1) unrotated -> every step goes due EAST (the old bug), and
    //   (2) rotated with the wrong SHIFT -> the bot crawls. mulscale9 (>>9)
    //       against a 2^14 sine table is a 32x gain; the old >>14 was
    //       unit-preserving (NO gain) and the +/-127 clamp is a LOCAL-scale
    //       bound -- together they pinned bots at ~1/20-1/32 of human running
    //       speed ("bots barely move, apart from rotating"). Use mulscale9 and
    //       the human's exact angle offsets; do NOT re-clamp (local fvel is
    //       already <= keyMove, and the world result fits int16). This is a
    //       HOST-side bug: every peer sees the same crawling bots.
    // (No exit rotation: the movement block above already emits WORLD-space
    // fvel/svel composed from moveAng + aimAng directly, decoupled from
    // facing. P_ProcessInput consumes them as vel.x/vel.y unchanged.)

    // Single-bot decision trace (forensics only): the room-escape failure has
    // survived four steering rewrites while the LOCALBOT-piloted seat crosses
    // rooms fine -- read the actual loop instead of guessing a fifth time.
    {
        extern int32_t g_netForensics;
        if (g_netForensics && k == 1 && (movefifoplc % 26) == 0)
        {
            // Name the wedge: nearest BLOCKING sprite within reach. The bot
            // faces open space with fvel=80 and moves ZERO units -- something
            // invisible pins it (suspect: dummy-player ghosts with cstat&1).
            int bPic = -1, bStat = -1, bDist = -1;
            for (int bi = 0; bi < MAXSPRITES; bi++)
            {
                if (sprite[bi].statnum >= MAXSTATUS || !(sprite[bi].cstat & 1) || bi == ps->i)
                    continue;
                int32_t const bd = klabs(sprite[bi].x - ps->pos.x) + klabs(sprite[bi].y - ps->pos.y);
                if (bd < 1024 && (bDist < 0 || bd < bDist))
                    { bDist = bd; bPic = sprite[bi].picnum; bStat = sprite[bi].statnum; }
            }
            EM_ASM({ console.log('[bot1] plc=' + $0 + ' x=' + $1 + ' y=' + $2 + ' sect=' + $3
                     + ' ang=' + $4 + ' fv=' + $5 + ' stuck=' + $6 + ' bounce=' + $7
                     + ' blkpic=' + $8 + ' blkd=' + $9); },
                   movefifoplc, ps->pos.x, ps->pos.y, ps->cursectnum,
                   fix16_to_int(ps->q16ang), (int)in.fvel,
                   s_botStuckTics[k], s_botBounceHold[k], bPic, bDist);
            EM_ASM({ console.log('[bot1g] plc=' + $0 + ' vx=' + $1 + ' vy=' + $2 + ' nown=' + $3
                     + ' thold=' + $4 + ' acc=' + $5 + ' fist=' + $6 + ' hland=' + $7); },
                   movefifoplc, ps->vel.x, ps->vel.y, (int)ps->newowner,
                   (int)ps->transporter_hold, (int)ps->access_incs,
                   (int)ps->fist_incs, (int)ps->hard_landing);
            EM_ASM({ console.log('[bot1n] plc=' + $0 + ' goal=' + $1 + ' gx=' + $2 + ' gy=' + $3
                     + ' gsect=' + $4 + ' door=' + $5 + ' tgt=' + $6 + ' sightt=' + $7); },
                   movefifoplc, (int)s_botGoal[k], s_botGoalX[k], s_botGoalY[k],
                   (int)s_botGoalSect[k], (int)s_botGoalDoor[k],
                   (int)s_botTarget[k], (int)s_botSightTics[k]);
        }
    }

    // Decoupling proof (forensics): when engaging with a move heading that
    // differs from the aim, `face` should track `aimAng` while `mvdir` (the
    // world direction of the emitted velocity) tracks `moveAng` -- i.e. the
    // bot looks at the target while the body keeps going its own way.
    {
        extern int32_t g_netForensics;
        if (g_netForensics && k == 1 && (movefifoplc % 26) == 0)
        {
            int const face  = fix16_to_int(ps->q16ang) & 2047;
            int const mvdir = (in.fvel || in.svel) ? getangle(in.fvel, in.svel) : -1;
            EM_ASM({ console.log('[aim] plc=' + $0 + ' eng=' + $1 + ' aimAng=' + $2
                     + ' moveAng=' + $3 + ' face=' + $4 + ' mvdir=' + $5); },
                   movefifoplc, (int)engaging, aimAng, moveAng, face, mvdir);
        }
    }

    // VIEWSCREEN CAMERA LOCK CURE (belt + braces; P_ProcessInput discards
    // ALL movement while newowner >= 0, player.cpp:5660, and only an
    // SK_ESCAPE edge clears cameras, sector.cpp:3382 -- which a bot never
    // presses; its own OPEN taps even keep the toggle latched). Never seen
    // firing on E1L1 probes ([cam] count 0), but a bot that ever activates
    // a screen would otherwise freeze forever, so the cure stays.
    if (ps->newowner >= 0)
    {
        in.bits &= ~BIT(SK_OPEN);
        if (movefifoplc & 1)
            in.bits |= BIT(SK_ESCAPE);
        extern int32_t g_netForensics;
        if (g_netForensics && (movefifoplc - s_botCamLogPlc[k] >= 260
                               || movefifoplc < s_botCamLogPlc[k]))
        {
            s_botCamLogPlc[k] = movefifoplc;
            LOG_F(INFO, "[cam] seat=%d viewscreen lock, clearing (own=%d) plc=%d",
                  k, (int)ps->newowner, (int)movefifoplc);
        }
    }

    if (in.bits & BIT(SK_JUMP))
        s_botJumps[k]++;

    return in;
}

// TEST MODE accessor: the local peer's own input generated by the bot brain
// (game.cpp substitutes it right before Net_HandleInput latches+sends).
input_t Net_BotInput(void)
{
    return Bot_GetInput(myconnectindex);
}

// The transport's slot allocator must NOT hand a joiner a CPU-held seat. This
// accessor is platform-neutral: browser and native admission both call it on
// the game thread, where engine roster state is safe to read.
extern "C" int Net_GetBotMask(void)
{
    return g_netBotMask;
}

void Net_SetLocalBot(int on)
{
    on = on ? 1 : 0;
    if (g_netLocalBot == on)
        return;
    g_netLocalBot = on;
    // Enabling/disabling changes the input owner for this seat. Neither owner
    // inherits the other's lock, route, fire latch or first-live anchor.
    Bot_ResetSeat(myconnectindex, BOT_RESET_BODY);
}

#ifdef NETNATIVE
// Focused H01 fixture over actual engine globals. Enabled only when the native
// executable is launched with NN_TEST_SEATBOTS=1; otherwise the helper is inert.
int Net_TestSeatBotsRelaunch(void)
{
    if (getenv("NN_TEST_SEATBOTS") == NULL)
        return 1;

    for (int k = 0; k < MAXPLAYERS; k++)
        g_player[k].connected = 0;
    myconnectindex = connecthead = 0;
    g_player[0].connected = g_player[1].connected = g_player[2].connected = 1;
    Bstrcpy(g_player[1].user_name, "CPU-1");
    Bstrcpy(g_player[2].user_name, "CPU-2");
    g_netBotMask = (1 << 1) | (1 << 2);

    Net_SeatBots(3, 2);
    int const ok = g_netBotMask == ((1 << 1) | (1 << 2))
                && g_player[1].connected && g_player[2].connected
                && Bstrcmp(g_player[1].user_name, "CPU-1") == 0
                && Bstrcmp(g_player[2].user_name, "CPU-2") == 0
                && numplayers == 3;

    return ok;
}
#endif

#ifdef __EMSCRIPTEN__
extern "C" void Web_SetLocalBot(int on)
{
    Net_SetLocalBot(on);
    EM_ASM({ console.log('[eng] localBot=' + $0); }, g_netLocalBot);
}
#endif

// Seat CPU players (host, pre-launch/relaunch). `minPlayers` is the host's
// MATCH-SIZE FLOOR: bots only fill the seats humans leave empty below it, so
// a lobby with enough humans launches with no bots at all, and a mid-game
// joiner displaces one (the yield lives at the join-flow seat point). Preserve
// CPU ownership for connected seats across Change Map/relaunch; only stale bits
// for seats that really disconnected are discarded. This keeps connected CPU
// seats from silently becoming pseudo-humans that no transport owns.
void Net_SeatBots(int minPlayers, int skill)
{
    int32_t const priorBotMask = g_netBotMask;
    g_netBotMask    = 0;
    g_netBotSkill   = clamp(skill, 0, 3);
    g_netMinPlayers = clamp(minPlayers, 1, 16);   // floor stays 1; cap = MAXPLAYERS seats
    // Central LEVEL reset covers every seat -- including myconnectindex, which
    // the seating loop below skips. The local-bot path can run the brain for the
    // host's own seat, so no zero-initialized target/index may survive seating.
    for (int k = 0; k < MAXPLAYERS; k++)
        Bot_ResetSeat(k, BOT_RESET_LEVEL);
    if (myconnectindex != connecthead)
        return;
    // At menu time the host's OWN connected flag may still be 0 -- without
    // these guards the host's slot looks "free", gets seated as a bot, and
    // the pump then fights the host's sampler over the slot-0 input column
    // (two writers -> aliased cursors -> garbage inputs -> crash at entry).
    g_player[myconnectindex].connected = 1;
    int occupied = 0, retained = 0;
    int teamCount[NET_TEAM_COUNT] = {};
    bool const tdm = (g_gametypeFlags[ud.m_coop] & GAMETYPE_TDM) != 0;
    // Canonical roster scan: transport humans and retained CPU seats both count
    // toward the match-size floor and team balance. A connected CPU keeps its
    // ownership bit across relaunch; only a disconnected prior bit is dropped.
    for (int k = 0; k < MAXPLAYERS; k++)
    {
        if (!g_player[k].connected)
            continue;
        occupied++;
        if (priorBotMask & (1 << k))
        {
            g_netBotMask |= (1 << k);
            retained++;
        }
        if (tdm)
            teamCount[Net_ClampTeam(g_player[k].pteam)]++;
    }
    int const want = max(g_netMinPlayers - occupied, 0);
    int seated = 0;
    for (int k = 0; k < MAXPLAYERS && seated < want; k++)
    {
        if (k == myconnectindex || g_player[k].connected)
            continue;
        G_MaybeAllocPlayer(k);
        Bot_ResetSeat(k, BOT_RESET_LEVEL);
        g_player[k].connected = 1;
        g_netBotMask |= (1 << k);
        Bsprintf(g_player[k].user_name, "CPU-%d", retained + seated + 1);
        // Place each new TDM CPU on the least-populated valid team in the full
        // canonical roster, breaking ties by the lowest team. Retained CPUs keep
        // their existing identity/team; only newly allocated seats are balanced.
        int team = 0;
        if (tdm)
        {
            team = Net_LeastPopulatedTeam(teamCount);
            teamCount[team]++;
        }
        g_player[k].pteam = team;
        seated++;
    }
    // Ownership is coherent now; only then publish the connected roster/chain.
    // Always rebuild: a relaunch that retained every CPU is just as significant as
    // one that allocated a new CPU, and queued non-bot seats may also be present.
    Net_SeatLateJoiners();
    if (seated || retained)
    {
        initprintf("net: retained %d and seated %d CPU player(s), skill %d (mask %x)\n",
                   retained, seated, g_netBotSkill, (unsigned)g_netBotMask);
        if (tdm && g_netForensics)
            LOG_F(INFO, "[team] CPUs: p0=%d p1=%d p2=%d p3=%d mask=0x%x",
                  g_player[0].pteam, g_player[1].pteam, g_player[2].pteam, g_player[3].pteam,
                  (unsigned)g_netBotMask);
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[bot] SeatBots: seated=' + $0 + ' np=' + $1 + ' mask=' + $2); },
               seated, numplayers, g_netBotMask);
#endif
    }
#ifdef __EMSCRIPTEN__
    else
        EM_ASM({ console.log('[bot] SeatBots: seated NOTHING (min=' + $0 + ' me=' + $1 + ' head=' + $2 + ')'); },
               g_netMinPlayers, myconnectindex, connecthead);
#endif
}

// ── LAST MAN STANDING (GAMETYPE_LMS) ────────────────────────────────────────
// Host-authoritative limited lives + elimination + last-standing round win.
// Gated on GTFLAGS(GAMETYPE_LMS) at every call site, so DM/Coop/TDM are wholly
// unaffected. "Eliminated" = the respawn is refused, so the player's death is
// permanent for the round; the sprite stream carries that dead state to guests
// exactly like any other death. Guests never decide or spend lives.
#define LMS_LIVES 3   // respawns per round; 0 left -> eliminated on next death
int8_t  g_lmsLives[MAXPLAYERS];
static int8_t  s_lmsInit;
static int16_t s_lmsCooldown;   // tics to wait after a round reset before re-checking

// Explicit level/match boundary. G_EnterLevel calls this after ud.coop has been
// committed from ud.m_coop and before the authoritative input timeline resumes.
// Non-LMS entry clears every byte too: a later LMS match can never inherit lives
// or cooldown from a prior match in the same process. The lazy init in the death
// and tick paths remains only as a defensive fallback for unusual entry routes.
void Net_LmsResetLevel(void)
{
    Bmemset(g_lmsLives, 0, sizeof(g_lmsLives));
    s_lmsInit = 0;
    s_lmsCooldown = 0;

    if (!(g_gametypeFlags[ud.coop] & GAMETYPE_LMS))
    {
        if (g_netForensics)
            LOG_F(INFO, "[lms] level reset: inactive (mode=%d)", ud.coop);
        return;
    }

    for (int i = 0; i < MAXPLAYERS; i++)
        g_lmsLives[i] = LMS_LIVES;
    s_lmsInit = 1;
    s_lmsCooldown = 260;   // ~8s grace so nobody "wins" before spawns settle
    if (g_netForensics)
        LOG_F(INFO, "[lms] level reset: lives=%d cooldown=%d", LMS_LIVES, (int)s_lmsCooldown);
}

#ifdef NETNATIVE
static int32_t Net_LmsTestRoundFromEnv(void)
{
    char const *value = getenv("NN_TESTLMSROUND");
    return (value && *value) ? Batoi(value) : 0;
}
static int32_t s_lmsTestRound = Net_LmsTestRoundFromEnv();
#endif

static void Net_LmsInit(void)
{
    // Defensive fallback only. Normal matches were initialized explicitly by
    // Net_LmsResetLevel at G_EnterLevel.
    for (int i = 0; i < MAXPLAYERS; i++)
        g_lmsLives[i] = LMS_LIVES;
    s_lmsInit = 1;
    s_lmsCooldown = 260;
    if (g_netForensics)
        LOG_F(INFO, "[lms] lazy fallback init: lives=%d", LMS_LIVES);
}

// Respawn gate (from VM_ResetPlayer). 1 = allow the respawn and spend a life;
// 0 = keep the player eliminated. Host authoritative; guests always pass (they
// mirror the host's streamed dead state).
int Net_LmsAllowRespawn(int playerNum)
{
    if (!s_lmsInit) Net_LmsInit();
    if (myconnectindex != connecthead || (unsigned)playerNum >= MAXPLAYERS)
        return 1;
    if (g_lmsLives[playerNum] <= 0)
    {
        if (g_netForensics)
            LOG_F(INFO, "[lms] eliminated seat=%d", playerNum);
        return 0;
    }
    g_lmsLives[playerNum]--;
    if (g_netForensics)
        LOG_F(INFO, "[lms] respawn seat=%d lives=%d", playerNum, (int)g_lmsLives[playerNum]);
    return 1;
}

// Per-tic host check: a seat is "still in the round" if it has lives left OR is
// currently alive. When <=1 remain, announce the survivor and start a new round
// (refill lives + resurrect everyone). Cooldown-gated so a fresh round doesn't
// instantly re-trigger while respawns are still settling.
void Net_LmsTick(void)
{
    if (myconnectindex != connecthead)
        return;
    if (!s_lmsInit) Net_LmsInit();   // first LMS tic of this match (browser is fresh each cycle)
    if (s_lmsCooldown > 0) { s_lmsCooldown--; return; }
#ifdef NETNATIVE
    // Focused native gate: once per process, after the normal opening cooldown,
    // make seat 1 eliminated so the ordinary authoritative round-reset path
    // must announce, resurrect, and telegram the owning guest. Default-off and
    // RNG-free; production play never enters this block.
    if (s_lmsTestRound > 0 && s_lmsTestRound < MAXPLAYERS && movefifoplc >= 300
        && g_player[s_lmsTestRound].connected && g_player[s_lmsTestRound].ps != NULL)
    {
        auto const p = g_player[s_lmsTestRound].ps;
        g_lmsLives[s_lmsTestRound] = 0;
        p->dead_flag = 1;
        if ((unsigned)p->i < MAXSPRITES)
            sprite[p->i].extra = 0;
        LOG_F(INFO, "[lms-test] eliminated seat=%d at tic=%d", s_lmsTestRound, movefifoplc);
        s_lmsTestRound = 0;
    }
#endif
    int inRound = 0, last = -1, seats = 0;
    for (int i = 0; i < MAXPLAYERS; i++)
    {
        auto const p = g_player[i].ps;
        if (!g_player[i].connected || p == NULL)
            continue;
        seats++;
        int const alive = ((unsigned)p->i < MAXSPRITES && sprite[p->i].extra > 0 && !p->dead_flag);
        if (g_lmsLives[i] > 0 || alive) { inRound++; last = i; }
    }
    if (seats >= 2 && inRound <= 1)
    {
        if (g_netForensics)
            LOG_F(INFO, "[lms] round win seat=%d seats=%d", last, seats);
        if (last >= 0 && g_player[last].ps != NULL)
        {
            Bsnprintf(apStrings[QUOTE_RESERVED], MAXQUOTELEN, "%s WINS THE ROUND", g_player[last].user_name);
            P_DoQuote(QUOTE_RESERVED, g_player[last].ps);
        }
        for (int i = 0; i < MAXPLAYERS; i++)
        {
            auto const p = g_player[i].ps;
            if (!g_player[i].connected || p == NULL)
                continue;
            g_lmsLives[i] = LMS_LIVES;
            if ((unsigned)p->i < MAXSPRITES && (sprite[p->i].extra <= 0 || p->dead_flag))
                P_ResetMultiPlayer(i);
        }
        // One explicit authoritative round command reaches every guest,
        // including the owning guest. It carries all life counters and causes
        // each receiver to resurrect its dead copies without consulting the
        // life-spending gate. The generic seat-spawn telegram remains for
        // ordinary respawns; LMS reset has its own idempotent lifecycle event.
        uint8_t pk[2 + NET_TEAM_VECTOR_SIZE] = { PACKET_TYPE_LMS_ROUND_RESET,
                                                 (uint8_t)(last + 1) };
        for (int i = 0; i < NET_TEAM_VECTOR_SIZE; i++)
            pk[2 + i] = (uint8_t)max<int>(g_lmsLives[i], 0);
        int dest;
        TRAVERSE_CONNECT(dest)
            if (dest != myconnectindex && !(g_netBotMask & (1 << dest)))
                oldnet_sendpacket(dest, pk, sizeof(pk));
        if (g_netForensics)
            LOG_F(INFO, "[lms] round reset broadcast winner=%d", last);
        s_lmsCooldown = 260;
    }
}

static void Net_ResetProtocolState(void)
{
    g_netStagedInput = {};
    s_ackOfMyInput   = 0;
    s_peerDownMask   = 0;
    g_netStallSince = g_netStallMask = 0;
    s_sessionStartClock = (int32_t)totalclock;
    g_netSampleHead = 0;
    Bmemset(g_netSendRing, 0, sizeof(g_netSendRing));
    g_netJoinCatchup = 0;
    g_netDesyncReporters = 0;
    g_netEolFromHost = 0;
    s_eolPending     = 0;
    s_pendingSpawnMask = 0;
    s_eolWaitMask    = 0;
    s_eolGraceClock  = 0;
    s_hostEolWait    = 0;
    s_hostEolClock   = 0;
    s_joinAwaitReal  = 0;
    s_fillActive     = 0;
    s_joinFlowSlot   = -1;
    s_joinFlowClock  = 0;
    s_joinFlowTries  = 0;
    s_joinFlowIsHeal = 0;
    s_healBasePlc    = -1;
    s_healAckFence   = 0;
    for (int i = 0; i < MAXPLAYERS; i++)
        s_softStrikes[i] = s_softStrikeClock[i] = 0;
    s_pendingSnapLen = 0;
    s_pendingSnapTic = -1;
    for (int i = 0; i < MAXPLAYERS; i++)
    {
        s_slaveAck[i] = s_lastSendClock[i] = 0;
        s_goneTic[i]  = -1;
        s_goneAnnounceUntil[i] = 0;
        s_joinTic[i]  = -1;
        s_joinAnnounceUntil[i] = 0;
        s_stallSince[i] = 0;
        s_lastRealRecvClock[i] = 0;
    }
}

// Absolute-position input codec: same per-field flag layout as
// Net_AddPlayerInputToPacket, but against an EXPLICIT base and with no side
// effects, so records chain inside one self-contained packet.
static int Net_WriteInputDelta(char *pbuf, int j, input_t const *base, input_t const *cur)
{
    int const inputFlagsPos = j++;
    int const extFlagsPos   = j++;
    pbuf[inputFlagsPos] = 0;
    pbuf[extFlagsPos]   = 0;

    if (cur->fvel != base->fvel)       { B_BUF16(&pbuf[j], cur->fvel);    j += 2; pbuf[inputFlagsPos] |= 1; }
    if (cur->svel != base->svel)       { B_BUF16(&pbuf[j], cur->svel);    j += 2; pbuf[inputFlagsPos] |= 2; }
    if (cur->q16avel != base->q16avel) { B_BUF32(&pbuf[j], cur->q16avel); j += 4; pbuf[inputFlagsPos] |= 4; }
    if (cur->q16horz != base->q16horz) { B_BUF32(&pbuf[j], cur->q16horz); j += 4; pbuf[inputFlagsPos] |= 8; }

    uint32_t const bx = cur->bits ^ base->bits;
    if (bx & 0x000000ffu) pbuf[j++] = (char)(cur->bits & 255),         pbuf[inputFlagsPos] |= 16;
    if (bx & 0x0000ff00u) pbuf[j++] = (char)((cur->bits >> 8) & 255),  pbuf[inputFlagsPos] |= 32;
    if (bx & 0x00ff0000u) pbuf[j++] = (char)((cur->bits >> 16) & 255), pbuf[inputFlagsPos] |= 64;
    if (bx & 0xff000000u) pbuf[j++] = (char)((cur->bits >> 24) & 255), pbuf[inputFlagsPos] |= 128;

    uint32_t const ex = cur->extbits ^ base->extbits;
    if (ex & 0x000000ffu) pbuf[j++] = (char)(cur->extbits & 255),         pbuf[extFlagsPos] |= 1;
    if (ex & 0x0000ff00u) pbuf[j++] = (char)((cur->extbits >> 8) & 255),  pbuf[extFlagsPos] |= 2;
    if (ex & 0x00ff0000u) pbuf[j++] = (char)((cur->extbits >> 16) & 255), pbuf[extFlagsPos] |= 4;
    if (ex & 0xff000000u) pbuf[j++] = (char)((cur->extbits >> 24) & 255), pbuf[extFlagsPos] |= 8;

    return j;
}

static int Net_ReadInputDelta(char const *pbuf, int j, input_t const *base, input_t *out)
{
    char const inputFlags = pbuf[j++];
    char const extFlags   = pbuf[j++];

    *out = *base;

    if (inputFlags & 1) { out->fvel    = (int16_t)B_UNBUF16(&pbuf[j]); j += 2; }
    if (inputFlags & 2) { out->svel    = (int16_t)B_UNBUF16(&pbuf[j]); j += 2; }
    if (inputFlags & 4) { out->q16avel = (fix16_t)B_UNBUF32(&pbuf[j]); j += 4; }
    if (inputFlags & 8) { out->q16horz = (fix16_t)B_UNBUF32(&pbuf[j]); j += 4; }

    if (inputFlags & 16)  out->bits = (out->bits & 0xffffff00u) | (uint8_t)pbuf[j++];
    if (inputFlags & 32)  out->bits = (out->bits & 0xffff00ffu) | ((uint32_t)(uint8_t)pbuf[j++] << 8);
    if (inputFlags & 64)  out->bits = (out->bits & 0xff00ffffu) | ((uint32_t)(uint8_t)pbuf[j++] << 16);
    if (inputFlags & 128) out->bits = (out->bits & 0x00ffffffu) | ((uint32_t)(uint8_t)pbuf[j++] << 24);

    if (extFlags & 1) out->extbits = (out->extbits & 0xffffff00u) | (uint8_t)pbuf[j++];
    if (extFlags & 2) out->extbits = (out->extbits & 0xffff00ffu) | ((uint32_t)(uint8_t)pbuf[j++] << 8);
    if (extFlags & 4) out->extbits = (out->extbits & 0xff00ffffu) | ((uint32_t)(uint8_t)pbuf[j++] << 16);
    if (extFlags & 8) out->extbits = (out->extbits & 0x00ffffffu) | ((uint32_t)(uint8_t)pbuf[j++] << 24);

    return j;
}

// Master: schedule an involuntary drop. The boundary is the first un-aggregated
// tic; every already-broadcast tic before it includes the player, so every peer
// can excise at exactly this tic.
static void Net_ScheduleDrop(int i, const char *why)
{
    if (s_goneTic[i] >= 0)
        return;
    s_goneTic[i] = movefifosendplc;
    s_goneAnnounceUntil[i] = (int32_t)totalclock + 600;
    initprintf("net: dropping player %d (%s) at tic %d\n", i, why, movefifosendplc);
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[eng] scheduleDrop p=' + $0 + ' tic=' + $1 + ' why=' + UTF8ToString($2)); },
           i, movefifosendplc, why);
#endif
}

// Receiver: the master flagged this player gone from `tic` on. Rides every M2S
// until the master stops announcing; idempotent.
static void Net_ScheduleDropFromWire(int slot, int32_t tic)
{
    if ((unsigned)slot >= MAXPLAYERS || s_goneTic[slot] >= 0)
        return;
    s_goneTic[slot] = tic;
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[eng] dropFromWire p=' + $0 + ' tic=' + $1); }, slot, tic);
#endif
}

// A join boundary landed (stamped locally on the master, or read from the M2S
// directory on every other peer): player `slot` exists from `tic` on. Fresh
// occupancy of the slot clears any stale drop boundary and primes the record
// cursor at the join base so the M2S apply raises it from there. Idempotent.
static void Net_ScheduleJoin(int slot, int32_t tic)
{
    if ((unsigned)slot >= MAXPLAYERS || g_player[slot].connected || s_joinTic[slot] == tic)
        return;
    s_joinTic[slot] = tic;
    s_goneTic[slot] = -1;
    s_stallSince[slot] = 0;
    s_lastRealRecvClock[slot] = (int32_t)totalclock;
    g_player[slot].movefifoend = tic;
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[eng] joinScheduled p=' + $0 + ' tic=' + $1 + ' plc=' + $2); }, slot, tic, movefifoplc);
#else
    initprintf("net: join boundary for slot %d at tic %d\n", slot, tic);
#endif
}

// Master health monitor: fold transport peer-downs and real-progress silence
// into deterministic drops. With deadline-fill running, movefifoend advances on
// its own for a stalled peer, so the old stall/zombie-rate axes (keyed on that
// cursor) would never fire again -- the only honest death signal left is "the
// wire has delivered NOTHING REAL from this peer for a long time". The old
// warm-up special cases are gone with the barrier: a loading joiner is not in
// the chain at all (nobody waits on it), and by the time it seats it has
// already proven a live catchup stream.
static void Net_CheckPeerHealth(void)
{
    if (myconnectindex != connecthead || g_player[myconnectindex].ps == NULL
        || !(g_player[myconnectindex].ps->gm & MODE_GAME))
        return;

    int32_t const now = (int32_t)totalclock;
    if (s_sessionStartClock > now)   // totalclock reset (level transition)
        s_sessionStartClock = now;
    int i;
    TRAVERSE_CONNECT(i)
    {
        if (i == myconnectindex || s_goneTic[i] >= 0)
            continue;

        if (s_peerDownMask & (1 << i)) { Net_ScheduleDrop(i, "connection lost"); continue; }

        // A guest mid-HEAL is chain-connected but deliberately silent: it is
        // reloading the snapshot and its catchup S2Ms carry only acks (no real
        // records), so the silence axe would kick it in the middle of the cure.
        // The flow has its own failure handling (retry -> kick); keep its
        // liveness clock fresh so the axe restarts cleanly after the resume.
        if (i == s_joinFlowSlot) { s_lastRealRecvClock[i] = now; continue; }
        // CPU seats have no transport to be silent on.
        if (g_netBotMask & (1 << i)) { s_lastRealRecvClock[i] = now; continue; }
        // Level-transition grace: this seat is loading the next map (armed at
        // the EOL broadcast, released by its first real post-transition record
        // below). A slow machine's map/art load runs way past the 10s axe, and
        // it is provably mute the whole time. Transport peer-downs (above)
        // still reap genuine disconnects instantly.
        if (s_eolWaitMask & (1 << i))
        {
            if (s_eolGraceClock > now)   // totalclock reset at OUR level entry
                s_eolGraceClock = now;
            if (now - s_eolGraceClock < NET_EOL_GRACE) { s_lastRealRecvClock[i] = now; continue; }
            LOG_F(WARNING, "[eol] seat %d never resumed after the transition (%ds) -- releasing the axe",
                  i, NET_EOL_GRACE / 120);
            s_eolWaitMask &= ~(1 << i);
        }
        // A guest still inside the NEW_GAME redelivery window is BOOTING its
        // engine -- a real browser can take minutes, and the axe was measured
        // kicking a throttled guest 48s after launch while its entry packets
        // sat queued in the reliable channel (the user's every "empty match").
        // Transport peer-downs (above) still reap genuine disconnects
        // instantly; the axe just waits for entry.
        if (!(s_newGameAckMask & (1u << i)) && s_newGameLen > 0
            && now - s_newGameClock < 7200)   // up to ~4 min of boot patience
        { s_lastRealRecvClock[i] = now; continue; }

        if (s_lastRealRecvClock[i] > now)   // totalclock reset
            s_lastRealRecvClock[i] = now;
        // A peer that has never delivered this session is presumed loading:
        // long grace from the barrier. One that was flowing and went quiet is
        // dead or wedged: short axe from its last real record. (Transport
        // peer-downs reap true disconnects far sooner in both cases.)
        bool const neverDelivered = (s_lastRealRecvClock[i] == 0);
        int32_t const lastAlive = neverDelivered ? s_sessionStartClock : s_lastRealRecvClock[i];
        int32_t const limit     = neverDelivered ? NET_FIRST_GRACE : NET_STALL_DROP;
        if (now - lastAlive > limit)
        {
#ifdef __EMSCRIPTEN__
            EM_ASM({ console.log('[eng] silencedrop p=' + $0 + ' now=' + $1 + ' lastReal=' + $2 + ' end=' + $3); },
                   i, now, s_lastRealRecvClock[i], g_player[i].movefifoend);
#endif
            Net_ScheduleDrop(i, "timed out"); continue;
        }
    }
    // Preserve peer-down bits for stamped-but-unseated joiners: they are not in
    // the chain yet, so the fold above could not see them. Once the seat tic
    // passes and they join the chain, the kept bit folds into their drop.
    {
        int32_t keep = 0;
        for (int k = 0; k < MAXPLAYERS; k++)
            if (s_joinTic[k] >= 0 && !g_player[k].connected)
                keep |= s_peerDownMask & (1 << k);
        s_peerDownMask = keep;
    }
}

// Build one tailored MASTER_TO_SLAVE packet for `dest`: everything from that
// slave's ack (capped) up to the aggregation cursor, plus the roster directory
// with drop boundaries and the pre-captured sync block. Length returned.
static int Net_BuildMasterPacket(int dest, int32_t start, char const *syncBlk, int syncLen)
{
    int32_t const end = movefifosendplc;
    packbuf[0] = PACKET_TYPE_MASTER_TO_SLAVE;
    packbuf[1] = (char)g_netMoveEpoch;
    int j = 2;
    B_BUF32(&packbuf[j], start); j += 4;
    int const countPos  = j++;
    int const pcountPos = j++;
    B_BUF32(&packbuf[j], g_player[dest].movefifoend); j += 4;  // ack of dest's own inputs

    // Roster directory: the live chain, recently-dropped players (their pre-drop
    // records still decode in repair windows; the drop boundary is redundant
    // across packets), and announced late joiners (their JOIN boundary must
    // reach every peer before anyone consumes the seat tic -- it rides every
    // packet until then).
    int listed[MAXPLAYERS], nlisted = 0;
    int i;
    TRAVERSE_CONNECT(i)
        listed[nlisted++] = i;
    for (i = 0; i < MAXPLAYERS; i++)
        if (!g_player[i].connected
            && ((s_goneTic[i] >= 0 && (int32_t)totalclock < s_goneAnnounceUntil[i])
                || (s_joinTic[i] >= 0 && (int32_t)totalclock < s_joinAnnounceUntil[i])))
            listed[nlisted++] = i;

    input_t const *bases[MAXPLAYERS];
    for (int k = 0; k < nlisted; k++)
    {
        int const p = listed[k];
        bool const announceJoin = (s_joinTic[p] >= 0 && (int32_t)totalclock < s_joinAnnounceUntil[p]);
        packbuf[j++] = (char)(p | (s_goneTic[p] >= 0 ? 0x80 : 0) | (announceJoin ? 0x40 : 0));
        packbuf[j++] = (char)min(max(g_player[p].myminlag, -128), 127);
        if (s_goneTic[p] >= 0) { B_BUF32(&packbuf[j], s_goneTic[p]); j += 4; }
        if (announceJoin)      { B_BUF32(&packbuf[j], s_joinTic[p]); j += 4; }
        bases[p] = &s_zeroInput;
    }

    int32_t built = 0;
    for (int32_t t = start; t < end && built < 255; t++)
    {
        int const savej = j;
        input_t const *savedBases[MAXPLAYERS];
        for (int k = 0; k < nlisted; k++) savedBases[listed[k]] = bases[listed[k]];

        for (int k = 0; k < nlisted; k++)
        {
            int const p = listed[k];
            if (s_goneTic[p] >= 0 && t >= s_goneTic[p])
                continue;   // gone from this tic on: no record
            if (s_joinTic[p] >= 0 && t < s_joinTic[p])
                continue;   // not in the match until the join boundary: no record
            input_t const *cur = &inputfifo[t & (MOVEFIFOSIZ - 1)][p];
            j = Net_WriteInputDelta(packbuf, j, bases[p], cur);
            bases[p] = cur;
        }
        if (j > NET_BYTE_BUDGET)
        {   // revert the partial tic; the remainder rides the next packet
            j = savej;
            for (int k = 0; k < nlisted; k++) bases[listed[k]] = savedBases[listed[k]];
            break;
        }
        built++;
    }
    packbuf[countPos]  = (char)built;
    packbuf[pcountPos] = (char)nlisted;

    Bmemcpy(&packbuf[j], syncBlk, syncLen);
    j += syncLen;
    return j;
}

#endif  // transport track

int32_t g_netPumpCalls = 0; // debug surface: proves the input pump runs
int32_t g_netGateC1 = 0, g_netGateC2 = 0; // main-loop gate probes (game.cpp)
int32_t g_mainLoopIter = 0;  // app_main do-loop residency (game.cpp)
int32_t g_demoLoopIter = 0;  // G_PlaybackDemo per-frame residency (demo.cpp)

void Net_HandleInput(void)
{
    g_netPumpCalls++;
    int i, j;
    //    short who;
    input_t *osyn, *nsyn;

    if (quickExit == 0 && KB_KeyPressed(sc_LeftControl) && KB_KeyPressed(sc_LeftAlt) && KB_KeyPressed(sc_Delete))
    {
        quickExit = 1;
        G_GameExit("Quick Exit.");
    }

    Net_GetPackets();

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
    (void)osyn; (void)nsyn;  // used only by the stock one-tic protocol in the #else branch
    // ── Tic-indexed protocol (see the wire-format comment above) ─────────────
    // A capped sampler must STILL send/aggregate: acks and repair windows ride
    // every packet, and stopping them deadlocks a peer that is waiting on a
    // resend (the stock early-return here starved exactly that).
    //
    // CANONICAL STREAM: the master samples straight into its own fifo column --
    // its samples ARE the timeline. A slave stages samples in g_netSendRing for
    // the wire and the predictor, and consumes its own column from the master's
    // echo exactly like every other player's; that way master-side synthesis
    // (deadline-fill, joiner zero-fill) reaches every sim identically and lag
    // can never fork anyone. A joiner mid-catchup samples nothing at all.
    {
        // NN_LOCALBOT=1: this seat plays itself through the full human input
        // pipeline (native twin of Web_SetLocalBot) -- live public test
        // matches need seats that move and fight, not statues.
        static int s_lbInit;
        if (!s_lbInit)
        {
            s_lbInit = 1;
            const char *e = getenv("NN_LOCALBOT");
            if (e && Batoi(e))
                Net_SetLocalBot(1);
        }
    }
    bool const isSlave = (numplayers > 1 && myconnectindex != connecthead);
    bool const seated  = g_player[myconnectindex].connected && !g_netJoinCatchup;
    int32_t const localHead = isSlave ? g_netSampleHead : g_player[myconnectindex].movefifoend;
    bool const capped = (localHead - movefifoplc >= 100);

    if (isSlave && !seated)
    {
        // CATCHUP ACK (unseated joiner / healing guest): the join and heal
        // flows pace on s_slaveAck -- and the only writer used to be the
        // seated-gated SLAVE_TO_MASTER, so these peers were silent, the host
        // saw ack==base forever, never announced the seat, and re-streamed
        // snapshots in a loop (live-reported 3rd DM seat, 2026-08-16).
        // RELIABLE by design: it rides the control channel (S2M is the lossy
        // move channel, and a joiner's early move-channel packets can vanish
        // while the SCTP stream is young); low-rate, tiny, and the flow
        // deadlocks without it -- reliability is correctness here.
        packbuf[0] = PACKET_TYPE_JOIN_ACK;
        packbuf[1] = (char)g_netMoveEpoch;
        j = 2;
        B_BUF32(&packbuf[j], movefifosendplc); j += 4;
        oldnet_sendpacket(connecthead, (unsigned char *)packbuf, j);
        {
            static int32_t s_caLogged;
            if (s_caLogged++ == 0)
                LOG_F(INFO, "[join] first catchup ack sent: epoch=%d ack=%d",
                      (int)(uint8_t)packbuf[1], movefifosendplc);
        }
        return;
    }

    if (!capped && seated)
    {
        if (isSlave)
        {
            input_t staged = netInput;
            // CLOSED-LOOP AIM -- the shot truth comes from the PLAYER THAT
            // FIRES (live-directed, twice). The wire carries direction DELTAS
            // and open-loop accumulation drifts from the crosshair (staging
            // clamps, sim centering): the host renders its own sim so its aim
            // was always exact, while a guest's sim shot wherever the drift
            // pointed. Stage, instead, exactly the delta that lands the sim
            // ON the view's current direction: anchor at our own sim (the
            // post-consume truth), replay the still-in-flight staged deltas,
            // send the remaining gap. Truncation self-heals -- the next tic
            // re-measures against a fresh anchor -- and every peer consumes
            // identical bytes, so determinism is untouched. The local-bot
            // test mode is exempt: its raw inputs ARE its intent (no view).
            if (!g_netLocalBot && g_player[myconnectindex].ps != NULL
                && (g_player[myconnectindex].ps->gm & MODE_GAME))
            {
                fix16_t wa = g_player[myconnectindex].ps->q16ang;
                fix16_t wh = g_player[myconnectindex].ps->q16horiz;
                int32_t const from = max(movefifoplc, g_netSampleHead - 64);
                for (int32_t tt = from; tt < g_netSampleHead; tt++)
                {
                    input_t const &r = g_netSendRing[tt & (MOVEFIFOSIZ - 1)];
                    wa = fix16_sadd(wa, r.q16avel);
                    wh = fix16_clamp(fix16_sadd(wh, r.q16horz), F16(HORIZ_MIN), F16(HORIZ_MAX));
                }
                while (wa < 0)          wa = fix16_sadd(wa, F16(2048));
                while (wa >= F16(2048)) wa = fix16_ssub(wa, F16(2048));
                fix16_t da = fix16_ssub(predictedPlayer.q16ang, wa);
                while (da > F16(1024))  da = fix16_ssub(da, F16(2048));
                while (da < -F16(1024)) da = fix16_sadd(da, F16(2048));
                // LOCALBOT seats keep the brain's OWN turn command: the
                // closed loop tracks the predicted VIEW, which only a mouse
                // moves -- it silently discarded every bot avel, so a guest
                // localbot could never turn (probe cameras walked into the
                // first wall and pushed forever; measured in frame bursts).
                extern int32_t g_netLocalBot;
                if (!g_netLocalBot)
                {
                    staged.q16avel = fix16_clamp(da, -F16(512), F16(512));
                    staged.q16horz = fix16_clamp(fix16_ssub(predictedPlayer.q16horiz, wh),
                                                 -F16(127), F16(127));
                }
            }
            {
                extern int32_t g_testFire, g_testDrive;   // hit-registration harness
                if (g_testFire)
                    staged.bits |= BIT(SK_FIRE);
                if (g_testDrive)
                {
                    // WORLD-SPACE CONTRACT: fvel/svel are world velocity
                    // components (P_ProcessInput adds them straight to
                    // vel.x/y). Rotate "forward 96" by the current facing or
                    // the probe walks due EAST forever regardless of view --
                    // the exact bug that froze the bots for two sessions.
                    auto const dps = g_player[myconnectindex].ps;
                    int const da = (dps != NULL) ? (fix16_to_int(dps->q16ang) & 2047) : 0;
                    staged.fvel = (int16_t)clamp((96 * sintable[(da + 512) & 2047]) >> 14, -127, 127);
                    staged.svel = (int16_t)clamp((96 * sintable[da & 2047]) >> 14, -127, 127);
                    staged.bits |= BIT(SK_RUN);
                    staged.extbits |= BIT(EK_MOVE_FORWARD);
                }
            }
            g_netSendRing[g_netSampleHead & (MOVEFIFOSIZ - 1)] = staged;
            g_netSampleHead++;
            // STREAM FREE-RUN: the guest's OWN column fills at SAMPLE time, so
            // its sim never waits for the host to echo its own inputs back.
            // The echo round-trip was the guest's whole-world input latency
            // (fire/doors/projectiles all ran ~RTT behind the mouse -- live at
            // 80ms: "a lot of input lag on the guest"). In stream mode the
            // canonical-timeline argument for echo-consumption is void: the
            // sim is a client-first predictor, world truth is repainted by the
            // host stream, POS_REPORT owns this seat on the host, and every
            // sync/divergence verdict is already stream-gated off. The echo's
            // copy of this column dup-drops on arrival (t < movefifoend).
            if (g_netStreamMode)
            {
                inputfifo[(g_netSampleHead - 1) & (MOVEFIFOSIZ - 1)][myconnectindex] = staged;
                if (g_player[myconnectindex].movefifoend < g_netSampleHead)
                    g_player[myconnectindex].movefifoend = g_netSampleHead;
            }
        }
        else
        {
            inputfifo[g_player[myconnectindex].movefifoend & (MOVEFIFOSIZ - 1)][myconnectindex] = netInput;
            g_player[myconnectindex].movefifoend++;
        }
    }

    if (numplayers < 2)
        return;

    int32_t const localNow = (isSlave ? g_netSampleHead : g_player[myconnectindex].movefifoend) - 1;
    TRAVERSE_CONNECT(i)
    if (i != myconnectindex)
    {
        int32_t const lag = localNow - g_player[i].movefifoend;
        g_player[i].myminlag = min(g_player[i].myminlag, lag);
        mymaxlag = max(mymaxlag, lag);
    }

    if (!capped && seated && (localNow & (TIMERUPDATESIZ - 1)) == 0)
    {
        i = mymaxlag - bufferjitter;
        mymaxlag = 0;
        if (i > 0)
            bufferjitter += ((2 + i) >> 2);
        else if (i < 0)
            bufferjitter -= ((2 - i) >> 2);
    }

    if (myconnectindex != connecthead)   // ── SLAVE ──
    {
        // Divergence visibility is lag-asymmetric: this guest's compares can
        // flag a split the host's never will. Report it (reliable, throttled);
        // the host latches g_foundSyncError and streams a healing snapshot.
        // WATCHERS (unseated joiners / healing guests mid-catchup) never
        // report: their compares are gated off, and any stale latch from
        // before the catchup must not re-trigger the heal that just ran.
        if (!g_netStreamMode && seated && (g_foundSyncError || Net_SyncErrorDetected()))
        {
            static int32_t s_lastReportClock;
            int32_t const now = (int32_t)totalclock;
            if (s_lastReportClock > now)
                s_lastReportClock = 0;
            if (now - s_lastReportClock > 300)   // ~2.5s
            {
                s_lastReportClock = now;
                uint8_t rpt[1] = { PACKET_TYPE_DESYNC_REPORT };
                oldnet_sendpacket(connecthead, rpt, 1);
#ifdef __EMSCRIPTEN__
                EM_ASM({ console.log('[eng] guest desync -> reporting to host'); });
#endif
            }
        }

        // Host silence watchdog: a host that stops sending entirely (crash,
        // pulled cable AND dead transport events) ends the match gracefully.
        {
            int32_t const now = (int32_t)totalclock;
            if (lastpackettime > now)   // totalclock reset (level transition)
                lastpackettime = now;
            // Transition grace: the host is loading the next map and cannot
            // send; keep the axe's base fresh until its stream resumes (the
            // M2S handler releases the wait) or the grace runs out.
            if (s_hostEolWait)
            {
                if (s_hostEolClock > now)   // totalclock reset at OUR level entry
                    s_hostEolClock = now;
                if (now - s_hostEolClock < NET_EOL_GRACE)
                    lastpackettime = now;
                else
                {
                    LOG_F(WARNING, "[eol] host never resumed after the transition (%ds) -- releasing the axe",
                          NET_EOL_GRACE / 120);
                    s_hostEolWait = 0;
                }
            }
            if (g_player[myconnectindex].ps != NULL
                && (g_player[myconnectindex].ps->gm & MODE_GAME)
                && now - lastpackettime > NET_HOST_SILENT)
            {
                Net_SetLocalBot(0);
                g_netHostGone = 1;
            }
        }

        //Fix timers and buffer/jitter value
        if (!capped && seated && ((g_netSampleHead-1)&(TIMERUPDATESIZ-1)) == 0)
        {
            // [JM] I wish this wasn't so fucking cryptic.
            i = g_player[connecthead].myminlag - otherminlag;
            if (klabs(i) > 2)
            {
                if (klabs(i) > 8)
                {
                    if (i < 0)
                        i++;
                    i >>= 1;
                }
                else
                {
                    if (i < 0)
                        i = -1;
                    if (i > 0)
                        i = 1;
                }
                totalclock -= TICSPERFRAME * i;
                otherminlag += i;
            }

            TRAVERSE_CONNECT(i)
            {
                g_player[i].myminlag = 0x7fffffff;
            }
        }

        // Window: everything the master has not acked, newest tic always
        // included. NEVER clamp the start past the ack: a receiver behind by
        // more than any fixed cap could then never repair (soak-caught as an
        // endless g_netGapDrops wedge). The byte budget below bounds each
        // packet; successive packets converge any backlog, and the sampler's
        // +100 runahead cap keeps every reachable tic inside the 256-tic ring.
        // Records come from g_netSendRing (locally sampled); an unseated joiner
        // has none, and its EMPTY window is a pure ACK carrier -- the ack paces
        // the master's catchup stream and triggers the seat stamp.
        int32_t const myEnd = g_netSampleHead;
        int32_t start = s_ackOfMyInput;
        if (start < 0) start = 0;
        if (start > myEnd) start = myEnd;
        if (start == myEnd && myEnd > 0) start = myEnd - 1;

#if defined(NETNATIVE) && !defined(__EMSCRIPTEN__)
        // Headless verification driver (env NN_TESTKILL; inert otherwise). Once
        // in-game: (1) switch to KNEE so the weapon-state wire shows a [gw] apply
        // on the host, (2) report a lethal hit on the nearest live enemy in OUR
        // world -- exercising report -> host resolve/wake -> host death CON ->
        // stream -> guest replay-kill end-to-end, minus only the literal hitscan.
        {
            static int s_tkPhase = -1;
            static int s_tkTarget = -1;
            if (s_tkPhase < 0)
                s_tkPhase = (getenv("NN_TESTKILL") != NULL) ? 0 : 9;
            auto const tkps = g_player[myconnectindex].ps;
            if (s_tkPhase < 9 && tkps != NULL && (tkps->gm & MODE_GAME))
            {
                if (s_tkPhase == 0 && movefifoplc > 120)
                {
                    tkps->curr_weapon = KNEE_WEAPON;
                    LOG_F(INFO, "[testkill] switched to KNEE (expect [gw] on host)");
                    s_tkPhase = 1;
                }
                else if (s_tkPhase == 1 && movefifoplc > 240)
                {
                    int best = -1; int64_t bestd = INT64_MAX;
                    for (int i = 0; i < MAXSPRITES; i++)
                    {
                        if (sprite[i].statnum >= MAXSTATUS || !A_CheckEnemySprite(&sprite[i]) || sprite[i].extra <= 0)
                            continue;
                        int64_t const dx = sprite[i].x - tkps->pos.x, dy = sprite[i].y - tkps->pos.y;
                        int64_t const d = dx * dx + dy * dy;
                        if (d < bestd) { bestd = d; best = i; }
                    }
                    if (best >= 0)
                    {
                        LOG_F(INFO, "[testkill] lethal report on idx=%d pic=%d stat=%d extra=%d",
                              best, (int)sprite[best].picnum, (int)sprite[best].statnum, (int)sprite[best].extra);
                        Net_ClientReportEnemyHit(best, sprite[best].x, sprite[best].y, sprite[best].z, 300, SHOTSPARK1);
                        s_tkTarget = best;
                        s_tkPhase = 2;
                    }
                }
                else if (s_tkPhase == 2 && movefifoplc > 420 && s_tkTarget >= 0)
                {
                    // Post-kill probe: htextra==-1 proves the local CON CONSUMED the
                    // replayed hit (death branch executed); statnum>=MAXSTATUS means
                    // the host gibbed+deleted it. Either is a completed death.
                    LOG_F(INFO, "[testkill] post idx=%d stat=%d extra=%d htextra=%d",
                          s_tkTarget, (int)sprite[s_tkTarget].statnum, (int)sprite[s_tkTarget].extra,
                          (int)actor[s_tkTarget].htextra);
                    s_tkPhase = 3;
                }
            }
        }
        // Headless dead-shooter driver (env NN_TESTDEADSHOOTER; inert otherwise).
        // Phase A -- CLAMP: arm a big LOCAL-ONLY hit (no report) on the nearest
        // live enemy; the local CON consumes it, A_IncurDamage must clamp at 1hp
        // ([lethal] line) and the monster must stay alive. Phase B -- REVIVE:
        // slam the same slot straight into a corpse (extra/cstat, the path the
        // clamp can't cover); the host keeps streaming it alive, so
        // Net_CorpseReviveDue must rebuild it (~2.5s). Probes log PASS/FAIL.
        {
            static int s_tdPhase = -1;
            static int s_tdTarget = -1;
            if (s_tdPhase < 0)
                s_tdPhase = (getenv("NN_TESTDEADSHOOTER") != NULL) ? 0 : 9;
            auto const tdps = g_player[myconnectindex].ps;
            if (s_tdPhase < 9 && tdps != NULL && (tdps->gm & MODE_GAME))
            {
                if (s_tdPhase == 0 && movefifoplc > 240)
                {
                    int best = -1; int64_t bestd = INT64_MAX;
                    for (int i = 0; i < MAXSPRITES; i++)
                    {
                        if (sprite[i].statnum >= MAXSTATUS || !A_CheckEnemySprite(&sprite[i]) || sprite[i].extra <= 0)
                            continue;
                        int64_t const dx = sprite[i].x - tdps->pos.x, dy = sprite[i].y - tdps->pos.y;
                        int64_t const d = dx * dx + dy * dy;
                        if (d < bestd) { bestd = d; best = i; }
                    }
                    if (best >= 0)
                    {
                        actor[best].htextra  = 300;          // local-only: never reported
                        actor[best].htpicnum = SHOTSPARK1;
                        if (sprite[best].statnum == STAT_ZOMBIEACTOR)
                            changespritestat((int16_t)best, STAT_ACTOR);   // sleeper CON never runs: wake like the host resolve does
                        s_tdTarget = best;
                        s_tdPhase  = 1;
                        LOG_F(INFO, "[testdead] A: local-only lethal armed on idx=%d pic=%d extra=%d",
                              best, (int)sprite[best].picnum, (int)sprite[best].extra);
                    }
                }
                else if (s_tdPhase == 1 && movefifoplc > 400 && s_tdTarget >= 0)
                {
                    bool const alive = sprite[s_tdTarget].statnum < MAXSTATUS
                                       && sprite[s_tdTarget].extra > 0 && (sprite[s_tdTarget].cstat & 1);
                    LOG_F(INFO, "[testdead] A %s: idx=%d stat=%d extra=%d cstat=%d",
                          alive ? "CLAMP OK" : "CLAMP FAILED", s_tdTarget,
                          (int)sprite[s_tdTarget].statnum, (int)sprite[s_tdTarget].extra,
                          (int)sprite[s_tdTarget].cstat);
                    if (alive)
                    {
                        sprite[s_tdTarget].extra = -1;      // B: corpse-slam past the clamp
                        sprite[s_tdTarget].cstat = 0;
                        LOG_F(INFO, "[testdead] B: slammed idx=%d into a local corpse", s_tdTarget);
                    }
                    s_tdPhase = alive ? 2 : 8;
                }
                else if (s_tdPhase == 2 && movefifoplc > 700 && s_tdTarget >= 0)
                {
                    bool const alive = sprite[s_tdTarget].statnum < MAXSTATUS
                                       && sprite[s_tdTarget].extra > 0 && (sprite[s_tdTarget].cstat & 1);
                    LOG_F(INFO, "[testdead] B %s: idx=%d stat=%d extra=%d cstat=%d",
                          alive ? "REVIVE OK" : "REVIVE FAILED", s_tdTarget,
                          (int)sprite[s_tdTarget].statnum, (int)sprite[s_tdTarget].extra,
                          (int)sprite[s_tdTarget].cstat);
                    s_tdPhase = 8;
                }
            }
        }
#endif
        Net_SendWeaponState();   // guest: tell the host which weapon it's firing (host-auth fire uses it)
        Net_SendPosReport();     // guest: authoritative self position (the host adopts, never corrects)
        Net_SendAccessState();   // guest: newly earned key cards -> the shared coop key ring
        {
            // Test rig (NN_TESTACCESS=1): simulate a guest card pickup so the
            // shared-key-ring round trip proves out headlessly -- guest report
            // up, host union+share, broadcast back down ([access] lines).
            static int s_taDone;
            if (!s_taDone && movefifoplc > 200 && getenv("NN_TESTACCESS") != NULL
                && g_player[myconnectindex].ps != NULL)
            {
                s_taDone = 1;
                g_player[myconnectindex].ps->got_access |= 1;
                LOG_F(INFO, "[testaccess] guest granted itself the blue card");
            }
        }
        {
            // Test rig (NN_TESTEOL=1): the guest's own sim hits the exit.
            // MUST be deferred (P_EndLevel's stream-guest guard) -- the real
            // transition then arrives from the host via PACKET_TYPE_EOL.
            static int s_teDone;
            if (!s_teDone && movefifoplc > 250 && getenv("NN_TESTEOL") != NULL)
            {
                s_teDone = 1;
                LOG_F(INFO, "[testeol] guest firing P_EndLevel");
                P_EndLevel();
            }
        }

        packbuf[0] = PACKET_TYPE_SLAVE_TO_MASTER;
        packbuf[1] = (char)g_netMoveEpoch;
        j = 2;
        B_BUF32(&packbuf[j], start); j += 4;
        int const countPos = j++;
        B_BUF32(&packbuf[j], movefifosendplc); j += 4;  // ack: contiguous master tics applied

        input_t const *base = &s_zeroInput;
        int32_t built = 0;
        for (int32_t t = start; t < myEnd && built < 255; t++)
        {
            input_t const *cur = &g_netSendRing[t & (MOVEFIFOSIZ - 1)];
            int const nj = Net_WriteInputDelta(packbuf, j, base, cur);
            if (nj > NET_BYTE_BUDGET)
                break;
            j = nj;
            base = cur;
            built++;
        }
        packbuf[countPos] = (char)built;

        Net_AddSyncInfoToPacket(&j);
        oldnet_sendpacket(connecthead, (unsigned char *)packbuf, j);
        return;
    }

    // ── MASTER ──
    Net_CheckPeerHealth();

    // Aggregate: advance over tics every still-required peer has provided --
    // synthesizing any column that has been missing past the fill deadline.
    // The user-facing contract: one peer's ping never holds the match. A
    // synthesized tic is canonical the moment it is aggregated; the real input
    // arriving late for it is dup-dropped, and the laggard consumes the
    // synthesized echo like everyone else, so nobody diverges.
    for (;;)
    {
        // CPU seats: the master IS their input source. Synthesize their column
        // for the aggregation tic the moment it is required -- they can never
        // block, never fill, and their inputs are canonical like anyone's.
        for (i = 0; i < MAXPLAYERS; i++)
            if ((g_netBotMask & (1 << i)) && g_player[i].connected
                && g_player[i].movefifoend <= movefifosendplc)
            {
                inputfifo[g_player[i].movefifoend & (MOVEFIFOSIZ - 1)][i] = Bot_GetInput(i);
                g_player[i].movefifoend++;
            }

        // Required columns for the NEXT aggregation tic: the live chain plus
        // announced joiners whose boundary the cursor has reached. The cursor
        // runs AHEAD of the sim, so an announced joiner may not be seated in
        // the chain yet -- but its records from joinTic on are part of the
        // canonical stream and MUST be aggregated (zero-filled until its real
        // inputs land), or those tics would ship with stale ring garbage.
        int32_t reqMask = 0;
        TRAVERSE_CONNECT(i)
            if (s_goneTic[i] < 0 || movefifosendplc < s_goneTic[i])
                reqMask |= (1 << i);
        for (i = 0; i < MAXPLAYERS; i++)
            if (s_joinTic[i] >= 0 && !g_player[i].connected && s_goneTic[i] < 0
                && movefifosendplc >= s_joinTic[i])
                reqMask |= (1 << i);

        int32_t blockMask = 0;
        for (i = 0; i < MAXPLAYERS; i++)
            if ((reqMask & (1 << i)) && g_player[i].movefifoend <= movefifosendplc)
                blockMask |= (1 << i);

        if (blockMask)
        {
            int32_t const now = (int32_t)totalclock;
            int filled = 0;
            for (i = 0; i < MAXPLAYERS; i++)
            {
                if (!(blockMask & (1 << i)))
                    continue;
                if (!s_stallSince[i] || s_stallSince[i] > now)
                    s_stallSince[i] = now;
                if (i == myconnectindex)
                    continue;   // never synthesize the master's own column: it IS the pace source

                bool const joinerNoReal = (s_joinAwaitReal & (1 << i)) != 0;
                if (joinerNoReal || (s_fillActive & (1 << i))
                    || now - s_stallSince[i] > NET_FILL_DEADLINE)
                {
                    int32_t const end = g_player[i].movefifoend;   // == movefifosendplc when blocking
                    input_t fillVal = s_zeroInput;
                    // Hold the last real input (keeps motion continuous over a
                    // spike); a freshly seated joiner has none -> neutral zero.
                    if (!joinerNoReal && end > 0 && (s_joinTic[i] < 0 || end > s_joinTic[i]))
                        fillVal = inputfifo[(end - 1) & (MOVEFIFOSIZ - 1)][i];
                    // One-shot latches must not repeat under synthesis.
                    fillVal.bits &= ~(BIT(SK_GAMEQUIT) | BIT(SK_PAUSE) | BIT(SK_MULTIFLAG));
                    inputfifo[end & (MOVEFIFOSIZ - 1)][i] = fillVal;
                    g_player[i].movefifoend = end + 1;
                    s_fillActive |= (1 << i);
                    g_netFillTics++;
                    filled++;
                }
            }
            if (!filled)
                break;
            continue;   // re-evaluate the gate over the synthesized columns
        }
        for (i = 0; i < MAXPLAYERS; i++)
            if (!(s_fillActive & (1 << i)))
                s_stallSince[i] = 0;

        if ((movefifosendplc & (TIMERUPDATESIZ - 1)) == 0)
            TRAVERSE_CONNECT(i)
                g_player[i].myminlag = 0x7fffffff;

        movefifosendplc++;
    }

    // Capture this pump's sync block ONCE; every tailored packet carries the
    // same bytes (Net_AddSyncInfoToPacket advances the multi-stamp cursor, so
    // calling it per-packet would starve all slaves but the first).
    char syncBlk[16 + 16 * (4 + MAX_SYNC_TYPES)];
    int  syncLen = 0;
    Net_AddSyncInfoToPacket(&syncLen);
    Bmemcpy(syncBlk, packbuf, syncLen);

    auto sendMasterTo = [&](int dest)
    {
        // Window starts at the slave's ack, NEVER clamped forward (see the
        // slave-side comment: a fixed cap wedges any receiver behind by more).
        int32_t start = s_slaveAck[dest];
        if (start < 0) start = 0;

        int32_t const now = (int32_t)totalclock;
        if (s_lastSendClock[dest] > now)
            s_lastSendClock[dest] = 0;
        if (start >= movefifosendplc && now - s_lastSendClock[dest] < NET_KEEPALIVE)
            return;   // nothing new and no keepalive due
        s_lastSendClock[dest] = now;

        int const len = Net_BuildMasterPacket(dest, start, syncBlk, syncLen);
        oldnet_sendpacket(dest, (unsigned char *)packbuf, len);
    };

    TRAVERSE_CONNECT(i)
    {
        if (i != myconnectindex)
            sendMasterTo(i);
    }
    // A joiner mid-catchup is not in the chain yet but consumes this stream --
    // its deep window (from the snapshot tic) converges packet by packet, paced
    // by the acks its empty S2Ms carry.
    if (s_joinFlowSlot >= 0 && !g_player[s_joinFlowSlot].connected)
        sendMasterTo(s_joinFlowSlot);

    // STATE AUTHORITY: after the cmd fan-out, broadcast what is actually TRUE.
    if (g_netStreamMode)
        Net_StreamAuthoritativeState();
    // NEW_GAME redelivery: a guest whose engine was still booting at launch
    // missed the roster packet and entered an empty world (live, twice).
    Net_ResendNewGameIfUnacked();

    // Classic master self-quit: exit once our own quit-bit tic has been
    // aggregated (its broadcast + the drop machinery inform every peer).
    if (g_gameQuit && movefifosendplc > 0
        && TEST_SYNC_KEY(inputfifo[(movefifosendplc - 1) & (MOVEFIFOSIZ - 1)][myconnectindex].bits, SK_GAMEQUIT))
        G_GameExit(" ");

#else  // stock NetDuke32 one-tic protocol (native builds without the seam)

    if (g_player[myconnectindex].movefifoend - movefifoplc >= 100)
        return;

    // Put our local input into the FIFO to be processed by P_ProcessInput and such.
    nsyn = &inputfifo[g_player[myconnectindex].movefifoend&(MOVEFIFOSIZ-1)][myconnectindex];
    *nsyn = netInput;

    g_player[myconnectindex].movefifoend++;

    if (numplayers < 2)
        return;

    TRAVERSE_CONNECT(i)
    if (i != myconnectindex)
    {
        int32_t lag = (g_player[myconnectindex].movefifoend-1)-g_player[i].movefifoend;
        g_player[i].myminlag = min(g_player[i].myminlag, lag);
        mymaxlag = max(mymaxlag, lag);
    }

    if (((g_player[myconnectindex].movefifoend - 1) & (TIMERUPDATESIZ - 1)) == 0)
    {
        i = mymaxlag - bufferjitter;
        mymaxlag = 0;
        if (i > 0)
            bufferjitter += ((2 + i) >> 2);
        else if (i < 0)
            bufferjitter -= ((2 - i) >> 2);
    }

    // Master/Slave
    if (myconnectindex != connecthead)   //Slave
    {
        //Fix timers and buffer/jitter value
        if (((g_player[myconnectindex].movefifoend-1)&(TIMERUPDATESIZ-1)) == 0)
        {
            // [JM] I wish this wasn't so fucking cryptic.
            i = g_player[connecthead].myminlag - otherminlag;
            if (klabs(i) > 2)
            {
                if (klabs(i) > 8)
                {
                    if (i < 0)
                        i++;
                    i >>= 1;
                }
                else
                {
                    if (i < 0)
                        i = -1;
                    if (i > 0)
                        i = 1;
                }
                totalclock -= TICSPERFRAME * i;
                otherminlag += i;
            }

            TRAVERSE_CONNECT(i)
            {
                g_player[i].myminlag = 0x7fffffff;
            }
        }

        packbuf[0] = PACKET_TYPE_SLAVE_TO_MASTER;
        j = 1;

        osyn = (input_t *)&inputfifo[(g_player[myconnectindex].movefifoend-2)&(MOVEFIFOSIZ-1)][myconnectindex];
        nsyn = (input_t *)&inputfifo[(g_player[myconnectindex].movefifoend-1)&(MOVEFIFOSIZ-1)][myconnectindex];

        Net_AddPlayerInputToPacket(&j, 0, osyn, nsyn);
        Net_AddSyncInfoToPacket(&j);

        oldnet_sendpacket(connecthead, (unsigned char*)packbuf,j);
        return;
    }

    while (1)  //Master
    {
        TRAVERSE_CONNECT(i)
        {
            if (g_player[i].movefifoend <= movefifosendplc)
                return;
        }

        osyn = (input_t *)&inputfifo[(movefifosendplc-1)&(MOVEFIFOSIZ-1)];
        nsyn = (input_t *)&inputfifo[(movefifosendplc)&(MOVEFIFOSIZ-1)];

        //MASTER -> SLAVE packet
        packbuf[0] = PACKET_TYPE_MASTER_TO_SLAVE;

        j = 2;
        char playerCount = 0;
        TRAVERSE_CONNECT(i)
        {
            packbuf[j++] = i; // Player Index

            //Fix timers and buffer/jitter value
            packbuf[j++] = min(max(g_player[i].myminlag, -128), 127);
            if ((movefifosendplc & (TIMERUPDATESIZ - 1)) == 0)
                g_player[i].myminlag = 0x7fffffff;

            Net_AddPlayerInputToPacket(&j, i, osyn, nsyn);
            playerCount++;
        }
        packbuf[1] = playerCount;

        Net_AddSyncInfoToPacket(&j); // Must always be at the end of the packet.

        TRAVERSE_CONNECT(i)
            oldnet_sendpacket(i, (unsigned char*)packbuf, j);

        movefifosendplc++;

        // We're the host, terminate the match after sending the packet.
        if (TEST_SYNC_KEY(nsyn[myconnectindex].bits, SK_GAMEQUIT) &&
            (myconnectindex == connecthead))
        {
            G_GameExit(" ");
        }
    }
#endif
}

void Net_ParsePackets(void)
{
    if (numplayers < 2 || (g_networkBroadcastMode == NETMODE_OFFLINE))
        return;

    // The transport delivers each queued inbound frame via Net_ReceiveFrame().
    net_poll();
}

// Handle a single inbound frame handed up by the transport (peerToken==other).
// Formerly the body of the mmulti_getpacket() loop; the do/while(0) preserves
// the original `continue`-to-skip-this-packet semantics now that each frame
// arrives individually.
extern "C" int Net_PendingSpawnSeat(int playerNum);   // defined below (pending-seat mask)
extern "C" void Net_PendingSpawnDone(int playerNum);

void Net_ReceiveFrame(int other, int /*channel*/, const uint8_t *frameData, int packbufleng)
{
    int i, j;

    input_t *osyn, *nsyn;
#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
    (void)osyn; (void)nsyn;  // used only by the stock one-tic protocol branches
#endif

    if (packbufleng <= 0 || packbufleng > (int)sizeof(packbuf))
        return;

    Bmemcpy(packbuf, frameData, packbufleng);

    do
    {
#if 0
        LOG_F(INFO, "RECEIVED PACKET: type: %d : len %d", packbuf[0], packbufleng);
#endif
        // If we're a client, reject any packets that don't come from the server.
        if ((myconnectindex != connecthead) && other != connecthead)
            continue;

        if (packbuf[0] >= PACKET_END) // Don't accept anything that isn't in our range of defined packets.
            continue;

        lastpackettime = (int32_t)totalclock;

        switch (packbuf[0])
        {
            // ****** Base master/slave packets ******
            case PACKET_TYPE_MASTER_TO_SLAVE:  //[0] (receive master sync buffer)
            {
                if (myconnectindex == connecthead)
                {
                    OSD_Printf("PACKET_TYPE_MASTER_TO_SLAVE: MASTER SHOULDN'T GET THIS PACKET!\n");
                    continue;
                }

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
                // Host stream landed AFTER our own level entry (residual
                // old-level M2S arrives while s_eolPending and must not count):
                // the host survived its load -- release the transition grace.
                if (s_hostEolWait && !s_eolPending)
                {
                    s_hostEolWait = 0;
                    LOG_F(INFO, "[eol] host stream resumed -- load grace released");
                }
                // Tic-indexed window: self-contained, loss/reorder tolerant.
                {
                    int8_t const d = (int8_t)((uint8_t)packbuf[1] - g_netMoveEpoch);
                    if (d != 0) { g_netEpochDrops++; break; }   // wrong generation either way
                }
                j = 2;
                int32_t const start  = (int32_t)B_UNBUF32(&packbuf[j]); j += 4;
                int32_t const count  = (uint8_t)packbuf[j++];
                int32_t const pcount = (uint8_t)packbuf[j++];
                {
                    int32_t const myAck = (int32_t)B_UNBUF32(&packbuf[j]); j += 4;
                    if (myAck > s_ackOfMyInput)   // monotonic: reorder-safe
                        s_ackOfMyInput = myAck;
                    // The master filled past our sample cursor (we were frozen or
                    // cut off): those tics are canonical without us. Re-base the
                    // sampler on the first tic the master still wants -- without
                    // this every post-resume sample lands on an already-filled
                    // tic and is dup-dropped forever (permanent ghost player).
                    if (g_player[myconnectindex].connected && !g_netJoinCatchup
                        && s_ackOfMyInput > g_netSampleHead)
                        g_netSampleHead = s_ackOfMyInput;
                }
                if (pcount > MAXPLAYERS)
                    break;   // malformed

                int     slots[MAXPLAYERS];
                int32_t gone[MAXPLAYERS];
                bool bad = false;
                for (int32_t k = 0; k < pcount; k++)
                {
                    uint8_t const sb = (uint8_t)packbuf[j++];
                    int const slot = sb & 0x3f;
                    int8_t const minlag = (int8_t)packbuf[j++];
                    int32_t gtic = -1;
                    if (sb & 0x80) { gtic = (int32_t)B_UNBUF32(&packbuf[j]); j += 4; }
                    if (sb & 0x40)
                    {
                        int32_t const jtic = (int32_t)B_UNBUF32(&packbuf[j]); j += 4;
                        if (slot < MAXPLAYERS)
                            Net_ScheduleJoin(slot, jtic);
                    }
                    if (slot >= MAXPLAYERS) { bad = true; break; }
                    slots[k] = slot;
                    gone[k]  = gtic;
                    if (slot == myconnectindex)
                        otherminlag = (int32_t)minlag;
                    if (gtic >= 0)
                        Net_ScheduleDropFromWire(slot, gtic);
                }
                if (bad)
                    break;

                int32_t const have = movefifosendplc;   // contiguous master-stream high-water
                if (start > have)
                {
                    g_netGapDrops++;   // hole: our stale ack forces a wider resend next packet
                    break;
                }

                input_t recs[MAXPLAYERS];
                for (int32_t k = 0; k < pcount; k++)
                    recs[k] = s_zeroInput;

                for (int32_t t = start; t < start + count; t++)
                {
                    for (int32_t k = 0; k < pcount; k++)
                    {
                        if (gone[k] >= 0 && t >= gone[k])
                            continue;   // dropped players carry no records past their boundary
                        // Join boundaries persist past the announce window (the
                        // persistent table, not the packet flag, decides): tics
                        // below the boundary never carried a record for the slot.
                        int32_t const jt = s_joinTic[slots[k]];
                        if (jt >= 0 && t < jt)
                            continue;
                        input_t tmp;
                        j = Net_ReadInputDelta(packbuf, j, &recs[k], &tmp);
                        recs[k] = tmp;
                        // CANONICAL STREAM: apply every column, our own included.
                        // The master may have synthesized our tics (deadline-fill)
                        // -- its version is the one every other sim consumed, so
                        // it must be the one we consume too. Local samples live
                        // in g_netSendRing for the wire and the predictor.
                        if (t >= have)
                            inputfifo[t & (MOVEFIFOSIZ - 1)][slots[k]] = tmp;
                    }
                    if (t < have)
                    {
                        g_netDupTics++;
                        continue;
                    }
                    for (int32_t k = 0; k < pcount; k++)
                    {
                        int32_t const jt = s_joinTic[slots[k]];
                        if ((gone[k] < 0 || t < gone[k]) && (jt < 0 || t >= jt)
                            && g_player[slots[k]].movefifoend <= t)
                            g_player[slots[k]].movefifoend = t + 1;
                    }
                    movefifosendplc = t + 1;
                }

                Net_GetSyncInfoFromPacket(packbuf, &j, other);
#else
                osyn = (input_t*)&inputfifo[(g_player[connecthead].movefifoend - 1) & (MOVEFIFOSIZ - 1)];
                nsyn = (input_t*)&inputfifo[(g_player[connecthead].movefifoend) & (MOVEFIFOSIZ - 1)];

                char playerCount = packbuf[1];
                j = 2;
                for(int32_t i = 0; i < playerCount; i++)
                {
                    int32_t playerNum = packbuf[j++];

                    //Fix timers and buffer/jitter value
                    int32_t minlag = (int32_t)((signed char)packbuf[j++]);

                    if (((g_player[other].movefifoend & (TIMERUPDATESIZ - 1)) == 0) && (playerNum == myconnectindex))
                        otherminlag = minlag;

                    Net_GetPlayerInputFromPacket(&j, playerNum, osyn, nsyn);
                }

                Net_GetSyncInfoFromPacket(packbuf, &j, other);

                movefifosendplc++;
#endif
                break;
            }
            case PACKET_TYPE_SLAVE_TO_MASTER:  //[1] (receive slave sync buffer)
            {
#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
                // Tic-indexed window for the sending slave's own input stream.
                {
                    int8_t const d = (int8_t)((uint8_t)packbuf[1] - g_netMoveEpoch);
                    if (d != 0)
                    {
                        g_netEpochDrops++;
                        static int32_t nextEd;
                        if ((int32_t)totalclock - nextEd >= 0 || (int32_t)totalclock + 480 < nextEd)
                        {
                            nextEd = (int32_t)totalclock + 240;
                            LOG_F(INFO, "[join] S2M epoch drop: from=%d theirs=%d ours=%d",
                                  other, (int)(uint8_t)packbuf[1], (int)g_netMoveEpoch);
                        }
                        break;   // wrong generation either way
                    }
                }
                if ((unsigned)other >= MAXPLAYERS)
                    break;
                j = 2;
                int32_t const start = (int32_t)B_UNBUF32(&packbuf[j]); j += 4;
                int32_t const count = (uint8_t)packbuf[j++];
                {
                    int32_t const ack = (int32_t)B_UNBUF32(&packbuf[j]); j += 4;
                    if (s_joinFlowIsHeal && other == s_joinFlowSlot && !s_healAckFence)
                    {
                        // Heal in flight: stale pre-apply acks must not lift the
                        // rebased cursor (see s_healAckFence). Every post-apply
                        // catchup S2M carries startTic==0 -- that arms the fence.
                        if (start == 0)
                            s_healAckFence = 1;
                    }
                    if ((!s_joinFlowIsHeal || other != s_joinFlowSlot || s_healAckFence)
                        && ack > s_slaveAck[other])   // monotonic: reorder-safe
                        s_slaveAck[other] = ack;
                }

                int32_t const have = g_player[other].movefifoend;
                if (start > have)
                {
                    g_netGapDrops++;   // hole: our ack in the next M2S forces a wider resend
                    break;
                }

                input_t prev = s_zeroInput, tmp;
                for (int32_t t = start; t < start + count; t++)
                {
                    j = Net_ReadInputDelta(packbuf, j, &prev, &tmp);
                    prev = tmp;
                    if (t < have)
                    {
                        g_netDupTics++;
                        continue;
                    }
                    inputfifo[t & (MOVEFIFOSIZ - 1)][other] = tmp;
                    g_player[other].movefifoend = t + 1;
                    // Real input landed: stop synthesizing this column, and count
                    // it as liveness for the silence axe. (Dup records don't count:
                    // a tab that only replays already-filled tics is not making
                    // progress; a healthy client produces new records every tic.)
                    s_fillActive    &= ~(1 << other);
                    s_joinAwaitReal &= ~(1 << other);
                    s_lastRealRecvClock[other] = (int32_t)totalclock;
                    if (s_eolWaitMask & (1 << other))
                    {
                        s_eolWaitMask &= ~(1 << other);
                        LOG_F(INFO, "[eol] seat %d resumed after the transition -- load grace released", other);
                    }
                }

                Net_GetSyncInfoFromPacket(packbuf, &j, other);
#else
                j = 1;

                osyn = (input_t *)&inputfifo[(g_player[other].movefifoend-1)&(MOVEFIFOSIZ-1)];
                nsyn = (input_t *)&inputfifo[(g_player[other].movefifoend)&(MOVEFIFOSIZ-1)];

                Net_GetPlayerInputFromPacket(&j, other, osyn, nsyn);
                Net_GetSyncInfoFromPacket(packbuf, &j, other);
#endif
                break;
            }

            // ****** Misc packets ******
            case PACKET_TYPE_INIT_SETTINGS:
            {
                if (myconnectindex == connecthead)
                {
                    OSD_Printf("PACKET_TYPE_INIT_SETTINGS: MASTER SHOULDN'T GET THIS PACKET!\n");
                    continue;
                }

                j = 1;
                ud.m_level_number = ud.level_number     = packbuf[j++];
                ud.m_volume_number = ud.volume_number   = packbuf[j++];
                ud.m_player_skill = ud.player_skill     = packbuf[j++];

                // Non-menu variables handled by G_EnterLevel
                ud.m_coop               = packbuf[j++];
                ud.warp_on              = packbuf[j++];
                ud.fraglimit            = packbuf[j++];
                ud.multimode            = packbuf[j++];
                ud.playerai             = packbuf[j++];
                ud.m_dmflags            = (int32_t)B_UNBUF32(&packbuf[j]); j += sizeof(int32_t);
                botNameSeed             = (int32_t)B_UNBUF32(&packbuf[j]); j += sizeof(int32_t);

                if (M_DMFLAGS_TEST(DMFLAG_ALLOWVISIBILITYCHANGE))
                    LOG_F(INFO, "Visibility adjustment enabled for multiplayer.");

                oldnet_gotinitialsettings = true;

                break;
            }
            case PACKET_TYPE_NEW_GAME:
            {
                if (myconnectindex == connecthead)
                {
                    OSD_Printf("PACKET_TYPE_NEW_GAME: MASTER SHOULDN'T GET THIS PACKET!\n");
                    continue;
                }

                if (vote.level != -1 || vote.episode != -1 || vote.starter != -1)
                    G_AddUserQuote("VOTE SUCCEEDED");

                j = 1;
                // Non-menu versions of these variables are handled by G_EnterLevel
                ud.m_level_number   = packbuf[j++];
                ud.m_volume_number  = packbuf[j++];
                ud.m_player_skill   = packbuf[j++];
                ud.m_coop           = packbuf[j++];
                ud.m_dmflags        = (int32_t)B_UNBUF32(&packbuf[j]);  j += sizeof(int32_t);
                uint32_t flags      = (uint32_t)B_UNBUF32(&packbuf[j]); j += sizeof(int32_t);
                // Launch-token dedupe: the host REDELIVERS this packet until
                // our entry ack lands (a booting engine can miss the first
                // send); a duplicate of a launch we already applied must not
                // restart the level. Token 0 = legacy sender, never deduped.
                {
                    uint8_t launchToken = 0;
                    if (packbufleng > j)
                        launchToken = (uint8_t)packbuf[j++];
                    static uint8_t s_lastLaunchToken;
                    if (launchToken != 0 && launchToken == s_lastLaunchToken)
                        break;
                    s_lastLaunchToken = launchToken;
                }
                // Extended transport packet: fixed 16-byte pteam vector follows
                // the launch token. A legacy shorter sender stops at the token,
                // so length-gate the complete vector and retain locally-known
                // teams when it is absent. Apply before MODE_NEWGAME/premap so
                // each seat's initial team palette/body agrees before tic zero.
                {
                    int team[NET_TEAM_VECTOR_SIZE];
                    if (Net_DecodeTeamVector((uint8_t const *)&packbuf[j], packbufleng - j, team))
                    {
                        for (int32_t k = 0; k < MAXPLAYERS && k < NET_TEAM_VECTOR_SIZE; k++)
                            g_player[k].pteam = team[k];
                        j += NET_TEAM_VECTOR_SIZE;
                    }
                }
                if (g_netForensics && (g_gametypeFlags[ud.m_coop] & GAMETYPE_TDM))
                    LOG_F(INFO, "[team] NEW_GAME: p0=%d p1=%d p2=%d p3=%d",
                          g_player[0].pteam, g_player[1].pteam,
                          g_player[2].pteam, g_player[3].pteam);
                // [NetDuke32 port] Upstream: G_NewGame(flags | NEWGAME_FROMSERVER).
                // TODO(netcode): upstream NEWGAME_* flag nuances (NOSEND/RESETALL) are
                // not modeled by this tree's G_NewGame. Low 16 bits stay reserved for
                // them; high 16 bits are the transport seat mask (see below).
                (void)flags;
#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
                // WebRTC/seam build (browser OR native NETNATIVE) -- stock native NetDuke32
                // keeps the original G_NewGame() in the #else. When NETNATIVE, the WebRTC
                // transport is the only wire (the default native build links the no-op stub),
                // so it is safe + required to set the MP session state here too.
                // The guest runs SINGLE-PLAYER logic without this: g_netServer is a macro
                // that is FALSE on a guest (not connecthead, oldnet.h:71), and the NEW_GAME
                // packet carries m_coop/skill/level but NOT multimode or monsters_off. Set
                // the MP session state locally to match the host, or the guest gets no keys
                // (premap.cpp:788), monsters spawn, and APLAYER starts are misclassified as
                // STAT_MISC (game.cpp:2797). numplayers is already >1 here (we only receive
                // NEW_GAME once connected).
                // SEAT MASK (flags high 16 bits): the host's authoritative session
                // roster. Apply it FIRST -- a guest only ever hears peer-ups for the
                // host itself, so on 3+ player launches and every late join its local
                // connected[] (hence numplayers) is stale until this packet.
                if (flags >> 16)
                {
                    uint32_t seatMask = flags >> 16;
                    for (int32_t k = 0; k < MAXPLAYERS && k < 16; k++)
                        if (k != myconnectindex)  // our own seat is implied
                            g_player[k].connected = (seatMask >> k) & 1;
                    g_player[myconnectindex].connected = 1;
                    Net_RebuildConnectChain();
                    LOG_F(INFO, "[nnative] seat mask 0x%x -> numplayers=%d", seatMask, numplayers);
                }
                ud.multimode            = numplayers;
                g_mostConcurrentPlayers = ud.multimode;
                // GAME TYPE arrived in this packet (ud.m_coop, read above). Derive
                // monsters the SAME way the host does (menus.cpp netmenu_relaunch):
                // on for Cooperative, off for every DM/TDM variant. monsters_off is
                // not networked, so this MUST mirror the host or a coop guest gets
                // an empty level while the host has monsters.
                {
                    extern int32_t g_gametypeFlags[];
                    ud.m_monsters_off   = (g_gametypeFlags[ud.m_coop] & GAMETYPE_COOP) ? 0 : 1;
                    ud.m_player_skill   = (g_gametypeFlags[ud.m_coop] & GAMETYPE_COOP) ? 2 : 0;
                    ud.m_respawn_monsters = 0;   // mirror the host: coop is story mode, no respawn
                    if (g_gametypeFlags[ud.m_coop] & GAMETYPE_COOP)
                        ud.m_ffire = 0;          // mirror the host: no friendly fire in coop
                }
                if (g_netStreamMode
                    && (g_netJoinCatchup
                        || (g_player[myconnectindex].connected
                            && g_player[myconnectindex].ps != NULL
                            && (g_player[myconnectindex].ps->gm & MODE_GAME))))
                {
                    // STALE REDELIVERY GUARD: the host re-sends NEW_GAME until
                    // a guest acks with PLAYER_READY -- which a catchup joiner
                    // NEVER sends (it skips the barrier). The redelivery used
                    // to land right after the seat and RESTART the level over
                    // the freshly synced world (plc back to 0, column never
                    // aggregated again: "stuck in one place, can't move but I
                    // can fire; they don't have my state" -- live-reported
                    // 3rd seat, 2026-08-16). Mid-catchup or seated in-game,
                    // a NEW_GAME can only be stale: drop it.
                    LOG_F(INFO, "[nnative] stale NEW_GAME ignored (catchup=%d, already in the match)",
                          (int)g_netJoinCatchup);
                    break;
                }
                if (flags & NEWGAME_VIA_SNAPSHOT)
                {
                    // LAUNCH VIA SNAPSHOT: do NOT enter locally. The host
                    // enters first (with the CPU seats) and streams this guest
                    // the entry snapshot through the late-join pipeline --
                    // sav_begin -> cold-process entry (savegame.cpp) -> catchup
                    // -> deterministic seat. An independent local entry here is
                    // what forked every lobby guest from tic 0.
                    LOG_F(INFO, "[nnative] NEW_GAME via snapshot: awaiting entry stream (vol=%d lev=%d)",
                          ud.m_volume_number, ud.m_level_number);
                    // Lobby-style wait at MATCH START: the guest's net menu
                    // stays open while the host enters first and streams the
                    // snapshot -- say WHY nothing is happening yet.
                    NetMenu_SetStatus("Host is loading the level...");
#ifdef __EMSCRIPTEN__
                    EM_ASM({ console.log('[eng] NEW_GAME via snapshot: awaiting entry stream'); });
#endif
                    break;
                }
                // Defer the level entry to the main loop instead of calling G_NewGame here:
                // this runs INSIDE net_poll -> Net_ReceiveFrame with the shared packbuf
                // mid-parse, so entering inline would re-enter net_poll and clobber packbuf.
                // gameHandleEvents (game.cpp:7114) pumps net_poll at the TOP of the frame,
                // BEFORE the MODE_NEWGAME check at 7186, so this flag is consumed the same
                // frame. G_NewGame reads the m_* staged above at game.cpp:7046.
                g_player[myconnectindex].ps->gm = MODE_RESTART|MODE_NEWGAME;
                LOG_F(INFO, "[nnative] guest received NEW_GAME vol=%d lev=%d, deferring entry (numplayers=%d)",
                      ud.m_volume_number, ud.m_level_number, numplayers);
#else
                // Native NetDuke32 (non-Emscripten): unchanged upstream behavior.
                G_NewGame(ud.m_volume_number, ud.m_level_number, ud.m_player_skill);
#endif
                break;
            }
            case PACKET_TYPE_NULL_PACKET:
            {
                break;
            }
            case PACKET_TYPE_DESYNC_REPORT:
            {
                // A guest's CRCs flagged a split we may never see ourselves.
                // Latch the verdict AND the reporter; the heal consumer
                // (menus.cpp, host-gated, cooldown) streams a targeted
                // snapshot to the diverged peer only -- veterans play on.
                // Reports from unseated peers are noise by the watcher
                // contract (they should not send them; old builds might).
                // Stream mode has no divergence concept: never arm the ladder.
                if (!g_netStreamMode && myconnectindex == connecthead && g_player[other].connected)
                {
                    g_foundSyncError = true;
                    g_netDesyncReporters |= (1 << other);
#ifdef __EMSCRIPTEN__
                    EM_ASM({ console.log('[eng] desync report from peer ' + $0 + ' -> targeted heal'); }, other);
#else
                    LOG_F(WARNING, "[desync] report from peer %d -> targeted heal", other);
#endif
                }
                break;
            }
            case PACKET_TYPE_HIT_REPORT:
            {
                // CLIENT-AUTHORITATIVE HITSCAN (guest -> host). ONE unified wire;
                // kind selects player-seat (0) vs enemy-sprite (1) -- both native
                // and wasm speak it. The sender is 'other', so a guest can only
                // ever report its OWN hits. Bounds/sanity gate everything; a report
                // from a non-guest seat (unseated / spoofed bot) is ignored.
                if (myconnectindex != connecthead || !g_player[other].connected
                    || (g_netBotMask & (1 << other)))
                    break;
                int const kind   = (uint8_t)packbuf[1];
                int const victim = (int)((uint8_t)packbuf[2] | ((uint8_t)packbuf[3] << 8));
                int const dmg    = (int)((uint8_t)packbuf[4] | ((uint8_t)packbuf[5] << 8));
                int const wpic   = (int)((uint8_t)packbuf[6] | ((uint8_t)packbuf[7] << 8));
                if (dmg <= 0 || dmg > 4000)   // hitscan pellets are small
                    break;
                if (kind == 0)   // PLAYER seat victim
                {
                    if ((unsigned)victim >= MAXPLAYERS || !g_player[victim].connected || victim == other)
                        break;
                    Net_ApplyHitReport(other, victim, dmg, wpic);
                }
                else if (kind == 1)   // ENEMY: 20-byte, carries the hit position (idx is a hint)
                {
                    // QUEUE for on-tick drain: the apply wakes dormant monsters
                    // (changespritestat) and writes health -- both must land on the
                    // authoritative timeline right before G_MoveActors, not here
                    // mid-packet.
                    if (s_enemyHitN < (int)ARRAY_SIZE(s_enemyHit))
                    {
                        s_enemyHit[s_enemyHitN] = { (int)B_UNBUF32(&packbuf[8]), (int)B_UNBUF32(&packbuf[12]),
                                                    (int)B_UNBUF32(&packbuf[16]), (int16_t)victim,
                                                    (uint16_t)dmg, (uint16_t)wpic, (uint8_t)other };
                        s_enemyHitN++;
                    }
                }
                else if (kind == 2)   // BREAKABLE / OBJECT sprite victim -- DEFER to on-tick
                {
                    if ((unsigned)victim < MAXSPRITES && s_objHitN < (int)ARRAY_SIZE(s_objHitVictim))
                    {
                        s_objHitVictim[s_objHitN] = (int16_t)victim;
                        s_objHitAtk[s_objHitN]    = (uint8_t)other;
                        s_objHitN++;
                    }
                }
                break;
            }
            case PACKET_TYPE_WEAPON_STATE:
            {
                // GUEST -> HOST: authoritative live weapon for seat 'other'. Stash
                // off-tick; Net_ApplyGuestWeapon forces it on-tick before that seat's
                // P_ProcessInput, so the host fires what the guest actually holds
                // instead of reconstructing it from a droppable keypress.
                if (myconnectindex != connecthead || (unsigned)other >= MAXPLAYERS
                    || !g_player[other].connected || (g_netBotMask & (1 << other)))
                    break;
                int const w = (uint8_t)packbuf[1];
                if ((unsigned)w >= MAX_WEAPONS || packbufleng < 8)
                    break;
                // Unordered channel: drop anything not strictly newer than the last
                // applied report (wraparound-safe), else a stale packet flaps the
                // seat's weapon back (seen live: 2->0->2 within 100ms).
                uint16_t const seq = (uint16_t)((uint8_t)packbuf[6] | ((uint8_t)packbuf[7] << 8));
                if (s_gwHave[other] && (int16_t)(seq - s_gwSeq[other]) <= 0)
                    break;
                s_gwSeq[other]    = seq;
                s_gwWeapon[other] = (int8_t)w;
                s_gwGot[other]    = (uint16_t)((uint8_t)packbuf[2] | ((uint8_t)packbuf[3] << 8));
                s_gwAmmo[other]   = (int16_t)((uint8_t)packbuf[4] | ((uint8_t)packbuf[5] << 8));
                s_gwHave[other]   = 1;
                break;
            }
            case PACKET_TYPE_POS_REPORT:
            {
                // GUEST -> HOST: client-authoritative self position. Stash the
                // newest (seq-guarded, unordered channel); Net_ApplyGuestPos
                // snaps the seat on-tick before its P_ProcessInput.
                if (myconnectindex != connecthead || (unsigned)other >= MAXPLAYERS
                    || !g_player[other].connected || (g_netBotMask & (1 << other))
                    || packbufleng < 41)
                    break;
                uint16_t const seq = (uint16_t)((uint8_t)packbuf[1] | ((uint8_t)packbuf[2] << 8));
                if (s_gpHave[other] && (int16_t)(seq - s_gpSeq[other]) <= 0)
                    break;
                s_gpSeq[other]   = seq;
                int jj = 3;
                s_gpPos[other].x = (int32_t)B_UNBUF32(&packbuf[jj]); jj += 4;
                s_gpPos[other].y = (int32_t)B_UNBUF32(&packbuf[jj]); jj += 4;
                s_gpPos[other].z = (int32_t)B_UNBUF32(&packbuf[jj]); jj += 4;
                s_gpVel[other].x = (int32_t)B_UNBUF32(&packbuf[jj]); jj += 4;
                s_gpVel[other].y = (int32_t)B_UNBUF32(&packbuf[jj]); jj += 4;
                s_gpVel[other].z = (int32_t)B_UNBUF32(&packbuf[jj]); jj += 4;
                s_gpAng[other]   = (fix16_t)B_UNBUF32(&packbuf[jj]); jj += 4;
                s_gpHoriz[other] = (fix16_t)B_UNBUF32(&packbuf[jj]); jj += 4;
                s_gpSect[other]  = (int16_t)B_UNBUF16(&packbuf[jj]); jj += 2;
                s_gpSprZ[other]  = (int32_t)B_UNBUF32(&packbuf[jj]); jj += 4;
                s_gpHave[other]  = 1;
                break;
            }
            case PACKET_TYPE_STATE_SNAP:
            {
                if (s_eolPending)   // mid level-flip: a stale snap teleports us
                    break;
                { extern int32_t g_netDbgPackN; g_netDbgPackN++; }
                // Host pushed an in-place correction. TIC-STAMPED: stash it and
                // apply at EXACTLY the stamped consume count (see
                // Net_ApplyPendingStateSnap, run at the top of every consumed
                // tic). Applied at any other tic, the correction itself installs
                // tics of position/RNG offset -- the ladder then re-latches off
                // its own snap and the guest "teleports" every few seconds.
                // Watchers skip: their pending snapshot supersedes this.
                if (myconnectindex == connecthead || other != connecthead || g_netJoinCatchup)
                    break;
                int32_t const snapTic = (int32_t)B_UNBUF32(&packbuf[2]);
                int const cnt = (uint8_t)packbuf[1];
                int const base = 14 + cnt * 75;
                // Trailing animwall tag phases: [count][int16 tags...].
                int const awc = (uint8_t)packbuf[base];
                int const len = base + 1 + awc * 2;
                // Stream mode: the pack is a PAINT of current truth, not a
                // timeline repair -- "stale on arrival" does not exist; the
                // newest pack always supersedes whatever is pending.
                if (len > (int)sizeof(s_pendingSnap))
                {
                    // NEVER silent again: the old 768B stash dropped every pack
                    // past ~8 seats right here -- the extra players appeared to
                    // join but simply never moved, with zero log evidence.
                    extern int32_t g_netForensics;   // defined below (file pattern)
                    if (g_netForensics)
                        LOG_F(ERROR, "[snap] pack DROPPED len=%d cap=%d", len, (int)sizeof(s_pendingSnap));
                    break;   // oversized: cannot stash
                }
                if (!g_netStreamMode && snapTic < movefifoplc)
                {
#ifdef __EMSCRIPTEN__
                    EM_ASM({ console.log('[eng] softsnap DROPPED (tic=' + $0 + ' plc=' + $1 + ' len=' + $2 + ')'); },
                           snapTic, movefifoplc, len);
#endif
                    break;   // stale on arrival (we consumed past it)
                }
                // UNORDERED-CHANNEL GUARD: STATE_SNAP now rides the unreliable
                // UNORDERED channel (the OpenArena snapshot model), so an OLDER
                // snapshot can arrive AFTER a newer one. Drop a straggler so it
                // never paints stale positions over fresh ones -- but only a
                // RECENT backward step; a large jump back is a new match / tic
                // reset / wrap and must pass through.
                {
                    static int32_t s_lastAcceptedSnapTic = INT32_MIN;
                    if (g_netStreamMode && snapTic <= s_lastAcceptedSnapTic
                        && (uint32_t)(s_lastAcceptedSnapTic - snapTic) < 600u)
                        break;
                    s_lastAcceptedSnapTic = snapTic;
                }
                Bmemcpy(s_pendingSnap, packbuf, len);
                s_pendingSnapLen = len;
                s_pendingSnapTic = snapTic;
#ifdef __EMSCRIPTEN__
                {
                    extern int32_t g_netForensics;   // defined below (file pattern)
                    if (!g_netStreamMode || g_netForensics)
                        EM_ASM({ console.log('[eng] softsnap stashed for tic ' + $0 + ' (plc=' + $1 + ')'); },
                               snapTic, movefifoplc);
                }
#endif
                break;
            }
            case PACKET_TYPE_SPRITE_STREAM:
            {
                if (s_eolPending)   // mid level-flip: stale paints corrupt the fresh world
                    break;
                // Host's continuous authoritative sprite deltas (stream mode).
                // Applied AT RECEIVE: this is a paint of current truth -- the
                // sooner it lands the smaller the visible drift. Guests only;
                // a watcher mid-catchup has a world mid-load (skip).
                if (myconnectindex == connecthead || other != connecthead
                    || g_netJoinCatchup || !g_netStreamMode)
                    break;
                if (g_player[myconnectindex].ps == NULL
                    || !(g_player[myconnectindex].ps->gm & MODE_GAME))
                    break;
                { extern int32_t g_netDbgSprN; g_netDbgSprN++; }
                Net_ApplySpriteStream((const char *)packbuf, packbufleng);
                break;
            }
            case PACKET_TYPE_SECTOR_STREAM:
            {
                if (s_eolPending)   // mid level-flip
                    break;
                // Host-owned sector heights (doors/elevators). Same guards as
                // the sprite stream.
                if (myconnectindex == connecthead || other != connecthead
                    || g_netJoinCatchup || !g_netStreamMode)
                    break;
                if (g_player[myconnectindex].ps == NULL
                    || !(g_player[myconnectindex].ps->gm & MODE_GAME))
                    break;
                Net_ApplySectorStream((const char *)packbuf, packbufleng);
                break;
            }
            case PACKET_TYPE_WALL_STREAM:
            {
                if (s_eolPending)   // mid level-flip
                    break;
                // Host-owned wall-motion doors (swing/slide). Same guards.
                if (myconnectindex == connecthead || other != connecthead
                    || g_netJoinCatchup || !g_netStreamMode)
                    break;
                if (g_player[myconnectindex].ps == NULL
                    || !(g_player[myconnectindex].ps->gm & MODE_GAME))
                    break;
                Net_ApplyWallStream((const char *)packbuf, packbufleng);
                break;
            }
            case PACKET_TYPE_ACCESS_STATE:
            {
                // SHARED COOP ACCESS: idempotent OR of the 3 key-card bits.
                // guest->host: that seat earned a card in its local sim;
                // host->guest: the coop-wide union (applied to every local
                // seat so the guest's own door checks pass immediately).
                if (packbufleng < 2 || !g_netStreamMode
                    || !(g_gametypeFlags[ud.coop] & GAMETYPE_COOP))
                    break;
                int const mask = (uint8_t)packbuf[1] & 7;
                if (myconnectindex == connecthead)
                {
                    if ((unsigned)other < MAXPLAYERS && g_player[other].ps != NULL)
                    {
                        g_player[other].ps->got_access |= (int16_t)mask;   // union spreads next tic
                        LOG_F(INFO, "[access] seat %d reports cards mask=%d", other, mask);
                    }
                }
                else if (other == connecthead)
                {
                    int ai;
                    TRAVERSE_CONNECT(ai)
                        if (g_player[ai].ps != NULL)
                            g_player[ai].ps->got_access |= (int16_t)mask;
                    LOG_F(INFO, "[access] host shared cards mask=%d", mask);
                }
                break;
            }
            case PACKET_TYPE_READY_ROSTER:
            {
                // Host's ~1Hz who-is-in bitmask while it sits at the entry
                // barrier. DISPLAY ONLY (the guest's wait screen names who is
                // still loading) -- never touches playerreadyflag, so the
                // release handshake is byte-for-byte the old one.
                extern uint32_t g_netBarrierReadyMask;
                if (packbufleng >= 5 && other == connecthead)
                    g_netBarrierReadyMask = (uint32_t)B_UNBUF32(&packbuf[1]);
                return;
            }
            case PACKET_TYPE_LMS_ROUND_RESET:
            {
                // Explicit host-authoritative round boundary. Guests mirror the
                // fixed life vector, announce the winner, and resurrect every
                // connected dead body locally. No Net_LmsAllowRespawn call: this
                // reset grants fresh lives and must never spend one independently.
                if (packbufleng >= 2 + NET_TEAM_VECTOR_SIZE && other == connecthead
                    && myconnectindex != connecthead
                    && (g_gametypeFlags[ud.coop] & GAMETYPE_LMS))
                {
                    int const winner = (uint8_t)packbuf[1] - 1;
                    for (int i = 0; i < MAXPLAYERS && i < NET_TEAM_VECTOR_SIZE; i++)
                        g_lmsLives[i] = (int8_t)clamp((int32_t)(uint8_t)packbuf[2 + i], 0, LMS_LIVES);
                    if ((unsigned)winner < MAXPLAYERS && g_player[winner].ps != NULL)
                    {
                        Bsnprintf(apStrings[QUOTE_RESERVED], MAXQUOTELEN, "%s WINS THE ROUND", g_player[winner].user_name);
                        P_DoQuote(QUOTE_RESERVED, g_player[winner].ps);
                    }
                    for (int i = 0; i < MAXPLAYERS; i++)
                    {
                        auto const p = g_player[i].ps;
                        if (!g_player[i].connected || p == NULL || (unsigned)p->i >= MAXSPRITES)
                            continue;
                        if (sprite[p->i].extra <= 0 || p->dead_flag)
                            P_ResetMultiPlayer(i);
                    }
                    s_lmsInit = 1;
                    s_lmsCooldown = 260;
                    LOG_F(INFO, "[lms] authoritative round reset applied winner=%d", winner);
                }
                return;
            }
            case PACKET_TYPE_SEAT_SPAWNED:
            {
                // Host's "seat pk[1] just spawned" telegram (on-demand join
                // spawn or CON respawn). Bystander guests receive ZERO-FILLED
                // inputs for other guests, so their sim never reaches the
                // spawn opcode for those seats -- without the telegram the
                // seat stayed a corpse here until the next aligned softsnap
                // (observed 65s live: "joining can desync another guest").
                // Position is NOT carried: the non-owner spawn hold keeps the
                // body where it is and the owner's POS_REPORT/gpos stream
                // places it within a beat.
                if (packbufleng >= 2 && other == connecthead && g_netStreamMode
                    && myconnectindex != connecthead)
                {
                    int const seat = packbuf[1];
                    // Include the owning guest itself. Its local simulation may
                    // have remained eliminated because only the host spends LMS
                    // lives; the telegram is the authoritative resurrection.
                    if ((unsigned)seat < MAXPLAYERS
                        && g_player[seat].ps != NULL
                        && (unsigned)g_player[seat].ps->i < MAXSPRITES)
                    {
                        int const pend = Net_PendingSpawnSeat(seat);
                        bool const dead = (sprite[g_player[seat].ps->i].extra <= 0
                                           || g_player[seat].ps->dead_flag);
                        if (pend || dead)
                        {
                            if (pend)
                                Net_PendingSpawnDone(seat);
                            P_ResetMultiPlayer(seat);
                            LOG_F(INFO, "[seat] remote spawn applied p=%d (pend=%d dead=%d)",
                                  seat, pend, (int)dead);
                        }
                        // Already alive: our sim processed this spawn itself
                        // (host seat / own seat) -- the telegram is a dupe.
                    }
                }
                return;
            }
            case PACKET_TYPE_LEVEL_GO:
            {
                // STREAM MODE: the host's EXPLICIT entry release ("there
                // should be explicit signaling from the host", 2026-08-16) --
                // sent ONLY from its genuine barrier release (loaded + all
                // seats reported). Value-carrying (the host's crossing number
                // this entry), so duplicates/ordering can never inflate; the
                // EOL fence zeroes both sides before the next entry.
                if (other != connecthead || packbufleng < 2)
                    return;
                int const go = (uint8_t)packbuf[1];
                if (go > g_player[connecthead].playerreadyflag)
                    g_player[connecthead].playerreadyflag = (char)go;
                LOG_F(INFO, "[barrier] host GO crossing=%d", go);
                return;
            }
            case PACKET_TYPE_JOIN_ACK:
            {
                // Catchup peer's reliable progress ack (see the sender in
                // Net_HandleInput). Paces Net_HostJoinFlow: seat/heal
                // completion fires when this passes the stream base.
                if (myconnectindex != connecthead || (unsigned)other >= MAXPLAYERS
                    || packbufleng < 6)
                    return;
                {
                    int8_t const d = (int8_t)((uint8_t)packbuf[1] - g_netMoveEpoch);
                    if (d != 0)
                        return;   // stale generation
                }
                int32_t const ack = (int32_t)B_UNBUF32(&packbuf[2]);
                // A joiner in the catchup pipeline consumed its entry state
                // from the snapshot -- stop the NEW_GAME redelivery loop that
                // would otherwise restart its level post-seat (it never sends
                // the PLAYER_READY that normally acks it).
                s_newGameAckMask |= (1u << other);
                if (s_joinFlowIsHeal && other == s_joinFlowSlot)
                    s_healAckFence = 1;   // post-apply by construction
                if (ack > s_slaveAck[other])
                    s_slaveAck[other] = ack;
                s_lastRealRecvClock[other] = (int32_t)totalclock;
                return;
            }
            case PACKET_TYPE_PLAYER_READY:
            {
                // STREAM MODE: a bare READY from the host is NOT a release.
                // The master echo below answers a guest's READY from ANY
                // pre-barrier packet drain (bonus screen, load screen) -- a
                // fast guest's barrier released while the host was half
                // loaded and it free-ran into the level (live-reported
                // 2026-08-16). Host readiness travels ONLY via LEVEL_GO.
                if (g_netStreamMode && myconnectindex != connecthead && other == connecthead)
                    return;
                if (g_player[other].playerreadyflag == 0)
                    LOG_F(INFO, "Player %d is ready", other);
                g_player[other].playerreadyflag++;
#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
                if (myconnectindex == connecthead)
                {
                    // Entry ack: stop NEW_GAME redelivery for this guest.
                    s_newGameAckMask |= (1u << other);
                    // ENTRY AUDIT -- the guest reports the world size it
                    // entered with; an empty-world entry (missed roster,
                    // live-reported twice) is visible in the host log.
                    if (packbufleng >= 2)
                    {
                        int const guestNp = (uint8_t)packbuf[1];
                        initprintf("net: guest %d entered with np=%d (host np=%d)\n",
                                   other, guestNp, numplayers);
#ifdef __EMSCRIPTEN__
                        EM_ASM({ console.log('[eng] entry audit: guest ' + $0 + ' np=' + $1 + ' hostNp=' + $2); },
                               other, guestNp, numplayers);
#endif
                    }
                }
                // STREAM GO-ECHO (mid-game join rendezvous): a joiner's seat
                // restores the snapshot's zeroed readyflags, so any barrier it
                // crosses afterwards waits for a release that was broadcast
                // before it existed ("stuck waiting for the go which had
                // already been given" -- live-reported 3rd DM seat,
                // 2026-08-16). Value gate keeps the load-leak fix intact: we
                // answer ONLY for crossings we have genuinely released
                // (our flag >= the reporter's); mid-load our flag is 0 (EOL
                // fence), so a loading host still answers nobody.
                if (myconnectindex == connecthead && other != myconnectindex && g_netStreamMode
                    && g_player[myconnectindex].playerreadyflag >= g_player[other].playerreadyflag)
                {
                    packbuf[0] = PACKET_TYPE_LEVEL_GO;
                    packbuf[1] = g_player[myconnectindex].playerreadyflag;
                    oldnet_sendpacket(other, (unsigned char *)packbuf, 2);
                }
                // MASTER ECHO (late-join rendezvous), LEGACY CLASSIC MODE ONLY:
                // the master's one-shot READY broadcast can land while a late
                // joiner is still LOADING the snapshot -- the load then restores
                // the saved (zero) readyflags over it and the joiner waits
                // forever for a packet that already came. Echoing READY back is
                // idempotent there. In STREAM mode this echo was the leak that
                // released loading-gate guests early (any pre-barrier drain
                // answered them); stream release is LEVEL_GO exclusively, and
                // stream late-joiners skip the barrier via g_netJoinCatchup.
                if (myconnectindex == connecthead && other != myconnectindex && !g_netStreamMode)
                {
                    packbuf[0] = PACKET_TYPE_PLAYER_READY;
                    oldnet_sendpacket(other, (unsigned char *)packbuf, 1);
                }
#endif
                return;
            }
            case PACKET_TYPE_PING:
            {
                j = 1;
                if (myconnectindex == connecthead) // We're the master, recieving from the client
                {
                    Net_GetPingTimeFromPacket(&j);
                    uint32_t tics = timerGetTicks();
                    g_player[other].ping = tics - pingTime;
                }
                else
                {
                    Net_GetPingTimeFromPacket(&j);
                    TRAVERSE_CONNECT(i)
                    {
                        Net_GetPlayerPingFromPacket(&j, i);
                    }

                    // Send ping time we got from master back to it, to calculate how long it took.
                    j = 1;
                    Net_AddPingTimeToPacket(&j);
                    oldnet_sendpacket(connecthead, (unsigned char*)packbuf, j);
                }
                break;
            }
            case PACKET_TYPE_MESSAGE:
            {
                //slaves in M/S mode only send to master
                if (myconnectindex == connecthead)
                {
                    if (packbuf[1] == 255)
                    {
                        //Master re-transmits message to all others
                        TRAVERSE_CONNECT(i)
                            if (i != other)
                                oldnet_sendpacket(i, (unsigned char*)packbuf, packbufleng);
                    }
                    else if (((int)packbuf[1]) != myconnectindex)
                    {
                        //Master re-transmits message not intended for master
                        oldnet_sendpacket((int)packbuf[1], (unsigned char*)packbuf, packbufleng);
                        break;
                    }
                }

                Bstrcpy(recbuf, packbuf + 2);
                recbuf[packbufleng - 2] = 0;

                G_AddUserQuote(recbuf);
                S_PlaySound(EXITMENUSOUND);

                pus = NUMPAGES;
                pub = NUMPAGES;

                break;
            }
            case PACKET_TYPE_FRAGLIMIT_CHANGED:
            {
                if (myconnectindex == connecthead)
                {
                    OSD_Printf("PACKET_TYPE_FRAGLIMIT_CHANGED: MASTER SHOULDN'T GET THIS PACKET!\n");
                    continue;
                }

                ud.fraglimit = packbuf[1];
                Bsprintf(tempbuf, "FRAGLIMIT CHANGED TO %d", ud.fraglimit);
                G_AddUserQuote(tempbuf);
                break;
            }
            case PACKET_TYPE_EOL:
            {
                // Host-authoritative level transition (stream mode): only the
                // host may end a level; guests adopt its progression wholesale.
                // NO gm write here: packets can pump inside the prediction
                // window, where g_player[].ps is the prediction copy and the
                // write would be restored away (eoltest-caught: the guest
                // logged this receive and then never transitioned). The main
                // loop arms MODE_EOL from g_netEolFromHost at a safe point.
                if (other != connecthead)
                    break;
                ud.level_number = packbuf[1];
                ud.from_bonus = packbuf[2];
                ud.secretlevel = packbuf[3];
                if (packbufleng > 5)
                {
                    ud.volume_number   = packbuf[4];
                    ud.m_volume_number = packbuf[4];
                    ud.eog             = packbuf[5];
                }
                ud.m_level_number = ud.level_number;
                g_netEolFromHost = 1;
                s_eolPending     = 1;
                // EPOCH FENCE (mirror of Net_SendEol): zero the cumulative
                // readiness counters so this entry's barrier genuinely waits.
                // The echo + release broadcast inflate our copy of the host's
                // flag ~2 per crossing vs our own 1; carried across levels the
                // surplus made every later barrier release instantly and we
                // free-ran into the level while the host was still loading.
                // Safe in the prediction window: playerreadyflag lives in
                // playerdata_t, not the predicted DukePlayer_t. Ordering is
                // safe on the reliable channel: any host READY for the NEW
                // entry is sent after this packet, so it cannot precede the
                // zeroing here.
                for (int prf = 0; prf < MAXPLAYERS; prf++)
                    g_player[prf].playerreadyflag = 0;
                // The host unloads its world right after this broadcast (and so
                // do we): hold the host-silence axe until its stream resumes.
                s_hostEolWait  = 1;
                s_hostEolClock = (int32_t)totalclock;
                LOG_F(INFO, "[eol] host says level over -> E%dL%d eog=%d",
                      ud.volume_number + 1, ud.level_number + 1, (int)ud.eog);
                Net_TestSlowLoad("guest");
                break;
            }

            // ****** Re-transmittable packets ******
            default:
            {
                switch (packbuf[0])
                {
                    case PACKET_TYPE_VERSION:
                    {
                        if (packbuf[2] != (char)atoi(s_buildRev))
                        {
                            LOG_F(ERROR, "Player has version %d, expecting %d", packbuf[2], (char)atoi(s_buildRev));
                            G_GameExit("You cannot play with different versions of NetDuke32!");
                        }
                        if (packbuf[3] != (char)BYTEVERSION)
                        {
                            LOG_F(ERROR, "Player has version %d, expecting %d (%d, %d, %d)", packbuf[3], BYTEVERSION, BYTEVERSION_NETDUKE32, PLUTOPAK, VOLUMEONE);
                            G_GameExit("You cannot play Duke with different versions!");
                        }

                        break;
                    }
                    case PACKET_TYPE_PLAYER_OPTIONS:
                    {
                        int32_t playerNum = packbuf[1];
                        if ((myconnectindex == connecthead) && playerNum != other)
                        {
                            OSD_Printf("PACKET_TYPE_PLAYER_OPTIONS: Player index doesn't match client index!\n");
                            playerNum = packbuf[1] = other; // Sanitize before re-transmit.
                        }

                        j = 2;
                        g_player[playerNum].ps->auto_aim = packbuf[j++];
                        g_player[playerNum].ps->weaponswitch = packbuf[j++];
                        g_player[playerNum].ps->palookup = g_player[playerNum].pcolor = playerColor_getValidPal(packbuf[j++]);
                        g_player[playerNum].pteam = packbuf[j++];

                        break;
                    }
                    case PACKET_TYPE_PLAYER_NAME:
                    {
                        int32_t playerNum = packbuf[1];
                        if ((myconnectindex == connecthead) && playerNum != other)
                        {
                            OSD_Printf("PACKET_TYPE_PLAYER_NAME: Player index doesn't match client index!\n");
                            playerNum = packbuf[1] = other; // Sanitize before re-transmit.
                        }

                        for (i = 2; packbuf[i] && i < (int32_t)sizeof(g_player[0].user_name); i++)
                        {
                            g_player[playerNum].user_name[i - 2] = packbuf[i];
                        }
                        g_player[playerNum].user_name[i - 2] = 0;
                        i++;

                        LOG_F(INFO, "Player %d's name is now %s", playerNum, g_player[playerNum].user_name);

                        break;
                    }
                    case PACKET_TYPE_WEAPON_CHOICE:
                    {
                        int32_t playerNum = packbuf[1];
                        if ((myconnectindex == connecthead) && playerNum != other)
                        {
                            OSD_Printf("PACKET_TYPE_WEAPON_CHOICE: Player index doesn't match client index!\n");
                            playerNum = packbuf[1] = other; // Sanitize before re-transmit.
                        }

                        i = 2;

                        j = i; //This used to be Duke packet #9... now concatenated with Duke packet #6
                        for (; i - j < 10; i++) g_player[playerNum].wchoice[i - j] = packbuf[i];

                        break;
                    }
                    case PACKET_TYPE_RTS:
                    {
                        // (3) DEFERRED: RTS taunt-over-net. Playback depends on
                        // rts_numlumps / g_RTSPlaying, which are file-private in this
                        // tree's RTS subsystem (not ported). Drop the taunt loudly.
                        NETDUKE32_MP_TODO("RTS taunt-over-net");
                        break;
                    }
                    case PACKET_TYPE_MENU_LEVEL_QUIT:
                    {
#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
                        // packbuf[1] = the aborting player. Classic killed EVERY
                        // peer's app here; on the transport track only the HOST
                        // aborting ends the match (gracefully, via the host-gone
                        // consumer). A guest's abort is just that guest leaving:
                        // its transport peer-down handles the seat.
                        if ((int32_t)(uint8_t)packbuf[1] == connecthead && myconnectindex != connecthead)
                        {
                            Net_SetLocalBot(0);
                            g_netHostGone = 1;
                        }
#else
                        G_GameExit("Game aborted from menu; disconnected.");
#endif
                        break;
                    }
                    case PACKET_TYPE_USER_MAP:
                    {
                        Bstrcpy(boardfilename, packbuf + 1);
                        boardfilename[packbufleng - 1] = 0;
                        Bcorrectfilename(boardfilename, 0);
                        if (boardfilename[0] != 0)
                        {
                            if ((i = kopen4loadfrommod(boardfilename, 0)) < 0)
                            {
                                Bmemset(boardfilename, 0, sizeof(boardfilename));
                                Net_SendUserMapName();
                            }
                            else kclose(i);
                        }

                        if (ud.m_level_number == 7 && ud.m_volume_number == 0 && boardfilename[0] == 0)
                            ud.m_level_number = 0;

                        break;
                    }
                    case PACKET_TYPE_MAP_VOTE:
                    {
                        int32_t playerNum = packbuf[1];
                        if ((myconnectindex == connecthead) && playerNum != other)
                        {
                            OSD_Printf("PACKET_TYPE_MAP_VOTE: Player index doesn't match client index!\n");
                            playerNum = packbuf[1] = other; // Sanitize before re-transmit.
                        }

                        if ((vote.starter == myconnectindex || myconnectindex == connecthead) && g_player[playerNum].gotvote == 0)
                        {
                            g_player[playerNum].gotvote = 1;
                            if (packbuf[2])
                                vote.yes_votes++;

                            Bsprintf(tempbuf, "CONFIRMED VOTE FROM %s", g_player[playerNum].user_name);
                            G_AddUserQuote(tempbuf);
                        }
                        break;
                    }
                    case PACKET_TYPE_MAP_VOTE_INITIATE: // call map vote
                    {
                        int32_t playerNum = packbuf[1];
                        if ((myconnectindex == connecthead) && playerNum != other)
                        {
                            OSD_Printf("PACKET_TYPE_MAP_VOTE_INITIATE: Player index doesn't match client index!\n");
                            playerNum = packbuf[1] = other; // Sanitize before re-transmit.
                        }

                        j = 2;

                        vote.starter            = playerNum;
                        vote.episode            = packbuf[j++];
                        vote.level              = packbuf[j++];
                        vote.skill              = packbuf[j++];
                        vote.gametype           = packbuf[j++];
                        vote.dmflags            = (int32_t)B_UNBUF32(&packbuf[j]); j += sizeof(int32_t);

                        Bsprintf(tempbuf, "%s^00 HAS CALLED A VOTE TO CHANGE MAP TO %s (E%dL%d)",
                            g_player[playerNum].user_name,
                            g_mapInfo[vote.episode * MAXLEVELS + vote.level].name,
                            vote.episode + 1, vote.level + 1);
                        G_AddUserQuote(tempbuf);

                        Bsprintf(tempbuf, "PRESS F1 TO ACCEPT, F2 TO DECLINE");
                        G_AddUserQuote(tempbuf);

                        for (ALL_PLAYERS(i))
                        {
                            g_player[i].gotvote = 0;
                        }

                        g_player[vote.starter].gotvote = vote.yes_votes = 1;
                        break;
                    }
                    case PACKET_TYPE_MAP_VOTE_CANCEL: // cancel map vote
                    {
                        int32_t playerNum = packbuf[1];
                        if ((myconnectindex == connecthead) && playerNum != other)
                        {
                            OSD_Printf("PACKET_TYPE_MAP_VOTE_CANCEL: Player index doesn't match client index!\n");
                            playerNum = packbuf[1] = other; // Sanitize before re-transmit.
                        }

                        if (vote.starter == playerNum)
                        {
                            vote = votedata_t();
                            int32_t numVotes = 0;

                            for(ALL_PLAYERS(i))
                                numVotes += g_player[i].gotvote;

                            if (numVotes != numplayers)
                                Bsprintf(tempbuf, "%s^00 HAS CANCELED THE VOTE", g_player[playerNum].user_name);
                            else
                                Bsprintf(tempbuf, "VOTE FAILED");

                            G_AddUserQuote(tempbuf);

                            for (ALL_PLAYERS(i))
                            {
                                g_player[i].gotvote = 0;
                            }
                            
                        }
                        break;
                    }
                    case PACKET_TYPE_LOAD_GAME:
                    {
                        // (2) DEFERRED: LOAD_GAME network relay. This tree's G_LoadPlayer
                        // takes a savebrief_t&, not netduke32's int slot -- a savegame-API
                        // divergence (not ported). Ignore the relayed load loudly.
                        NETDUKE32_MP_TODO("LOAD_GAME network relay");
                        break;
                    }
                }

                //Slaves in M/S mode only send to master
                //Master re-transmits message to all others
                if (myconnectindex == connecthead)
                    TRAVERSE_CONNECT(i)
                        if (i != other)
                            oldnet_sendpacket(i, (unsigned char*)packbuf, packbufleng);
                break;
            }
        }
    } while (0);
}

// ---------------------------------------------------------------------------
// Connection handshake (transport -> netcode).
//
// The transport is a STAR listen-server with host-authoritative slot
// assignment (host == slot 0; peerToken == connectindex). These entry points
// feed its decisions into the classic connectindex/connecthead/connectpoint2/
// numplayers state the lockstep protocol runs on. netduke32's oldnet does not
// negotiate connectindex itself (classic mmulti set it); the transport does.
// ---------------------------------------------------------------------------
static void Net_RebuildConnectChain(void)
{
    // Rebuild the connect chain (terminated by -1) and player count from the
    // connected flags. connecthead is the lowest connected slot (host hub).
    int count = 0, prev = -1, head = -1;

    for (int i = 0; i < MAXPLAYERS; i++)
    {
        if (!g_player[i].connected)
            continue;

        ++count;
        if (head < 0) head = i;
        if (prev >= 0) connectpoint2[prev] = i;
        prev = i;
    }

    if (prev >= 0) connectpoint2[prev] = -1;

    connecthead = (head < 0) ? 0 : head;
    numplayers  = max(count, 1);

    // Every seated player must be PROCESSABLE: P_ProcessInput early-outs on
    // playerquitflag==0 (player.cpp:5018), and the only thing that ever set it was
    // app_main's boot loop over ud.multimode -- which is 1 on every transport-track
    // boot (the classic -net connect path never runs). Result: player 2 could
    // receive input, sync CRCs, render -- and NEVER MOVE, on every stack, since the
    // transport track existed (live-reported: "player 2 isn't doing the multiplayer
    // loop properly"). This is the single chokepoint every seat path funnels
    // through (peer-up, seat mask, Net_SetLocalIndex, late join). Quit handling
    // zeroes the flag again on PACKET_TYPE_QUIT (game.cpp:7373) after which the
    // peer-down also drops `connected`, so we never resurrect a quitter here.
    for (int k = 0; k < MAXPLAYERS; k++)
        if (g_player[k].connected)
            g_player[k].playerquitflag = 1;

    // Session size follows the live lobby, established at CONNECT time exactly
    // like the classic connect path did via PACKET_TYPE_INIT_SETTINGS (which
    // nothing on the transport track sends). This matters twice over:
    // 1. demo.cpp:505 gates attract-demo playback on `ud.multimode < 2` -- a
    //    connected guest still at multimode=1 keeps looping SP attract demos,
    //    whose restored playback state overwrites live ud.* between the
    //    NEW_GAME packet and level entry.
    // 2. prelevel keys ALL multiplayer map setup off `(g_netServer ||
    //    ud.multimode > 1)` (premap.cpp:147/215/548/610/788), and g_netServer
    //    is false on guests -- multimode=1 there means a single-player world
    //    that immediately desyncs from the host's.
    // Never resize a running match: mid-game drops keep the session size they
    // started with (the entry paths pin multimode again at launch anyway).
    if (g_player[myconnectindex].ps == NULL || !(g_player[myconnectindex].ps->gm & MODE_GAME))
        ud.multimode = numplayers;
}

// Bitmask of slots whose peer-up arrived while the host was in-game. Consumed by
// Net_SeatLateJoiners() from the host's late-join relaunch (menus.cpp).
int32_t g_netLateJoinMask = 0;

// Set when the HOST's peer goes down on a guest. Consumed by the menus.cpp
// NETMENU block: tear the match down and return to the MAIN MENU instead of
// starving the tic loop (or silently soloing the map).
int32_t g_netHostGone = 0;

// ── Late-join SNAPSHOT (no round reset) ──────────────────────────────────────
// The host seats a late joiner in the LIVE world, snapshots the whole game via
// the savegame system, and the transport streams the file to every peer; each
// receiver reloads the identical bytes and everyone rendezvouses at the
// Net_WaitForPlayers barrier. Veterans keep positions/frags (they reload what
// they already have, minus at most the transfer window); the host never leaves
// the level at all.
#define LATEJOIN_SAVE "latejoin.esv"

int32_t g_netSnapshotReady = 0;         // receiver: LATEJOIN_SAVE landed in the FS
static uint32_t g_netSnapshotMask = 0;  // bits 0..15 seat mask, 16..23 move epoch

// MOVE EPOCH: stamps every M2S/S2M packet. Bumped by Net_ClearFIFO (i.e., at every
// barrier, symmetrically on all peers); receivers DISCARD mismatched packets. This
// kills the whole "stale move packets in flight across a rendezvous contaminate
// the fresh FIFOs" divergence class -- the reliable channel happily delivers
// pre-barrier moves after the reset, and without the stamp they became the first
// "inputs" of the new session, permanently shifting the stream pairing
// (live-reported as mirrored movement + kills that never propagate).
uint8_t g_netMoveEpoch = 0;
int32_t g_netEpochDrops = 0; // discarded stale-generation move packets (debug surface)

// Prediction feature mask / kill-switch (also the desync-validation bisect):
//   bit0 (1) = Net_CorrectPrediction runs (the correction replay)
//   bit1 (2) = predicted-view render swap active
//   bit2 (4) = RESERVED for P3 (instant weapon visuals) -- NOT implemented
//   bit3 (8) = P2 instant local action sounds (Net_LocalSoundGate watermark)
//   bit4 (16) = idle correction deadband (skip the reset+replay when a stream
//               guest is stationary + predicted matches sim -- kills the
//               standstill-flashing shimmer; see net_predict.cpp)
// Default = all shipped features ON. NN_PREDICT=<mask> overrides at boot;
// NN_PREDICT=0 turns every prediction feature off (guests fall back to
// authoritative-time sounds, the raw lockstep view, and per-tic correction).
static int32_t Net_PredictModeFromEnv(void)
{
    const char *e = getenv("NN_PREDICT");
    return (e && *e) ? Batoi(e) : (1|2|8|16);
}
int32_t g_netPredictMode = Net_PredictModeFromEnv();
// Forensic console dumps (MISMATCH/INPDUMP/SPAWNDUMP/RNGDUMP/STATDUMP) --
// default OFF: comparisons and auto-resync run regardless; the dump bursts
// correlated with renderer deaths on the bench. Soak enables for hunts.
// Native harnesses arm it with NN_FORENSICS=1 (browser uses the Web_ exports).
static int32_t Net_ForensicsFromEnv(void)
{
    const char *e = getenv("NN_FORENSICS");
    return (e && *e == '1') ? 1 : 0;
}
int32_t g_netForensics = Net_ForensicsFromEnv();
// Test-harness input injectors: set only by the Emscripten Web_Test* exports
// below, so they stay 0 in native builds -- but the consumer that reads them in
// the input-staging path is compiled for NETNATIVE too, so the storage must
// exist unconditionally (else the native link fails on these two symbols).
int32_t g_testFire  = 0;
int32_t g_testDrive = 0;
// Consume tic of our own most recent spawn (P_Dead spawn hook, respawns too):
// the pack applier ignores SELF health from packs stamped before it -- a stale
// in-flight pack carrying the pre-spawn extra=0 armed selfpain for a full
// 100-damage hit the instant the seat spawned ("presses open and immediately
// dies", live-reported).
int32_t g_netSelfSpawnTic = -1;

// Forensics: OUR OWN position watchdog. Called between the tic pipeline's
// phases (game.cpp); any >512-unit jump of the LOCAL seat between two calls
// names the segment that moved it -- built for the late-join hunt where the
// joiner's sim was yanked back to the seat point every tic by an unknown
// writer while every audited authority path checked out clean on paper.
void Net_SelfPosWatch(const char *tag)
{
    extern int32_t g_netForensics;
    if (!g_netForensics || numplayers < 2 || g_player[myconnectindex].ps == NULL)
        return;
    static vec3_t s_last;
    static int    s_valid;
    vec3_t const now = g_player[myconnectindex].ps->pos;
    if (s_valid)
    {
        int32_t const d = klabs(now.x - s_last.x) + klabs(now.y - s_last.y);
        if (d > 512)
            LOG_F(INFO, "[pw] %s moved %d -> (%d,%d) plc=%d", tag, d, now.x, now.y, movefifoplc);
    }
    s_last  = now;
    s_valid = 1;
}
#ifdef __EMSCRIPTEN__
extern "C" void Web_SetPredictMode(int mode)
{
    g_netPredictMode = mode;
    EM_ASM({ console.log('[eng] predictMode=' + $0); }, mode);
}
#endif
// One-line world-truth dump for pair probes: position drift, health/score
// parity. Two call paths: Web_NetProbe (JS, wall-time cadence) and the
// tic-aligned site in Net_GetSyncStat (forensics, movefifoplc&255==0) --
// tic-aligned lines carry the SAME plc on every peer, so the differ compares
// the same sim tic exactly. This is the OA-model acceptance instrument
// (there is no CRC verdict to read in stream mode). Defined on EVERY platform
// (browser logs to the console, native to the logger): the Net_GetSyncStat
// call site compiles unconditionally, and ld64 rejects the dangling reference
// even when forensics never turns on (Linux's --gc-sections only masked it).
extern "C" void Web_NetProbe(void)
{
    char line[640];
    int n = 0, i;
    n += Bsprintf(line + n, "[probe] plc=%d np=%d", movefifoplc, numplayers);
    TRAVERSE_CONNECT(i)
    {
        auto const ps = g_player[i].ps;
        if (ps == NULL || (unsigned)ps->i >= MAXSPRITES)
            continue;
        n += Bsprintf(line + n, " p%d[x=%d y=%d s=%d hp=%d fr=%d dd=%d z=%d cw=%d gw=%d]",
                      i, ps->pos.x, ps->pos.y, (int)ps->cursectnum,
                      (int)sprite[ps->i].extra, (int)ps->frag, (int)ps->dead_flag,
                      ps->pos.z, (int)ps->curr_weapon, (int)ps->gotweapon);
        if (n > (int)sizeof(line) - 96)
            break;
    }
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log(UTF8ToString($0)); }, line);
#else
    LOG_F(INFO, "%s", line);
#endif
}
#ifdef __EMSCRIPTEN__

extern "C" void Web_SetStreamMode(int on)
{
    g_netStreamMode = on ? 1 : 0;
    EM_ASM({ console.log('[eng] stream mode (state authority) ' + ($0 ? 'ON' : 'OFF -> legacy lockstep repair')); }, on);
}

extern "C" void Web_SetForensics(int on)
{
    g_netForensics = on;
    EM_ASM({ console.log('[eng] forensics=' + $0); }, on);
}
// The late-join snapshot is a savegame and carries NO premap spawn table. A
// fresh-process joiner never ran premap, so g_playerSpawnCnt was 0 there and
// Net_InsertLatePlayer silently refused to materialize the joiner ON ITS OWN
// SIM ONLY (veterans inserted it fine): an instant, permanent fork, plus the
// prediction replica initializing on the shadowed HOST player (live-reported
// as every ?join= freezing seconds after the seat). The transport ships the
// host's table alongside the snapshot; identical level data on every peer.
// TEST HOOKS (aim harness): nudge the local VIEW exactly like a mouse frame
// would, and read the view-vs-sim direction gap. The closed-loop staging must
// drive the sim onto the view within a couple of tics; the probe asserts it.
extern "C" void Web_TestAim(int davel, int dhorz)
{
    fix16_t a = fix16_sadd(predictedPlayer.q16ang, fix16_from_int(davel));
    while (a < 0)          a = fix16_sadd(a, F16(2048));
    while (a >= F16(2048)) a = fix16_ssub(a, F16(2048));
    predictedPlayer.q16ang   = a;
    predictedPlayer.q16horiz = fix16_clamp(fix16_sadd(predictedPlayer.q16horiz, fix16_from_int(dhorz)),
                                           F16(HORIZ_MIN), F16(HORIZ_MAX));
}
extern "C" int Web_AimGapAng(void)
{
    if (g_player[myconnectindex].ps == NULL) return 0;
    fix16_t d = fix16_ssub(predictedPlayer.q16ang, g_player[myconnectindex].ps->q16ang);
    while (d > F16(1024))  d = fix16_ssub(d, F16(2048));
    while (d < -F16(1024)) d = fix16_sadd(d, F16(2048));
    return fix16_to_int(d);
}
extern "C" int Web_AimGapHoriz(void)
{
    if (g_player[myconnectindex].ps == NULL) return 0;
    return fix16_to_int(fix16_ssub(predictedPlayer.q16horiz, g_player[myconnectindex].ps->q16horiz));
}
// TEST HOOKS (hit-registration harness): point the VIEW at the nearest sprite
// of a picnum, hold the trigger through the real input pipeline, and check
// the victim's fate in the consumed sim. With closed-loop aim staged, the
// shot every peer computes comes from this view -- a dead prop here plus a
// clean sync verdict IS cross-peer registration.
extern "C" int Web_AimAtNearestPic(int pic)
{
    auto const ps = g_player[myconnectindex].ps;
    if (ps == NULL || (unsigned)ps->cursectnum >= (unsigned)numsectors) return -1;
    int best = -1, bestAny = -1; int32_t bestd = INT32_MAX, bestdAny = INT32_MAX;
    for (int i = 0; i < MAXSPRITES; i++)
    {
        if (sprite[i].picnum != pic || sprite[i].statnum >= MAXSTATUS || sprite[i].xrepeat == 0)
            continue;
        if ((unsigned)sprite[i].sectnum >= (unsigned)numsectors)
            continue;
        int32_t const d = klabs(sprite[i].x - ps->pos.x) + klabs(sprite[i].y - ps->pos.y);
        if (d < bestdAny) { bestdAny = d; bestAny = i; }
        if (d < bestd && cansee(ps->pos.x, ps->pos.y, ps->pos.z, ps->cursectnum,
                                sprite[i].x, sprite[i].y, sprite[i].z - 2048, sprite[i].sectnum))
            { bestd = d; best = i; }
    }
    if (best < 0) { best = bestAny; bestd = bestdAny; }   // walk toward it (Web_TestDrive)
    if (best < 0) return -1;
    int32_t const dist = max(bestd, 64);
    predictedPlayer.q16ang   = fix16_from_int(getangle(sprite[best].x - ps->pos.x, sprite[best].y - ps->pos.y));
    int32_t const dz = (sprite[best].z - 2048) - ps->pos.z;
    predictedPlayer.q16horiz = fix16_clamp(fix16_from_int(100 - (int)(((int64_t)dz * 16) / dist)),
                                           F16(HORIZ_MIN), F16(HORIZ_MAX));
    return best;
}
extern "C" void Web_TestFire(int on) { g_testFire = on; }
extern "C" void Web_TestDrive(int on) { g_testDrive = on; }
extern "C" int Web_PicAlive(int idx, int pic)
{
    if ((unsigned)idx >= MAXSPRITES) return 0;
    return (sprite[idx].picnum == pic && sprite[idx].xrepeat > 0 && sprite[idx].statnum < MAXSTATUS) ? 1 : 0;
}

extern "C" const char *Net_GetSpawnTable(void)
{
    static char buf[MAXPLAYERS * 48 + 8];
    int n = 0;
    for (int i = 0; i < g_playerSpawnCnt && i < MAXPLAYERS; i++)
        n += Bsnprintf(buf + n, sizeof(buf) - n, "%d,%d,%d,%d,%d;",
                       g_playerSpawnPoints[i].x, g_playerSpawnPoints[i].y,
                       g_playerSpawnPoints[i].z, (int)g_playerSpawnPoints[i].sect,
                       (int)g_playerSpawnPoints[i].ang);
    return buf;
}
extern "C" void Net_SetSpawnTable(const char *s)
{
    if (!s || !*s)
        return;
    int cnt = 0;
    while (*s && cnt < MAXPLAYERS)
    {
        int x, y, z, sect, ang;
        if (sscanf(s, "%d,%d,%d,%d,%d", &x, &y, &z, &sect, &ang) != 5)
            break;
        auto &sp = g_playerSpawnPoints[cnt];
        sp.x    = x;
        sp.y    = y;
        sp.z    = z;
        sp.sect = (int16_t)sect;
        sp.ang  = (int16_t)ang;
        cnt++;
        const char *semi = strchr(s, ';');
        if (!semi)
            break;
        s = semi + 1;
    }
    if (cnt)
        g_playerSpawnCnt = cnt;
    initprintf("net: spawn table received (%d points)\n", cnt);
}
#endif

#ifdef __EMSCRIPTEN__
// TEST HOOK (harness only): deliberately fork THIS peer's world state, so the
// CRC watchdog -> host auto-resync healing path can be exercised in anger. A
// real desync (should any bug ever cause one) takes exactly this route: CRCs
// mismatch, the host pushes a snapshot, everyone reloads. NOTE: randomseed is
// NOT forkable here -- both peers deterministically reset it from
// ticrandomseed (always 0 on this track) inside the tic, which also means an
// RNG-stream desync is impossible by construction. Position is the real thing:
// it feeds physics and every later CRC.
extern "C" void Web_ForceDesync(void)
{
    auto const ps = g_player[myconnectindex].ps;
    if (ps == NULL || !(ps->gm & MODE_GAME))
        return;
    // Fork like a REAL desync: divergent-but-VALID state. The old wall/sector
    // teleport corrupted geometry outright -- under the targeted heal the
    // diverged peer keeps SIMMING until the snapshot lands (the old broadcast
    // stopped the world almost immediately), and clipmove over a degenerate
    // wall can spin the wasm main loop (soak: guest heartbeat died seconds
    // after the fork, before sav_begin ever arrived). randomseed is genuine
    // lockstep state (post-5l): every krand consumer diverges from the next
    // tic on, self-sustaining, and the world stays self-consistent -- exactly
    // the shape of an organic desync. The ps nudge makes it instant + visible
    // (cat 1 RNG + cat 2/7 positions flag on the next stamp).
    ps->pos.x += 256;
    if ((unsigned)ps->i < MAXSPRITES)
        sprite[ps->i].x += 256;
    randomseed ^= 0x5A5A;
    g_globalRandom ^= 0xA5;
    EM_ASM({ console.log('[eng] Web_ForceDesync: forked (psIsPredicted=' + $0 + ')'); },
           (ps == &predictedPlayer) ? 1 : 0);
}
#endif

// Called by the transports (JS via ccall / native directly) when the snapshot
// file is fully written. Consumed in menus.cpp at a safe frame point.
// plc = the sender's movefifoplc at save time (the catchup stream base);
// isJoin = barrier-free join/catchup (1) vs legacy resync broadcast (0).
extern "C" void Net_SnapshotReady(int seatMask, int plc, int isJoin)
{
    g_netSnapshotMask  = (uint32_t)seatMask;
    s_snapshotPlc      = plc;
    s_snapshotIsJoin   = isJoin;
    g_netSnapshotReady = 1;
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[eng] Net_SnapshotReady mask=' + $0 + ' plc=' + $1 + ' join=' + $2); },
           seatMask, plc, isJoin);
#endif
}

// Rotate a CHOSEN free-list index to the head so the next insertsprite() hands
// out exactly that index. The free chain's ORDER is not cross-peer stable
// (per-viewer churn perturbs it), so deterministic lockstep events that
// allocate sprites -- the SEAT insert above all -- were handed DIFFERENT
// indices on different peers (measured: i=700 vs 712, i=801 vs 813), after
// which every index-ordered sim walk diverges. Fixed per-slot indices from
// the TOP of the table (allocated organically only after ~16k live sprites)
// make the seat allocation a pure function of the slot number.
static int Net_RotateFreeSpriteToHead(int16_t idx)
{
    if ((unsigned)idx >= MAXSPRITES || sprite[idx].statnum != MAXSTATUS)
        return -1;
    if (headspritestat[MAXSTATUS] == idx)
        return 0;
    int16_t const prev = prevspritestat[idx];
    int16_t const next = nextspritestat[idx];
    if (prev >= 0) nextspritestat[prev] = next;
    if (next >= 0) prevspritestat[next] = prev;
    if (tailspritefree == idx)
        tailspritefree = prev;
    int16_t const ohead = headspritestat[MAXSTATUS];
    prevspritestat[idx] = -1;
    nextspritestat[idx] = ohead;
    if (ohead >= 0) prevspritestat[ohead] = idx;
    headspritestat[MAXSTATUS] = idx;
    return 0;
}

// Materialize a late joiner in the running level. Mirrors what level entry does
// for players 1+ (G_ResetAllPlayers: players are memcpys of a reference ps with
// identity/position fixups) plus the sprite insert. Under the barrier-free join
// EVERY peer runs this at the same consumed tic (Net_ApplyPendingJoins), so it
// must be bit-deterministic: the template is CONNECTHEAD's ps -- identical
// lockstep state on every peer -- never myconnectindex's (which differs).
// Seats waiting for their first spawn (press open/fire). The synthetic
// dead-pending body never ran the CON dying sequence, so the CON's own
// respawn keypress check can never fire for it -- P_Dead consults this mask
// and runs the same gametype-gated reset engine-side. The keypress is only
// visible where the seat's REAL input bits flow (the host, and the seat's
// own client): stream-mode bystanders get zero-filled columns for other
// guests, so they learn of the spawn via PACKET_TYPE_SEAT_SPAWNED instead.
extern uint32_t s_pendingSpawnMask;  // declared with the early statics (level reset zeroes it)
extern "C" int Net_PendingSpawnSeat(int playerNum)
{
    return (playerNum >= 0 && playerNum < MAXPLAYERS
            && (s_pendingSpawnMask & (1u << playerNum))) ? 1 : 0;
}
extern "C" void Net_PendingSpawnDone(int playerNum)
{
    if (playerNum >= 0 && playerNum < MAXPLAYERS)
        s_pendingSpawnMask &= ~(1u << playerNum);
}

// Host, stream mode: tell every guest a seat just spawned (join spawn-on-
// demand or CON respawn). A bystander's sim never reaches the spawn opcode
// for OTHER guests (zero-filled input columns), so until this telegram
// existed the seat stayed a corpse there until the next aligned softsnap --
// up to a minute of "the joiner is dead/frozen on my screen" (live-reported
// as "joining can desync another guest"). Carries no position: the receiver
// resets the seat in place (non-owner spawn hold) and the owner's stream
// places it within a beat.
void Net_BroadcastSeatSpawn(int playerNum)
{
    if (!g_netStreamMode || numplayers <= 1 || myconnectindex != connecthead
        || oldnet_predicting || (unsigned)playerNum >= MAXPLAYERS)
        return;

    uint8_t pk[2] = { PACKET_TYPE_SEAT_SPAWNED, (uint8_t)playerNum };
    int i;
    TRAVERSE_CONNECT(i)
        if (i != myconnectindex && !(g_netBotMask & (1 << i)))
            oldnet_sendpacket(i, pk, 2);
    LOG_F(INFO, "[seat] spawn telegram p=%d", playerNum);
}

void Net_InsertLatePlayer(int k)
{
    if ((unsigned)k >= MAXPLAYERS || g_playerSpawnCnt <= 0)
        return;

    G_MaybeAllocPlayer(k);

    auto &plr = g_player[k];
    Bmemcpy(plr.ps, g_player[connecthead].ps, sizeof(DukePlayer_t));
    Bmemset(plr.frags, 0, sizeof(plr.frags));

    // CLAIM THE SEAT'S PREMAP START SPRITE. The level premaps ud.multimode
    // seats (every JOINABLE seat, so mid-game joins can materialize), which
    // parks an APLAYER start sprite per unconnected seat at its spawn row --
    // statnum STAT_PLAYER, yvel = the seat number, extra 0. G_MovePlayers
    // reads yvel as the seat: the orphan SHADOWED the freshly inserted body,
    // and its extra=0 dead-branch copied the start-row position over the
    // seat's ps EVERY TIC on EVERY peer -- the live 3rd-seat "I am stuck in
    // one place, I can't move, however I can fire" (fire rides the guest
    // weapon report, position rides the stomped ps). owner >= 0 spares
    // holoduke decoys (also APLAYER+yvel on STAT_PLAYER, but owner < 0).
    // Deterministic: identical world and tic on every peer.
    for (int spr = headspritestat[STAT_PLAYER]; spr >= 0; )
    {
        int const nextspr = nextspritestat[spr];
        if (sprite[spr].picnum == APLAYER && (int)sprite[spr].yvel == k
            && sprite[spr].owner >= 0)
        {
            LOG_F(INFO, "[seat] seat %d claims its premap start: orphan sprite %d deleted", k, spr);
            A_DeleteSprite(spr);
        }
        spr = nextspr;
    }

    // Farthest row from the living: a late joiner must not materialize into
    // the fight pit (index-keyed rows concentrated on E1L1's start street).
    extern int G_PickFarSpawnRow(void);
    auto &spawn = g_playerSpawnPoints[G_PickFarSpawnRow()];
    // Deterministic seat index: slot k claims sprite MAXSPRITES-1-k on every
    // peer (see Net_RotateFreeSpriteToHead). Fallback to the organic freelist
    // only if the reserved index is somehow occupied -- loudly.
    if (Net_RotateFreeSpriteToHead((int16_t)(MAXSPRITES - 1 - k)) != 0)
        initprintf("net: seat %d reserved sprite %d unavailable -> organic freelist\n",
                   k, MAXSPRITES - 1 - k);
    int const i = A_InsertSprite(spawn.sect, spawn.x, spawn.y, spawn.z,
                                 APLAYER, 0, 0, 0, spawn.ang, 0, 0, 0, 10);

    auto &p = *plr.ps;
    p.i          = i;
    p.opos = p.pos = spawn.xyz;
    p.bobpos     = p.pos.xy;
    p.cursectnum = spawn.sect;
    p.oq16ang = p.q16ang = fix16_from_int(spawn.ang);
    p.q16horiz   = F16(100);
    p.q16horizoff = 0;
    p.vel        = { 0, 0, 0 };
    p.dead_flag  = 0;
    p.newowner   = -1;
    p.frag = p.fraggedself = 0;
    p.frag_ps    = k;
    p.gm         = MODE_GAME;
    p.spritezoffset = 38 << 8;  // PHEIGHT: G_MovePlayerSprite hangs the body below the eye

    // BODY INIT -- the exact premap P_ResetMultiPlayer block. A_InsertSprite
    // leaves extra at the EGS default (<=0) and repeats at 0: the seat shipped
    // a size-zero, dead-to-every-check body, so the joiner had no HUD weapon
    // (weapon draw gates on sprite extra > 0), sat eye-at-floor, and the
    // player pack re-imposed the dead body 3x/sec (live: "did not spawn
    // fully, he hasn't got a gun and is in the middle of the ground").
    auto &s = sprite[i];
    s.yvel     = k;    // classic contract: a player sprite's yvel is its player index
    s.clipdist = 64;
    s.owner    = i;
    s.pal      = p.palookup;
    s.shade    = -12;
    s.xoffset  = 0;
    s.xrepeat  = 42;
    s.yrepeat  = 36;
    // LATE JOINERS SPAWN ON DEMAND (user 2026-08-16: "they should need to
    // press open to spawn -- and only if the game type allows it"): the seat
    // lands DEAD-PENDING -- no visible body, zero health, dead_flag armed --
    // and the engine's own death machinery takes it from there: P_Dead runs
    // the wait state, the APLAYER CON accepts the open/fire press, and
    // CON_RESETPLAYER's MP branch applies the gametype gates (LMS lives,
    // no-respawn co-op) before P_ResetMultiPlayer spawns the body (which
    // restores cstat 257 + full health + dead_flag 0). Every peer consumes
    // the same input column, so the spawn decision replicates; the exact
    // position converges via POS_REPORT like any ordinary respawn.
    s.cstat    = 32768;
    s.extra    = 0;
    p.dead_flag  = 1;
    p.last_extra = p.max_player_health;
    p.inv_amount[GET_SHIELD] = g_startArmorAmount;

    P_ResetWeapons(k);   // includes the MP shotgun arena loadout
    P_ResetInventory(k);

    s_pendingSpawnMask |= (1u << k);
    LOG_F(INFO, "[seat] player %d inserted PENDING (press open to spawn)", k);
}

// RECEIVER: seat the mask, load LATEJOIN_SAVE, restore the LOCAL identity (the
// snapshot was written by the host). 0 on success. Two modes (s_snapshotIsJoin):
//   JOIN    -- barrier-free: we are the joiner, NOT in the roster; we adopt the
//              host's absolute timeline at the snapshot tic, spectate, and let
//              the M2S catchup stream carry us to live. The seat itself happens
//              deterministically at the announced joinTic (Net_ApplyPendingJoins).
//   LEGACY  -- resync broadcast: every veteran reloads and rendezvouses at the
//              barrier exactly as before.
int Net_ApplyLateJoinSnapshot(void)
{
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[eng] Apply: seat+load starting (join=' + $0 + ' plc=' + $1 + ')'); },
           s_snapshotIsJoin, s_snapshotPlc);
#endif
    int const myIdx  = myconnectindex;
    int const myPeek = screenpeek;
    bool const joinMode = (s_snapshotIsJoin != 0);

    // Seat BEFORE loading: G_LoadPlayer rejects h.numplayers != ud.multimode.
    uint32_t const seatBits = g_netSnapshotMask & 0xffffu;
    // TARGETED HEAL: a "join" snapshot whose roster contains US. We are a
    // seated player being handed fresh authoritative state: same catchup
    // stream, but our ps comes from the save (no host shadow), we keep our
    // own view, and there is no seat coming -- we self-resume at the live
    // edge (Net_CheckHealResume). A real joiner is NEVER in the mask.
    bool const healMode = joinMode && ((seatBits >> myIdx) & 1);
    // Adopt the sender's move epoch EXACTLY (a resync host bumped before sending;
    // a join host did NOT -- the joiner slots into the RUNNING generation).
    g_netMoveEpoch = (uint8_t)((g_netSnapshotMask >> 16) & 0xff);
    for (int k = 0; k < MAXPLAYERS && k < 16; k++)
        g_player[k].connected = (seatBits >> k) & 1;
    if (!joinMode)
        g_player[myIdx].connected = 1;   // legacy: we are part of the reloading roster
    Net_SeatLateJoiners(); // mask already applied; sets quitflags + rebuilds the chain
    ud.multimode            = numplayers;
    g_mostConcurrentPlayers = ud.multimode;

    if (joinMode)
        g_netJoinCatchup = 1;   // BEFORE the load: bypasses the interior barrier
                                // G_LoadPlayer reaches via Net_WaitForServer

    savebrief_t sv;
    Bstrcpy(sv.path, LATEJOIN_SAVE);
    sv.isExt = 0;

    int const r = G_LoadPlayer(sv);

#ifdef __EMSCRIPTEN__
    // nsprt canary: the host's world had a specific sprite count at save time
    // -- if ours differs right here, the savegame restore itself leaves local
    // stragglers; if it matches here but skews by seat time, catchup replay
    // diverges. (pair24: host 586 vs guest 598 at the seat -> index skew 700
    // vs 712 -> permanent index-order fork no heal can outrun.)
    EM_ASM({ console.log('[eng] Apply: load r=' + $0 + ' np=' + $1 + ' nsect=' + $2 + ' nsprt=' + $3 + ' fh=' + $4); },
           r, numplayers, numsectors, (int)Numsprites, (int)headspritestat[MAXSTATUS]);
    // MISC-timer fidelity canary (load side) -- see the save-side twin.
    {
        extern int32_t g_netForensics;
        if (g_netForensics)
        {
            int shown = 0;
            for (int i = headspritestat[5]; i >= 0 && shown < 16; i = nextspritestat[i], shown++)
                EM_ASM({ console.log('[misc] i=' + $0 + ' pic=' + $1 + ' t0=' + $2 + ' t1=' + $3 + ' xr=' + $4); },
                       i, sprite[i].picnum, actor[i].t_data[0], actor[i].t_data[1], sprite[i].xrepeat);
        }
    }
#endif

    // The snapshot carries the HOST's view of every per-player struct; OUR identity
    // is local state and must survive the load.
    myconnectindex = myIdx;
    // Joiner: spectate the host while syncing. Healing guest: keep own view
    // (our player exists in the snapshot; the world fast-forwards around it).
    screenpeek     = (joinMode && !healMode) ? connecthead : myPeek;
    Net_SeatLateJoiners(); // re-assert chain + quitflags over whatever the load restored

    if (joinMode && r == 0)
    {
        // Continue the HOST's absolute timeline from the snapshot tic: the
        // self-contained M2S stream repairs everything from here to live, and
        // our empty S2Ms ack the progress that paces it.
        g_netMoveEpoch = (uint8_t)((g_netSnapshotMask >> 16) & 0xff);  // in case the load path reset it
        movefifoplc = movefifosendplc = s_snapshotPlc;
        for (int k = 0; k < MAXPLAYERS; k++)
        {
            g_player[k].movefifoend  = s_snapshotPlc;
            g_player[k].lastSyncTick = s_snapshotPlc;
        }
        s_ackOfMyInput  = 0;
        g_netSampleHead = 0;            // nothing staged until the seat tic
        predictfifoplc  = s_snapshotPlc;
        {
            extern int32_t g_worldExecs;   // align the epoch-relative world-step count
            g_worldExecs = s_snapshotPlc;
        }
        if (!healMode)
        {
            // Our slot is NOT in the world yet; input/HUD paths still dereference our
            // ps. Shadow the host's player until the deterministic seat replaces it.
            G_MaybeAllocPlayer(myIdx);
            if (g_player[connecthead].ps != NULL)
                Bmemcpy(g_player[myIdx].ps, g_player[connecthead].ps, sizeof(DukePlayer_t));
            g_player[myIdx].ps->gm = MODE_GAME;
        }
        else
            s_healBasePlc = s_snapshotPlc;   // arms the self-resume check
        ready2send = 1;                 // pump ON: the catchup acks must flow
    }
    else if (joinMode)
        g_netJoinCatchup = 0;           // failed load: back out of catchup mode
    else
    {
        // Deterministic barrier slate: the load restored SAVED readyflags (zeros)
        // over any READY that arrived mid-load. Zero everything; our own flag
        // increments at barrier entry and the master's echo re-raises its flag.
        for (int k = 0; k < MAXPLAYERS; k++)
            g_player[k].playerreadyflag = 0;
    }

    Net_ResetSyncCheck(); // fresh authoritative state -> any recorded divergence is history

    if (r == 0)
        initprintf("net: snapshot applied (%d players, I am %d, join=%d, tic=%d)\n",
                   numplayers, myconnectindex, (int)joinMode, s_snapshotPlc);
    else
        initprintf("net: snapshot load FAILED (%d)\n", r);

    return r;
}

// Seat every queued late joiner (menus.cpp consumer calls this at a safe frame
// point, right before relaunching the current map).
// LAUNCH VIA SNAPSHOT (menus.cpp): pull every human guest out of the entry
// roster and queue them on the late-join pipeline -- the host enters with the
// CPU seats only, then streams each guest the canonical entry world.
void Net_DemoteGuestsToSnapshotEntry(void)
{
    if (myconnectindex != connecthead)
        return;
    for (int k = 0; k < MAXPLAYERS && k < 16; k++)
        if (k != myconnectindex && g_player[k].connected && !(g_netBotMask & (1 << k)))
        {
            g_player[k].connected = 0;
            g_netLateJoinMask |= (1 << k);
        }
    Net_RebuildConnectChain();
    ud.multimode            = numplayers;
    g_mostConcurrentPlayers = ud.multimode;
}

void Net_SeatLateJoiners(void)
{
    for (int k = 0; k < MAXPLAYERS; k++)
        if (g_netLateJoinMask & (1 << k))
            g_player[k].connected = 1;
    g_netLateJoinMask = 0;
    Net_RebuildConnectChain();
}

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)

// Deterministic seat: every peer -- veterans, host, and the joiner itself --
// inserts the announced player the moment its sim reaches the joinTic, exactly
// like Net_ApplyPendingDrops excises at the goneTic. Runs at the top of
// G_MoveLoop, before the gate and before prediction.
void Net_ApplyPendingJoins(void)
{
    if (numplayers < 1 || g_player[myconnectindex].ps == NULL
        || !(g_player[myconnectindex].ps->gm & MODE_GAME))
        return;


    for (int k = 0; k < MAXPLAYERS; k++)
    {
        if (s_joinTic[k] < 0 || g_player[k].connected || movefifoplc < s_joinTic[k])
            continue;
        // The latest membership event wins: a drop AFTER the seat means the
        // player is gone -- without this, the excise clears `connected` and the
        // seat re-applied every tic forever (soak-caught: joinApplied spam +
        // ghost churn after a dead joiner was silence-dropped).
        if (s_goneTic[k] >= 0 && s_goneTic[k] >= s_joinTic[k])
            continue;
        if (movefifoplc > s_joinTic[k])
            // The announcement should always land tics ahead (it rides every
            // M2S until consumed); arriving late means we insert late and our
            // world diverges for the interim -- the CRC watchdog + resync heal
            // it, but log the anomaly loudly.
            initprintf("net: JOIN APPLIED LATE for slot %d (tic %d, plc %d)\n",
                       k, s_joinTic[k], movefifoplc);

        Net_InsertLatePlayer(k);
        g_player[k].connected      = 1;
        g_player[k].playerquitflag = 1;
        Net_RebuildConnectChain();
        ud.multimode            = numplayers;
        g_mostConcurrentPlayers = max(g_mostConcurrentPlayers, numplayers);

        if (k == myconnectindex)
        {
#ifdef __EMSCRIPTEN__
            // Wedge forensics: dump the next clipshape initindex inputs (the
            // post-seat hitscan clip walk hard-looped; see clip.cpp).
            { extern int32_t g_clipLogBudget; g_clipLogBudget = 40; }
#endif
            // I am the joiner: leave spectator mode and start staging real
            // inputs AT THE LIVE EDGE, not the seat tic. The host zero-fills
            // this seat's column from the announcement on (joinerNoReal
            // synthesis: the match never stalls on a joiner) and its send
            // cursor runs AHEAD of the sims -- records sampled from the seat
            // tic arrive forever behind the fill and every one is dup-dropped:
            // the seat was PERMANENTLY zero-input on the host ("I'm stuck in
            // one place, I can't move but I can fire; they don't have my
            // state" -- live-reported 3rd seat, 2026-08-16). movefifosendplc
            // tracks the host's send cursor within wire latency; sampling just
            // ahead of it lands our first real records beyond the fill, which
            // clears joinerNoReal and hands the column back to us.
            g_netJoinCatchup = 0;
            // WAITING STATE (live directive: "make the new guest join in a
            // waiting state"): the seat's own camera is a synthetic corpse at
            // ankle height in a far spawn row -- as a first-person view it
            // reads as a broken death screen. Spectate the host's seat until
            // the spawn press; the P_Dead spawn hook flips the view back.
            screenpeek       = connecthead;
            g_netSampleHead  = max(movefifoplc, movefifosendplc + 4);
            s_ackOfMyInput   = g_netSampleHead;
            g_seatDiagUntil  = timerGetTicks() + 40000;
            // Spawn-on-demand seat: tell the player what the black screen
            // wants from them (the seat is dead-pending by design).
            // QUOTE_RESERVED3 is NOT allocated unless some CON defined it --
            // writing it blind was a null Bstrcpy that killed the engine the
            // instant the seat landed (every post-seat stall since the
            // redesign was this crash).
            if (apStrings[QUOTE_RESERVED3] == NULL)
                C_AllocQuote(QUOTE_RESERVED3);
            if (apStrings[QUOTE_RESERVED3] != NULL)
            {
                Bstrcpy(apStrings[QUOTE_RESERVED3], "SPECTATING - PRESS OPEN TO SPAWN");
                // The HUD draws quotes from the SCREENPEEK player -- while
                // spectating that is the host's replica, so the line must
                // land on ITS fta/ftq (cosmetic-only local fields).
                if (g_player[screenpeek].ps != NULL)
                    P_DoQuote(QUOTE_RESERVED3, g_player[screenpeek].ps);
            }
            // The pre-seat sampler never ran, so the lag/jitter bookkeeping
            // accumulated garbage (localNow was -1): reset it or the first
            // timer-nudge block would jerk totalclock by a bogus offset.
            for (int p2 = 0; p2 < MAXPLAYERS; p2++)
                g_player[p2].myminlag = 0x7fffffff;
            mymaxlag = otherminlag = 0;
            Net_InitializePrediction();
            // The catchup consume filled the compare ring at fast-forward
            // phase: those slots hash a DIFFERENT moment than the host's
            // real-time fills, so post-seat compares read them as divergence
            // (forensics-proven false positive: STATDUMP parity while cat-18
            // "mismatched"). Restart the compare window at the seat.
            Net_ResetSyncCheck();
        }
#ifdef __EMSCRIPTEN__
        // i= is the DETERMINISM CANARY: the seat inserts this player's sprite
        // on every peer at the same tic -- if the allocated index differs
        // across peers (freelist order skew after a snapshot load), every
        // index-ordered sim walk diverges from that moment on.
        EM_ASM({ console.log('[eng] joinApplied p=' + $0 + ' tic=' + $1 + ' np=' + $2 + ' me=' + $3
                 + ' i=' + $4 + ' nsprt=' + $5); },
               k, movefifoplc, numplayers, (int)(k == myconnectindex),
               (int)g_player[k].ps->i, (int)Numsprites);
#else
        initprintf("net: player %d seated at tic %d (%d players, sprite %d)\n",
                   k, movefifoplc, numplayers, (int)g_player[k].ps->i);
#endif
    }
}

// A join in flight (snapshot streaming, catchup, or seat announced but not yet
// crossed)? The legacy auto-resync broadcast must WAIT for it: its snapshot
// roster would exclude the half-seated joiner and fork the connect chains.
int Net_JoinFlowActive(void)
{
    if (s_joinFlowSlot >= 0)
        return 1;
    for (int k = 0; k < MAXPLAYERS; k++)
        if (s_joinTic[k] >= 0 && !g_player[k].connected)
            return 1;
    return 0;
}

// HOST: barrier-free join state machine, driven once per frame from the
// menus.cpp NETMENU consumer. The match NEVER pauses: snapshot -> targeted
// stream -> watch the joiner's acks -> stamp the seat tic into the directory.
void netmenu_send_snapshot_to(int seatMask, int slot, int plc, int isJoin);  // menus.cpp

void Net_HostJoinFlow(void)
{
    // numplayers >= 2 only: a solo host has no live MP timeline to stream (the
    // menus.cpp consumer routes that case through the classic barrier join).
    if (numplayers < 2 || myconnectindex != connecthead || g_player[myconnectindex].ps == NULL
        || !(g_player[myconnectindex].ps->gm & MODE_GAME))
        return;

    int32_t const now = (int32_t)totalclock;
    if (s_joinFlowClock > now)
        s_joinFlowClock = now;   // totalclock reset

    if (s_joinFlowSlot >= 0)
    {
        int const k = s_joinFlowSlot;
        if (!s_joinFlowIsHeal && g_player[k].connected)   // insert applied on our own sim: flow done
        {
            s_joinFlowSlot = -1;
            return;
        }
        if (s_joinFlowIsHeal && !g_player[k].connected)
        {
            // The heal target dropped mid-cure (peer-down/quit excised it):
            // nothing left to heal. Latches were for this peer; clear them.
            s_joinFlowSlot = -1;
            s_joinFlowIsHeal = 0;
            g_netDesyncReporters = 0;
            Net_ResetSyncCheck();
            return;
        }
        if (!s_joinFlowIsHeal && s_joinTic[k] >= 0)   // stamped; every sim seats when it crosses the tic
            return;

        int32_t const gap = movefifosendplc - s_slaveAck[k];
        {
            // Host-side join-flow heartbeat (~2s), the other half of the
            // joiner's [join] catchup beat.
            static int32_t nextBeat;
            if (now - nextBeat >= 0)
            {
                nextBeat = now + 240;
                LOG_F(INFO, "[join] flow slot=%d ack=%d base=%d send=%d gap=%d tries=%d",
                      k, s_slaveAck[k], s_joinFlowBase, movefifosendplc, gap, s_joinFlowTries);
            }
        }
        if (s_slaveAck[k] > s_joinFlowBase && gap <= 8)
        {
            if (s_joinFlowIsHeal)
            {
                // Healed guest is back at the live edge (it self-resumes at its
                // end -- no seat: its membership never changed). Clear our own
                // divergence latches NOW: everything they recorded described
                // the world this snapshot just replaced.
                initprintf("net: heal complete for slot %d (gap %d)\n", k, gap);
#ifdef __EMSCRIPTEN__
                EM_ASM({ console.log('[eng] healFlow complete p=' + $0); }, k);
#endif
                s_joinFlowSlot = -1;
                s_joinFlowIsHeal = 0;
                // Stale reports from before the guest's apply may still be in
                // flight; drop ALL reporter bits -- a peer that is genuinely
                // still diverged re-reports within seconds and re-targets.
                g_netDesyncReporters = 0;
                Net_ResetSyncCheck();
                return;
            }
            // Caught up FOR REAL (acks advanced past the snapshot base AND
            // near-live): seat at the aggregation head plus a margin every peer
            // will cross AFTER the announcement (it rides every M2S packet).
            int32_t const tic = movefifosendplc + NET_JOIN_MARGIN;
            Net_ScheduleJoin(k, tic);
            s_joinAnnounceUntil[k] = now + NET_JOIN_ANNOUNCE;
            s_joinAwaitReal |= (1 << k);
            initprintf("net: joiner %d caught up (gap %d) -> seat at tic %d\n", k, gap, tic);
            // MIN-PLAYERS yield: a human raised the head count over the host's
            // floor -> the newest CPU seat gives way, through the SAME drop
            // boundary a quitting human rides (announced on every M2S packet;
            // every sim, this joiner included, unseats it at the same tic).
            if (g_netBotMask)
            {
                int total = 1;   // the joiner, connected once it crosses `tic`
                for (int i = 0; i < MAXPLAYERS && i < 16; i++)
                    if (g_player[i].connected)
                        total++;
                if (total > g_netMinPlayers)
                {
                    // Prefer sacrificing a HARD-TRAPPED bot (the E1L1 vent
                    // spawn pins one deterministically): the joining human
                    // inherits the functional roster, not the ghost.
                    int pick = -1;
                    for (int i = 15; i >= 0; i--)
                        if ((g_netBotMask & (1 << i)) && g_player[i].connected)
                        {
                            if (pick < 0)
                                pick = i;
                            if (s_botTrapTics[i] > 104 || s_botTrapRounds[i] > 0)
                            {
                                pick = i;
                                break;
                            }
                        }
                    if (pick >= 0)
                    {
                        initprintf("net: CPU seat %d yields to the joining player\n", pick);
                        Net_ScheduleDrop(pick, "seat yielded to a joining player");
                    }
                }
            }
            return;
        }
        if (now - s_joinFlowClock > NET_JOIN_RETRY && gap > NET_JOIN_RING_MAX)
        {
            // Too slow for the 256-tic ring (cold art/texture caches). Each
            // retry re-bases the stream on a FRESH snapshot; the joiner gets
            // warmer every pass. Persistent failure -> kick -- but ONLY count
            // a try once the joiner has acked at all: a browser seat can
            // spend 20s+ just booting the engine (bigger GRPs boot slower),
            // and exhausting the try budget during boot kicked live joiners
            // ("immediately desyncs and disconnects"). While it has never
            // acked, re-base patiently; the transport peer-down path still
            // reaps genuinely dead joiners.
            bool const everAcked = (s_slaveAck[k] > s_joinFlowBase);
            if (!everAcked)
                s_joinFlowTries = 0;
            if (everAcked && ++s_joinFlowTries >= NET_JOIN_TRIES)
            {
                initprintf("net: %s %d cannot catch up -> kick\n",
                           s_joinFlowIsHeal ? "heal target" : "joiner", k);
                net_kick(k);
                s_joinFlowSlot = -1;
                if (s_joinFlowIsHeal)
                {
                    s_joinFlowIsHeal = 0;
                    g_netDesyncReporters = 0;
                    Net_ResetSyncCheck();   // the diverged copy left with the kick
                }
                return;
            }
            if (Net_SaveLateJoinSnapshot() == 0)
            {
                int seatMask = 0;
                for (int i = 0; i < MAXPLAYERS && i < 16; i++)
                    if (g_player[i].connected)
                        seatMask |= (1 << i);
                seatMask |= ((int)g_netMoveEpoch) << 16;
                s_joinFlowClock = now;
                s_joinFlowBase  = movefifoplc;
                s_slaveAck[k]   = movefifoplc;
                if (s_joinFlowIsHeal)
                    s_healAckFence = 0;   // fresh apply expected: re-fence
                netmenu_send_snapshot_to(seatMask, k, movefifoplc, 1);
                initprintf("net: %s retry %d for slot %d (snapshot at tic %d)\n",
                           s_joinFlowIsHeal ? "heal" : "join", s_joinFlowTries, k, movefifoplc);
            }
        }
        return;
    }

    if (!g_netLateJoinMask)
        return;

    int k = -1;
    for (int i = 0; i < MAXPLAYERS; i++)
        if (g_netLateJoinMask & (1 << i)) { k = i; break; }
    g_netLateJoinMask &= ~(1 << k);
    if (g_player[k].connected)
        return;   // stale queue entry

    if (Net_SaveLateJoinSnapshot() != 0)
    {
        initprintf("net: join snapshot save FAILED for slot %d\n", k);
        return;
    }
    int seatMask = 0;
    for (int i = 0; i < MAXPLAYERS && i < 16; i++)
        if (g_player[i].connected)
            seatMask |= (1 << i);
    // The joiner adopts the CURRENT epoch: no bump, veterans entirely untouched.
    seatMask |= ((int)g_netMoveEpoch) << 16;

    s_joinFlowSlot  = k;
    s_joinFlowIsHeal = 0;
    s_joinFlowClock = now;
    s_joinFlowTries = 0;
    s_joinTic[k]    = -1;
    s_joinFlowBase  = movefifoplc;
    s_slaveAck[k]   = movefifoplc;           // stream base = snapshot tic
    g_player[k].movefifoend = 0;             // no records until the seat
    netmenu_send_snapshot_to(seatMask, k, movefifoplc, 1);
    initprintf("net: barrier-free join started for slot %d (snapshot at tic %d)\n", k, movefifoplc);
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[eng] joinFlow start p=' + $0 + ' plc=' + $1); }, k, movefifoplc);
#endif
}

// ── Soft correction (the first rung of the divergence ladder) ────────────────
// The user's contract: "not perfect sync -- periodic correction so positions/
// health are correct", and NEVER a map reload for drift. On a divergence latch
// the host first pushes an IN-PLACE correction to the diverged guest: RNG
// seeds + every player's position/velocity/angles/health, applied without
// touching the level (no G_LoadPlayer, no texture churn, sub-second). RNG
// convergence makes future krand-driven behavior line up prospectively;
// residual world drift either fades or keeps latching, and only REPEATED
// failures escalate to the full snapshot heal (the reload becomes the rare
// last resort instead of the first response).
enum
{
    NET_SOFT_STRIKES     = 4,      // soft attempts before escalating to a reload heal
    NET_SOFT_DECAY_TICKS = 14400,  // strike counter forgets after ~2min of peace
};

void Net_SendStateSnap(int k)
{
    if (myconnectindex != connecthead || (unsigned)k >= MAXPLAYERS || !g_player[k].connected)
        return;

    int j = 0;
    packbuf[j++] = PACKET_TYPE_STATE_SNAP;
    int const countPos = j++;
    // TIC STAMP: state captured with movefifoplc tics consumed. The receiver
    // applies at EXACTLY this consume count -- applied at any other tic, the
    // "correction" itself installs tics of position/RNG offset and the ladder
    // re-triggers forever (the guest teleports every few seconds).
    B_BUF32(&packbuf[j], movefifoplc);    j += 4;
    B_BUF32(&packbuf[j], randomseed);     j += 4;
    B_BUF32(&packbuf[j], g_globalRandom); j += 4;

    int n = 0, i;
    TRAVERSE_CONNECT(i)
    {
        auto const ps = g_player[i].ps;
        if (ps == NULL || (unsigned)ps->i >= MAXSPRITES)
            continue;
        packbuf[j++] = (char)i;
        B_BUF32(&packbuf[j], ps->pos.x);   j += 4;
        B_BUF32(&packbuf[j], ps->pos.y);   j += 4;
        B_BUF32(&packbuf[j], ps->pos.z);   j += 4;
        B_BUF32(&packbuf[j], ps->opos.x);  j += 4;
        B_BUF32(&packbuf[j], ps->opos.y);  j += 4;
        B_BUF32(&packbuf[j], ps->opos.z);  j += 4;
        B_BUF32(&packbuf[j], ps->bobpos.x); j += 4;
        B_BUF32(&packbuf[j], ps->bobpos.y); j += 4;
        B_BUF32(&packbuf[j], ps->vel.x);   j += 4;
        B_BUF32(&packbuf[j], ps->vel.y);   j += 4;
        B_BUF32(&packbuf[j], ps->vel.z);   j += 4;
        B_BUF32(&packbuf[j], (int32_t)ps->q16ang);   j += 4;
        B_BUF32(&packbuf[j], (int32_t)ps->q16horiz); j += 4;
        B_BUF32(&packbuf[j], sprite[ps->i].x); j += 4;
        B_BUF32(&packbuf[j], sprite[ps->i].y); j += 4;
        B_BUF32(&packbuf[j], sprite[ps->i].z); j += 4;
        B_BUF16(&packbuf[j], (int16_t)sprite[ps->i].extra); j += 2;
        B_BUF16(&packbuf[j], ps->cursectnum); j += 2;
        // Host-truth score/death state: a guest's local sim rolls its own RNG
        // for hits, so its kill ledger drifts -- the pack is the scoreboard.
        B_BUF16(&packbuf[j], ps->frag);         j += 2;
        B_BUF16(&packbuf[j], ps->fraggedself);  j += 2;
        B_BUF16(&packbuf[j], ps->dead_flag);    j += 2;
        n++;
    }
    packbuf[countPos] = (char)n;
    // ANIMWALL TAG PHASES: the forcefield animator's tag reset is RNG-coupled
    // (tag = 128<<(krand()&3)), so ONE transient stream fork makes every
    // wall's phase absorb different random values -- and a soft snap that
    // realigns randomseed but NOT the tags re-forks within a tic (wall-level
    // forensics: same-tic krand windows on walls {1590,1570} vs {1574}). The
    // phases ride along; the correction finally corrects the thing that was
    // re-breaking it.
    {
        int const awc = min<int>(g_animWallCnt, 64);
        packbuf[j++] = (char)awc;
        for (int aw = 0; aw < awc; aw++)
        {
            B_BUF16(&packbuf[j], (int16_t)animwall[aw].tag);
            j += 2;
        }
    }
    oldnet_sendpacket(k, (unsigned char *)packbuf, j);
    // Stream mode sends this ~3x/sec per guest: prints are forensics-only there.
    // LOG_F, not initprintf: initputs is a no-op without the startup window, so
    // the pack SIZE (the 16-seat capacity datum) was invisible to native
    // harness logs -- exactly how the 768B receive cap stayed silent.
    if (!g_netStreamMode || g_netForensics)
    {
        LOG_F(INFO, "net: soft state snap -> slot %d (%d players, %d bytes, tic %d)", k, n, j, movefifoplc);
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[eng] softsnap sent p=' + $0 + ' bytes=' + $1 + ' tic=' + $2); }, k, j, movefifoplc);
#endif
    }
}

// Consume a stashed tic-stamped soft snap. Called at the top of EVERY consumed
// tic (G_DoMoveThings, before the input read): when our consume count equals
// the stamp, both sims have executed exactly the same tics, so the host's
// captured state is bit-appropriate NOW -- positions, velocities and RNG land
// with zero offset and the sims evolve identically from here.
// ── Remote-seat smoothing (stream mode, guest side) ─────────────────────────
// Packs are TARGETS, not teleports. Hard-applying pos/opos at every 3Hz pack
// was a structural sawtooth: the local replay drifts for ~333ms, then the
// remote snaps to host truth -- no remote player could ever WALK A STRAIGHT
// LINE on a guest's screen (the user's exact words), on any build, no matter
// what the bot brain did, and a 15s-cadence parity probe could never see it.
// Each consumed tic pulls remotes 5/16 of the way to target (converges ~90%
// in 6 tics, well inside the pack interval), slews facing at a bounded rate,
// and reserves the hard snap for teleport-scale error (respawns, warps).
static vec3_t  s_rsPos[MAXPLAYERS], s_rsSpr[MAXPLAYERS];
static vec3_t  s_rsVel[MAXPLAYERS];
static fix16_t s_rsAng[MAXPLAYERS], s_rsHoriz[MAXPLAYERS];
static int16_t s_rsSect[MAXPLAYERS];
static int8_t  s_rsHave[MAXPLAYERS];
static int32_t s_rsLastTic[MAXPLAYERS];   // tic a remote seat last got a pack
// On-screen net-debug counters (drawn by G_DisplayRest for MP): the USER's
// real session is the only place the "bots barely move" repros, and localhost
// headless can't see it -- so measure transport delivery where it lives.
int32_t g_netDbgPackN = 0, g_netDbgSprN = 0, g_netDbgHitN = 0;
// ── CLIENT-AUTHORITATIVE HITSCAN ────────────────────────────────────────────
// User directive (2026-08-10): "the hitscan should be client side, not host
// side ... the lag is 1ms, so the problem is not lag, it's where you are
// calculating it." Correct diagnosis: at ~1ms the guest's local world matches
// the host's, but the host reproduces the guest's AIM only by integrating the
// avel it received (a lossy copy), so the host's authoritative re-fire points
// a hair off and MISSES a shot the guest saw connect -- then the sprite stream
// reverts the guest's honest local hit. Net effect: "I shot the bot and it
// barely took damage." Fix: the guest is the authority for damage IT deals.
// A_IncurDamage (actors.cpp) calls Net_ClientReportHit when a guest's own shot
// lands locally; the host applies it verbatim and drops the phantom damage it
// would have computed from the same shot (no double-count, no wrong-aim miss).

// GUEST -> HOST: report a hit this client resolved locally against its OWN world
// with its EXACT aim (favor-the-shooter). ONE unified protocol; kind selects the
// victim namespace so players AND monsters ride the SAME wire (native and wasm
// both speak it -- the block that calls this is compiled on both):
//   kind 0 = PLAYER victim  (victim is a player/bot SEAT index, < MAXPLAYERS)
//   kind 1 = ENEMY  victim  (victim is a live enemy SPRITE index, < MAXSPRITES)
void Net_ClientReportHit(int kind, int victim, int damage, int weaponPic)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex == connecthead)
        return;
    if (damage <= 0)
        return;
    if (kind == 0 ? ((unsigned)victim >= MAXPLAYERS) : ((unsigned)victim >= MAXSPRITES))
        return;
    uint8_t buf[8];
    buf[0] = PACKET_TYPE_HIT_REPORT;
    buf[1] = (uint8_t)kind;
    buf[2] = (uint8_t)(victim & 0xff);
    buf[3] = (uint8_t)((victim >> 8) & 0xff);
    buf[4] = (uint8_t)(damage & 0xff);
    buf[5] = (uint8_t)((damage >> 8) & 0xff);
    buf[6] = (uint8_t)(weaponPic & 0xff);
    buf[7] = (uint8_t)((weaponPic >> 8) & 0xff);
    oldnet_sendpacket(connecthead, buf, 8);
    { extern int32_t g_netDbgHitN; g_netDbgHitN++; }   // guest tally: reports SENT
#ifdef __EMSCRIPTEN__
    if (g_netForensics)
        EM_ASM({ console.log('[hitrep] SENT kind=' + $0 + ' victim=' + $1 + ' dmg=' + $2 + ' pic=' + $3); },
               kind, victim, damage, weaponPic);
#endif
}

// GUEST -> HOST: report a MONSTER hit by POSITION. Same PACKET_TYPE_HIT_REPORT,
// kind 1, but 20 bytes -- it carries the hit sprite's x/y/z so the host can find
// the LIVE enemy at that spot. The sprite index is only a hint (used iff the host
// happens to number that enemy the same). This is THE fix for "I killed it but the
// host wasn't aware": guest and host index the same monster differently.
void Net_ClientReportEnemyHit(int idxHint, int x, int y, int z, int damage, int weaponPic)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex == connecthead)
        return;
    if (damage <= 0)
        return;
    uint8_t buf[20];
    buf[0] = PACKET_TYPE_HIT_REPORT;
    buf[1] = 1;                                  // kind 1 = enemy
    buf[2] = (uint8_t)(idxHint & 0xff);
    buf[3] = (uint8_t)((idxHint >> 8) & 0xff);
    buf[4] = (uint8_t)(damage & 0xff);
    buf[5] = (uint8_t)((damage >> 8) & 0xff);
    buf[6] = (uint8_t)(weaponPic & 0xff);
    buf[7] = (uint8_t)((weaponPic >> 8) & 0xff);
    B_BUF32(&buf[8],  x);
    B_BUF32(&buf[12], y);
    B_BUF32(&buf[16], z);
    oldnet_sendpacket(connecthead, buf, 20);
    { extern int32_t g_netDbgHitN; g_netDbgHitN++; }
}

// HOST: apply a guest-reported hit to the victim's authoritative health. The
// victim's own P_ProcessInput detects extra<=0 and runs P_FragPlayer, which
// credits the frag from frag_ps -- so setting extra/wackedbyactor/frag_ps here
// is the whole job. wackedbyactor also drives bot retaliation (Bot_GetInput).
static void Net_ApplyHitReport(int attacker, int victim, int dmg, int weaponPic)
{
    (void)weaponPic;
    if ((unsigned)victim >= MAXPLAYERS || (unsigned)attacker >= MAXPLAYERS)
        return;
    auto const vp = g_player[victim].ps;
    if (vp == NULL || (unsigned)vp->i >= MAXSPRITES)
        return;
    if (sprite[vp->i].extra <= 0)
        return;                                  // already dead this frame
    auto const ap = g_player[attacker].ps;
    int const as = (ap != NULL && (unsigned)ap->i < MAXSPRITES) ? ap->i : -1;
    // FRIENDLY FIRE: this client-hitscan path bypasses A_IncurDamage, so it must
    // enforce the same rule -- with ffire off, a teammate (coop, or same-team
    // TDM) takes no damage from a reported hit. Coop forces ffire off
    // (netmenu_relaunch), so a guest's stray shot can't kill an ally either.
    if (ud.ffire == 0 && attacker != victim && ap != NULL)
    {
        extern int32_t g_gametypeFlags[];
        if ((g_gametypeFlags[ud.coop] & GAMETYPE_PLAYERSFRIENDLY)
            || ((g_gametypeFlags[ud.coop] & GAMETYPE_TDM) && ap->team == vp->team))
            return;
    }
    sprite[vp->i].extra -= dmg;
    if (sprite[vp->i].extra < 0)
        sprite[vp->i].extra = 0;
    if (as >= 0)
        vp->wackedbyactor = (int16_t)as;         // -> bot revenge + death attribution
    if (attacker != victim)
        vp->frag_ps = (uint8_t)attacker;         // P_FragPlayer credits this on death
    { extern int32_t g_netDbgHitN; g_netDbgHitN++; }   // host tally: reports APPLIED
#ifdef __EMSCRIPTEN__
    if (g_netForensics)
        EM_ASM({ console.log('[hitapply] atk=' + $0 + ' victim=' + $1 + ' dmg=' + $2 + ' hp=' + $3); },
               attacker, victim, dmg, sprite[vp->i].extra);
#endif
}

// HOST: apply a guest-reported MONSTER hit, resolved BY POSITION. The guest and host
// number the same monster with different sprite indices, so the index is only a hint;
// the truth is "the guest's shot hit a live enemy AT (x,y,z)". We find the LIVE enemy
// nearest that spot, wake it if dormant, and ARM the engine's pending-damage fields
// (htextra) -- its CON consumes that next tic exactly like any local weapon hit and
// runs pain or the full death, so the guest that FIRED decides the kill and the host
// world processes it natively.
static void Net_ApplyEnemyHitReport(int attacker, int idxHint, int x, int y, int z, int dmg, int weaponPic)
{
    if ((unsigned)attacker >= MAXPLAYERS)
        return;
    auto const ap = g_player[attacker].ps;
    if (ap == NULL || (unsigned)ap->i >= MAXSPRITES)
        return;
    // Prefer the index hint iff it IS a live enemy here (indices happen to agree);
    // otherwise take the live enemy nearest the reported position.
    int target = -1;
    if ((unsigned)idxHint < MAXSPRITES && sprite[idxHint].statnum < MAXSTATUS
        && A_CheckEnemySprite(&sprite[idxHint]) && sprite[idxHint].extra > 0)
        target = idxHint;
    else
    {
        // Resolve radius: generous ON PURPOSE. The guest aims at where the
        // host stream painted the enemy -- up to a full RTT stale -- so the
        // reported position trails the host's live copy by (enemy speed x
        // ping). 1200 units was tuned on loopback; at a playable 80-250ms it
        // silently ate legitimate hits ("MISS ... no live enemy near"). The
        // index hint (exact in stream mode) still short-circuits this scan;
        // the radius only decides hint-mismatch cases, so widening it cannot
        // misattribute at close range (nearest-live still wins).
        int64_t best = (int64_t)2600 * 2600;   // resolve radius^2 (build x/y units)
        // Scan BOTH the awake (STAT_ACTOR) and DORMANT (STAT_ZOMBIEACTOR) enemy
        // lists. Map monsters SPAWN dormant and only wake when the host's own coarse
        // LOS check to the nearest player clears -- flakier and laggier than the
        // guest's exact-aim shot. A dormant enemy was invisible to the old
        // ACTOR-only scan, so a guest's kill on a monster the host hadn't woken was
        // silently dropped ("MISS"). Resolve it here across both lists; wake below.
        for (int pass = 0; pass < 2; pass++)
        {
            int const scanStat = pass ? STAT_ZOMBIEACTOR : STAT_ACTOR;
            for (bssize_t SPRITES_OF(scanStat, i))
            {
                if (!A_CheckEnemySprite(&sprite[i]) || sprite[i].extra <= 0)
                    continue;
                int64_t const dx = (int64_t)sprite[i].x - x, dy = (int64_t)sprite[i].y - y, dz = ((int64_t)sprite[i].z - z) >> 4;
                int64_t const d = dx * dx + dy * dy + dz * dz;
                if (d < best) { best = d; target = i; }
            }
        }
    }
    if (target < 0)
    { LOG_F(INFO, "[enemyhit] MISS hint=%d pos=%d,%d,%d dmg=%d (no live enemy near)", idxHint, x, y, z, dmg); return; }

    // WAKE a dormant target. Death is CON-driven and runs ONLY for STAT_ACTOR
    // sprites (G_MoveActors), so damaging a STAT_ZOMBIEACTOR just makes a 0-HP sprite
    // that never dies. Relinking it to STAT_ACTOR (a pure linked-list move -- no RNG,
    // no spawn/delete; we're on-tick via Net_DrainEnemyHits) makes G_MoveActors run
    // its death this SAME tic. The guest's shot IS the authority that this monster is
    // live for it -- the host now "handles things outside its own view" for the guest.
    bool const woke = (sprite[target].statnum == STAT_ZOMBIEACTOR);
    if (woke)
    {
        changespritestat(target, STAT_ACTOR);
        actor[target].timetosleep = 0;
    }

    // ARM the engine's pending-damage fields -- the SAME path every weapon uses
    // (A_DamageObject/A_RadiusDamage do htextra += dmg) -- and let the monster's
    // CON consume it once: ifhitweapon -> A_IncurDamage -> ifdead -> death, WITH
    // gibs/sound/kill credit, on the host too.
    //
    // The previous direct `extra -= dmg` write was THE host-side zombie bug: it
    // armed nothing, so the host's copy of a guest-killed monster never entered
    // its damage branch (no death), walked on at negative HP, and A_IncurDamage's
    // extra<0 bail then ATE every subsequent host hit on it -- "my kill didn't
    // register, the guest had already killed it". Accumulating htextra instead
    // gives: simultaneous host+guest damage sums and is consumed as ONE hit (death
    // processed once); a re-fire on a monster whose REAL death already ran still
    // no-ops via the extra<0 bail. Both properties, no direct-write hack.
    if (actor[target].htextra < 0)
        actor[target].htextra = (int16_t)dmg;
    else
        actor[target].htextra = (int16_t)min(actor[target].htextra + dmg, 32000);
    if ((unsigned)weaponPic < MAXTILES)
        actor[target].htpicnum = (int16_t)weaponPic;   // death type / bot retaliation
    actor[target].htowner = (int16_t)ap->i;            // kill attribution to the shooter
    LOG_F(INFO, "[enemyhit] ARM target=%d (hint=%d)%s pic=%d dmg=%d htextra=%d extra=%d", target, idxHint, woke ? " WOKE" : "", (int)sprite[target].picnum, dmg, (int)actor[target].htextra, (int)sprite[target].extra);
    { extern int32_t g_netDbgHitN; g_netDbgHitN++; }   // host tally: reports APPLIED
    if (g_hostProbeIdx < 0) { g_hostProbeIdx = target; g_hostProbePlc = movefifoplc; }   // arm the test probe
}

// HOST, ON-TICK: run the breakable/object hits the guests reported this frame. Called
// from G_DoMoveThings (same phase as weapon-fire A_DamageObject), so the sprites it
// spawns/deletes and the RNG it rolls stay on the authoritative timeline -- unlike
// the old mid-packet call that desynced. dmgSrc = the shooter's player sprite.
void Net_DrainObjectHits(void)
{
    if (myconnectindex != connecthead)
    {
        s_objHitN = 0;   // only the host applies these; a guest just drops them
        return;
    }
    for (int k = 0; k < s_objHitN; k++)
    {
        int const victim = s_objHitVictim[k];
        int const atk    = s_objHitAtk[k];
        if ((unsigned)victim >= MAXSPRITES || sprite[victim].statnum >= MAXSTATUS)
            continue;
        // Players/enemies have their own authoritative paths; never route them here.
        if (sprite[victim].picnum == APLAYER || A_CheckEnemySprite(&sprite[victim]))
            continue;
        if ((unsigned)atk >= MAXPLAYERS)
            continue;
        auto const ap = g_player[atk].ps;
        if (ap != NULL && (unsigned)ap->i < MAXSPRITES)
            A_DamageObject(victim, ap->i);
    }
    s_objHitN = 0;
}

// HOST, ON-TICK: apply the enemy hits guests reported this frame. Called from
// G_DoMoveThings right after the object drain and BEFORE G_MoveWorld, so a monster
// this wakes runs its death CON on this very tic. Draining here (not mid-packet)
// keeps the wake + health writes on the authoritative timeline.
void Net_DrainEnemyHits(void)
{
    if (myconnectindex != connecthead)
    {
        s_enemyHitN = 0;   // only the host resolves these
        return;
    }
    for (int k = 0; k < s_enemyHitN; k++)
    {
        NetEnemyHit const *h = &s_enemyHit[k];
        Net_ApplyEnemyHitReport(h->atk, h->idx, h->x, h->y, h->z, h->dmg, h->wpic);
    }
    s_enemyHitN = 0;
#if defined(NETNATIVE) && !defined(__EMSCRIPTEN__)
    // Headless verification probe (env NN_TESTKILL on the HOST; inert otherwise):
    // ~5s after the first ARM, log the target's state. cstat==0 + a bumped kill
    // counter is positive proof the HOST's own death CON ran -- the exact evidence
    // the previous round's test lacked (it proved only the guest's death).
    extern int32_t g_hostProbeIdx, g_hostProbePlc;
    static int s_probeOn = -1;
    if (s_probeOn < 0)
        s_probeOn = (getenv("NN_TESTKILL") != NULL) ? 1 : 0;
    if (s_probeOn == 1 && g_hostProbeIdx >= 0 && movefifoplc > g_hostProbePlc + 150)
    {
        int killed = 0, pi;
        TRAVERSE_CONNECT(pi)
            if (g_player[pi].ps != NULL)
                killed += g_player[pi].ps->actors_killed;
        LOG_F(INFO, "[hostprobe] idx=%d stat=%d extra=%d htextra=%d cstat=%d killed=%d",
              g_hostProbeIdx, (int)sprite[g_hostProbeIdx].statnum, (int)sprite[g_hostProbeIdx].extra,
              (int)actor[g_hostProbeIdx].htextra, (int)sprite[g_hostProbeIdx].cstat, killed);
        s_probeOn = 2;   // one-shot
    }
#endif
}

// GUEST -> HOST, per-tic: the guest's live weapon so the host fires what the guest
// actually holds instead of reconstructing it from a transient keypress (which it
// drops whenever its copy of the seat is mid-recoil -> "picked up the RPG but fired
// the shotgun", and fatal for the RPG since a projectile weapon needs the host to
// spawn the actual rocket). curr_weapon + the gotweapon bit + this weapon's ammo are
// all P_FireWeapon needs to shoot it and P_CheckWeapon needs to NOT switch away.
static void Net_SendWeaponState(void)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex == connecthead)
        return;
    auto const ps = g_player[myconnectindex].ps;
    if (ps == NULL || (unsigned)ps->curr_weapon >= MAX_WEAPONS)
        return;
    int const w = ps->curr_weapon;
    static uint16_t s_gwSeqOut;
    ++s_gwSeqOut;
    uint8_t buf[8];
    buf[0] = PACKET_TYPE_WEAPON_STATE;
    buf[1] = (uint8_t)w;
    buf[2] = (uint8_t)(ps->gotweapon & 0xff);
    buf[3] = (uint8_t)((ps->gotweapon >> 8) & 0xff);
    buf[4] = (uint8_t)(ps->ammo_amount[w] & 0xff);
    buf[5] = (uint8_t)((ps->ammo_amount[w] >> 8) & 0xff);
    buf[6] = (uint8_t)(s_gwSeqOut & 0xff);          // seq: the channel is UNORDERED --
    buf[7] = (uint8_t)((s_gwSeqOut >> 8) & 0xff);   // a stale packet must not regress the host
    oldnet_sendpacket(connecthead, buf, 8);
}

// GUEST -> HOST, per-tic: authoritative self position/velocity/facing. The host
// adopts it for this seat (Net_ApplyGuestPos) instead of trusting its own
// input-replay integration, which drifts. 41 bytes on the unreliable move
// channel; a drop just means the host coasts one tic on the last base.
static void Net_SendPosReport(void)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex == connecthead || g_netJoinCatchup)
        return;
    auto const ps = g_player[myconnectindex].ps;
    if (ps == NULL || (unsigned)ps->i >= MAXSPRITES)
        return;
    static uint16_t s_posSeqOut;
    ++s_posSeqOut;
    uint8_t buf[41];
    buf[0] = PACKET_TYPE_POS_REPORT;
    buf[1] = (uint8_t)(s_posSeqOut & 0xff);
    buf[2] = (uint8_t)((s_posSeqOut >> 8) & 0xff);
    int j = 3;
    B_BUF32(&buf[j], ps->pos.x);            j += 4;
    B_BUF32(&buf[j], ps->pos.y);            j += 4;
    B_BUF32(&buf[j], ps->pos.z);            j += 4;
    B_BUF32(&buf[j], ps->vel.x);            j += 4;
    B_BUF32(&buf[j], ps->vel.y);            j += 4;
    B_BUF32(&buf[j], ps->vel.z);            j += 4;
    B_BUF32(&buf[j], (int32_t)ps->q16ang);  j += 4;
    B_BUF32(&buf[j], (int32_t)ps->q16horiz); j += 4;
    B_BUF16(&buf[j], (uint16_t)ps->cursectnum); j += 2;
    B_BUF32(&buf[j], sprite[ps->i].z);      j += 4;
    oldnet_sendpacket(connecthead, buf, j);
}

// HOST, per-tic: adopt a guest seat's reported position right before its input
// runs. The guest is the authority for its OWN movement (the OpenArena rule the
// self-snap removal on the guest side completes): the host stops second-guessing
// where the guest ended up and instead keeps its copy honest for everything that
// reads it -- enemy AI targeting, hazard checks, the host player's view of the
// guest. Facing rides along so host-side fire/projectiles for the seat aim true.
void Net_ApplyGuestPos(int seat)
{
    if (!g_netStreamMode || myconnectindex != connecthead
        || (unsigned)seat >= MAXPLAYERS || seat == myconnectindex)
        return;
    if (!s_gpHave[seat] || s_joinFlowSlot == seat)   // mid-heal reports describe the pre-heal world
        return;
    auto const ps = g_player[seat].ps;
    if (ps == NULL || (unsigned)ps->i >= MAXSPRITES)
        return;
    ps->opos      = ps->pos;              // continuity for interpolation consumers
    ps->pos       = s_gpPos[seat];
    ps->bobpos.x  = ps->pos.x;
    ps->bobpos.y  = ps->pos.y;
    ps->vel       = s_gpVel[seat];
    ps->oq16ang   = ps->q16ang;
    ps->oq16horiz = ps->q16horiz;
    ps->q16ang    = s_gpAng[seat];
    ps->q16horiz  = s_gpHoriz[seat];
    if ((unsigned)s_gpSect[seat] < (unsigned)numsectors)
        ps->cursectnum = s_gpSect[seat];
    else
        updatesector(ps->pos.x, ps->pos.y, &ps->cursectnum);
    vec3_t sp = { ps->pos.x, ps->pos.y, s_gpSprZ[seat] };
    setsprite(ps->i, &sp);                // relinks the sprite's sector too
    // Low-rate liveness proof for the wire (send -> stash -> on-tick adopt).
    // PER SEAT: a global counter with two strictly-interleaving guests aliased
    // to one seat forever (511&+1 always hit the same parity) -- the harness
    // read "seat 2 never applies" from a healthy wire.
    static int32_t s_gpApplied[MAXPLAYERS];
    if ((++s_gpApplied[seat] & 511) == 1)
        LOG_F(INFO, "[gpos] seat=%d applies=%d at (%d,%d) plc=%d", seat, s_gpApplied[seat], ps->pos.x, ps->pos.y, (int)movefifoplc);
}

// GUEST: report newly earned key cards upstream (reliable channel; idempotent).
// Sends only on a NEW bit; a level change (mask reset) just re-baselines.
static void Net_SendAccessState(void)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex == connecthead
        || !(g_gametypeFlags[ud.coop] & GAMETYPE_COOP))
        return;
    auto const ps = g_player[myconnectindex].ps;
    if (ps == NULL)
        return;
    uint8_t const cur = (uint8_t)(ps->got_access & 7);
    if ((cur & ~s_accSentMask) == 0)
    {
        s_accSentMask = cur;
        return;
    }
    s_accSentMask = cur;
    uint8_t buf[2] = { PACKET_TYPE_ACCESS_STATE, cur };
    oldnet_sendpacket(connecthead, buf, 2);
    LOG_F(INFO, "[access] guest reports cards mask=%d", cur);
}

// HOST, per-tic (coop stream): one shared key ring. Union every seat's cards,
// OR the union back into every seat (host-side door checks pass for anyone),
// and broadcast newly-added bits to the guests. Level change (empty union)
// re-baselines the broadcast tracker.
void Net_ShareCoopAccess(void)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex != connecthead
        || !(g_gametypeFlags[ud.coop] & GAMETYPE_COOP))
        return;
    int un = 0, i;
    TRAVERSE_CONNECT(i)
        if (g_player[i].ps != NULL)
            un |= (g_player[i].ps->got_access & 7);
    if (un == 0)
    {
        s_accBcastMask = 0;
        return;
    }
    TRAVERSE_CONNECT(i)
        if (g_player[i].ps != NULL)
            g_player[i].ps->got_access |= (int16_t)un;
    if ((un & ~s_accBcastMask) != 0)
    {
        s_accBcastMask = (uint8_t)un;
        uint8_t buf[2] = { PACKET_TYPE_ACCESS_STATE, (uint8_t)un };
        TRAVERSE_CONNECT(i)
            if (i != myconnectindex && !(g_netBotMask & (1 << i)))
                oldnet_sendpacket(i, buf, 2);
        LOG_F(INFO, "[access] host shares cards mask=%d to all seats", un);
    }
}

// HOST, per-tic: converge a guest seat's weapon onto what the guest reports,
// right before we run its input. Guest seats only; no-op until we've heard from it.
//
// The switch NEVER lands mid-fire-cycle (kickback_pic != 0): stomping curr_weapon
// there evaluates the new weapon's timeline at the old weapon's phase (eaten /
// instant shots) and -- worse -- aborts in-flight cycles before their payload
// frame (a HANDBOMB cycle whose spawn frame hasn't hit yet would never throw the
// bomb the guest already threw). Because the report re-arrives every tic, the
// switch simply lands on the first tic the cycle is over -- the same "wait out
// the cycle" a local switch does. When it lands it goes through a full state
// reset (what static P_ChangeWeapon would do, minus the CON veto): raise anim,
// no stale crossfade, clean kickback.
void Net_ApplyGuestWeapon(int seat)
{
    if (myconnectindex != connecthead || (unsigned)seat >= MAXPLAYERS || seat == myconnectindex)
        return;
    if (!s_gwHave[seat])
        return;
    auto const ps = g_player[seat].ps;
    if (ps == NULL)
        return;
    int const w = s_gwWeapon[seat];
    if ((unsigned)w >= MAX_WEAPONS)
        return;
    ps->gotweapon      = s_gwGot[seat];      // the guest owns its inventory
    ps->ammo_amount[w] = s_gwAmmo[seat];     // ...and this weapon's ammo
    if (ps->curr_weapon != w && ps->kickback_pic == 0)
    {
        LOG_F(INFO, "[gw] seat=%d weap %d -> %d", seat, ps->curr_weapon, w);
        ps->last_weapon       = -1;
        ps->reloading         = 0;
        ps->random_club_frame = 0;
        ps->curr_weapon       = (int8_t)w;
        ps->weapon_pos        = WEAPON_POS_RAISE;   // raise the new weapon; fire waits like the guest's did
        P_SetWeaponGamevars(seat, ps);
    }
}

const char *Net_DebugHudStr(void)
{
    static char buf[160];
    int a[4] = { -1, -1, -1, -1 };
    int n = 0;
    for (int i = 0; i < MAXPLAYERS && n < 4; i++)
        if (i != myconnectindex && g_player[i].connected && s_rsHave[i])
            a[n++] = movefifoplc - s_rsLastTic[i];
    extern int32_t g_netDbgHitN;
    extern int32_t g_dbgAutoAimPic;
    // AIM DIAGNOSTIC (la, aa): la = your look_ang (view-vs-shot yaw; nonzero
    // means the crosshair is offset from where the gun fires); aa = the picnum
    // Auto Aim last locked your shot onto (-1 = went straight). If you fire at a
    // wall and aa jumps to an enemy tile, Auto Aim is grabbing an off-crosshair
    // monster -> Options>Game Setup>Auto Aim: Off. If la is nonzero, it's a
    // look-angle offset instead.
    int const la = (g_player[myconnectindex].ps != NULL) ? g_player[myconnectindex].ps->look_ang : 0;
    // gt: gametype index (0=DM, 1=Coop, 2=DM-nospawn, 3/4=TDM); ff: friendly
    // fire. hits: client-authoritative hit reports (guest=sent, host=applied).
    Bsnprintf(buf, sizeof(buf), "NETDBG v53  pkts=%d strm=%d hits=%d  gt=%d ff=%d sk=%d  la=%d aa=%d  age=%d,%d,%d,%d  plc=%d%s",
              g_netDbgPackN, g_netDbgSprN, g_netDbgHitN, ud.coop, ud.ffire, g_netBotSkill, la, g_dbgAutoAimPic,
              a[0], a[1], a[2], a[3], movefifoplc,
              (myconnectindex == connecthead) ? "  [HOST]" : "  [GUEST]");
    return buf;
}

void Net_SmoothRemoteSeats(void)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex == connecthead)
        return;
    Net_ClientPickupScan();   // guest grabs its own pickups (height-tolerant), per tic
    for (int i2 = 0; i2 < MAXPLAYERS; i2++)
    {
        if (!s_rsHave[i2] || i2 == myconnectindex)
            continue;
        auto const ps = g_player[i2].ps;
        if (ps == NULL || (unsigned)ps->i >= MAXSPRITES)
            { s_rsHave[i2] = 0; continue; }
        // VELOCITY CARRIES THE MOTION. With remote inputs zeroed
        // (snapshot-driven seats), P_ProcessInput integrates ps->vel through
        // the clipper this tic -- full host speed, wall-aware. It must be the
        // ONLY thing that moves them: v26 also applied a 5/16 positional pull,
        // so the remote moved TWICE per tic and overshot every target, then
        // the next pull reversed -- the "severe regression" (an oscillation a
        // 256-tic net-displacement probe averages away, so metrics missed it).
        ps->vel = s_rsVel[i2];
        int32_t const ex = s_rsPos[i2].x - ps->pos.x;
        int32_t const ey = s_rsPos[i2].y - ps->pos.y;
        int32_t const ez = s_rsPos[i2].z - ps->pos.z;
        int32_t const em = klabs(ex) + klabs(ey);
        if (em > 2048)
        {
            // Teleport scale (respawn/warp): converge instantly, no ghost glide.
            ps->pos = s_rsPos[i2];
            ps->opos = ps->pos;
            ps->bobpos.x = ps->pos.x; ps->bobpos.y = ps->pos.y;
            vec3_t sp2 = s_rsSpr[i2];
            setsprite(ps->i, &sp2);
            actor[ps->i].bpos = sp2;
            ps->q16ang = ps->oq16ang = s_rsAng[i2];
            ps->q16horiz = ps->oq16horiz = s_rsHoriz[i2];
            if ((unsigned)s_rsSect[i2] < (unsigned)numsectors)
                ps->cursectnum = s_rsSect[i2];
            continue;
        }
        // OVERSHOOT GUARD: when the position error points AGAINST the held
        // velocity, the host has slowed/stopped/turned and we have glided
        // past -- drop the held velocity so the remote settles instead of
        // sailing on for the rest of the pack interval. This is what makes a
        // stop look like a stop instead of a skid-and-return.
        if ((int64_t)ex * ps->vel.x + (int64_t)ey * ps->vel.y < 0)
            ps->vel.x = ps->vel.y = 0;
        // DRIFT CORRECTION ONLY: velocity did the moving; nudge just the
        // accumulated error, gently and clamped, past a deadband -- never a
        // second full displacement.
        if (em + (klabs(ez) >> 2) > 128)
        {
            int32_t const cx = clamp(ex >> 3, -48, 48);
            int32_t const cy = clamp(ey >> 3, -48, 48);
            int32_t const cz = clamp(ez >> 3, -96, 96);
            ps->pos.x += cx; ps->pos.y += cy; ps->pos.z += cz;
            ps->opos.x += cx; ps->opos.y += cy; ps->opos.z += cz;
            ps->bobpos.x += cx; ps->bobpos.y += cy;
        }
        // SYNC THE RENDERED SPRITE TO ps->pos EVERY TIC. This is THE fix for
        // "the bots are not running/moving, apart from rotating a bit": the
        // sprite position was only setsprite'd INSIDE the drift block, so
        // whenever velocity-carry tracked well (error < deadband, drift
        // skipped) the sprite FROZE while ps->pos kept advancing (the probe
        // saw motion, the screen did not). Facing slews below every tic ->
        // "rotating a bit". Now the body follows the position every tic; bpos
        // holds the pre-move sprite pos so the render lerp stays smooth.
        {
            int32_t const oxx = sprite[ps->i].x, oyy = sprite[ps->i].y, ozz = sprite[ps->i].z;
            vec3_t spp = { ps->pos.x, ps->pos.y,
                           ps->pos.z + (s_rsSpr[i2].z - s_rsPos[i2].z) };
            setsprite(ps->i, &spp);
            actor[ps->i].bpos.x = oxx; actor[ps->i].bpos.y = oyy; actor[ps->i].bpos.z = ozz;
            int16_t cs2 = ps->cursectnum;
            updatesector(ps->pos.x, ps->pos.y, &cs2);
            if (cs2 >= 0)
                ps->cursectnum = cs2;
            else if ((unsigned)s_rsSect[i2] < (unsigned)numsectors)
                ps->cursectnum = s_rsSect[i2];
        }
        // Per-tic motion trace (forensics): the JITTER meter the net-displacement
        // probe is blind to. Sign reversals in dx/dy between consecutive samples
        // = oscillation. First remote seat only, every 2nd tic.
        {
            extern int32_t g_netForensics;
            // Log POSITION and SPRITE together: after the fix the sprite must
            // track the position tic for tic. Every remote seat, every 4th tic.
            if (g_netForensics && (movefifoplc & 3) == 0)
                EM_ASM({ console.log('[rsmot] plc=' + $0 + ' seat=' + $1 + ' px=' + $2 + ' py=' + $3
                         + ' sx=' + $4 + ' sy=' + $5); },
                       movefifoplc, i2, ps->pos.x, ps->pos.y,
                       sprite[ps->i].x, sprite[ps->i].y);
        }
        // Facing: bounded slew, never a head-jerk (the F16(32) deadband let
        // remote heads sit visibly wrong, then snap 3x a second).
        fix16_t dA = fix16_ssub(s_rsAng[i2], ps->q16ang);
        while (dA > F16(1024))  dA = fix16_ssub(dA, F16(2048));
        while (dA < -F16(1024)) dA = fix16_sadd(dA, F16(2048));
        fix16_t const stepA = fix16_clamp(dA, -F16(20), F16(20));
        ps->q16ang = ps->oq16ang = fix16_sadd(ps->q16ang, stepA);
        while (ps->q16ang < 0)          ps->q16ang = fix16_sadd(ps->q16ang, F16(2048));
        while (ps->q16ang >= F16(2048)) ps->q16ang = fix16_ssub(ps->q16ang, F16(2048));
        ps->oq16ang = ps->q16ang;
        fix16_t const dH = fix16_ssub(s_rsHoriz[i2], ps->q16horiz);
        ps->q16horiz = ps->oq16horiz = fix16_sadd(ps->q16horiz, fix16_clamp(dH, -F16(8), F16(8)));
        if ((unsigned)ps->i < MAXSPRITES)
            sprite[ps->i].ang = fix16_to_int(ps->q16ang) & 2047;
    }
}

void Net_ApplyPendingStateSnap(void)
{
    if (s_pendingSnapLen == 0 || numplayers < 2)
        return;
    // Stream mode: the pack paints current truth -- apply the moment we are
    // here, no tic alignment (the echo RTT means our consume cursor is always
    // past the host's stamp; under lockstep that made every pack "stale").
    if (!g_netStreamMode)
    {
        if (movefifoplc < s_pendingSnapTic)
            return;                              // not at the stamp yet
        if (movefifoplc > s_pendingSnapTic)
        {
            // Overshot: shouldn't happen (this runs every tic) -- drop; the
            // ladder re-sends if the divergence persists.
            s_pendingSnapLen = 0;
#ifdef __EMSCRIPTEN__
            EM_ASM({ console.log('[eng] softsnap OVERSHOT (tic=' + $0 + ' plc=' + $1 + ')'); },
                   s_pendingSnapTic, movefifoplc);
#endif
            return;
        }
    }
    s_pendingSnapLen = 0;
    char *buf = s_pendingSnap;
    int jj = 1;
    int const cnt = (uint8_t)buf[jj++];
    jj += 4;                                     // tic stamp (already matched)
    // RNG rides the wire for the legacy path only. Stream mode never touches
    // the guest's RNG ("there shouldn't be much RNG here" -- live directive):
    // the guest sim is a cosmetic predictor, world truth arrives as state.
    if (!g_netStreamMode)
    {
        randomseed     = (int32_t)B_UNBUF32(&buf[jj]);
        g_globalRandom = (int32_t)B_UNBUF32(&buf[jj + 4]);
    }
    jj += 8;
    bool selfCorrected = false;
    for (int e = 0; e < cnt; e++)
    {
        int const slot = (uint8_t)buf[jj++];
        if ((unsigned)slot >= MAXPLAYERS || g_player[slot].ps == NULL)
            { jj += 74; continue; }
        auto const ps = g_player[slot].ps;
        vec3_t pp, op, vel, sp;
        vec2_t bob;
        pp.x  = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        pp.y  = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        pp.z  = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        op.x  = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        op.y  = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        op.z  = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        bob.x = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        bob.y = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        vel.x = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        vel.y = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        vel.z = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        fix16_t const snapAng   = (fix16_t)B_UNBUF32(&buf[jj]); jj += 4;
        fix16_t const snapHoriz = (fix16_t)B_UNBUF32(&buf[jj]); jj += 4;
        sp.x = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        sp.y = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        sp.z = (int32_t)B_UNBUF32(&buf[jj]); jj += 4;
        int16_t  const sprExtra = (int16_t)B_UNBUF16(&buf[jj]); jj += 2;
        int16_t  const cursect  = (int16_t)B_UNBUF16(&buf[jj]); jj += 2;
        uint16_t const frag     = (uint16_t)B_UNBUF16(&buf[jj]); jj += 2;
        uint16_t const fragged  = (uint16_t)B_UNBUF16(&buf[jj]); jj += 2;
        int16_t  const dead     = (int16_t)B_UNBUF16(&buf[jj]); jj += 2;

        bool const self = (slot == myconnectindex);
        // SELF-MOVEMENT IS CLIENT-OWNED (the OpenArena rule; the user's
        // directive, twice: "it's meant to be client first"). Under stream
        // mode the pack corrects self position only at teleport scale
        // (respawn, teleporter, RESPAWN pad) -- anything smaller is ordinary
        // prediction skew the closed loop absorbs, and stomping it WAS the
        // live "host keeps correcting my movement". Legacy lockstep mode
        // keeps the full tic-aligned correction.
        bool applyPos = true;
        if (!self && g_netStreamMode)
        {
            // REMOTE seats: pack becomes the smoothing TARGET (applied over
            // the next few tics by Net_SmoothRemoteSeats). Health/score/death
            // stay instant below -- status syncs once; motion glides.
            s_rsPos[slot]   = pp;
            s_rsSpr[slot]   = sp;
            s_rsAng[slot]   = snapAng;
            s_rsHoriz[slot] = snapHoriz;
            s_rsSect[slot]  = cursect;
            s_rsVel[slot]   = vel;
            s_rsHave[slot]  = 1;
            s_rsLastTic[slot] = movefifoplc;
            ps->vel = vel;              // and held per-tic by the smoother
            {
                extern int32_t g_netForensics;
                if (g_netForensics && (e == 0))
                    EM_ASM({ console.log('[rserr] plc=' + $0 + ' seat=' + $1 + ' err=' + $2); },
                           movefifoplc, slot,
                           klabs(ps->pos.x - pp.x) + klabs(ps->pos.y - pp.y));
#ifndef __EMSCRIPTEN__
                // 16-seat probe (native forensics): every remote seat's snap
                // target, sampled. Two samples with different (x,y) for a seat
                // prove that seat MOVES on this guest's screen -- the exact
                // user-visible outcome the old 768B stash cap silently broke
                // (seats past ~8 joined but never moved).
                static uint32_t s_rsProbeN[MAXPLAYERS];
                if (g_netForensics && (++s_rsProbeN[slot] & 7) == 1)
                    LOG_F(INFO, "[rsnap] seat=%d applies=%u at (%d,%d) plc=%d",
                          slot, s_rsProbeN[slot], pp.x, pp.y, (int)movefifoplc);
#endif
            }
            if ((unsigned)ps->i < MAXSPRITES)
                sprite[ps->i].extra = sprExtra;
            ps->frag        = frag;
            ps->fraggedself = fragged;
            ps->dead_flag   = dead;
            continue;
        }
        if (self && g_netStreamMode)
        {
            // NEVER. Self movement is CLIENT-OWNED, full stop (live 2026-08-14:
            // "it should be trusting me as the client where I ended up"). The
            // teleport-scale escape hatch that used to live here compared us
            // against the host's INPUT-REPLAY copy of this seat -- a drifting
            // reconstruction -- so any transport hiccup while running (or the
            // host picking a different respawn point) yanked the guest to a
            // place it never was. The host's copy is now slaved to our per-tic
            // POS_REPORT (Net_ApplyGuestPos), so there is nothing authoritative
            // to correct us WITH -- the echo could only ever be our own stale
            // position. Health/score below still apply.
            applyPos = false;
        }
        if (applyPos)
        {
            ps->pos = pp;
            ps->opos = op;
            ps->bobpos = bob;
            ps->vel = vel;
        }
        // SELF-FACING IS CLIENT-OWNED, full stop ("the truth needs to come
        // from the player that fires"). Remote facing: the guest's local sim
        // turns remotes smoothly from the echoed inputs -- re-anchoring every
        // pack snapped their heads 3x/sec for nothing (animations are client
        // side wherever possible). Re-anchor only past real drift.
        if (!self)
        {
            if (g_netStreamMode)
            {
                fix16_t dAng = fix16_ssub(ps->q16ang, snapAng);
                while (dAng > F16(1024))  dAng = fix16_ssub(dAng, F16(2048));
                while (dAng < -F16(1024)) dAng = fix16_sadd(dAng, F16(2048));
                if (fix16_abs(dAng) > F16(32)
                    || fix16_abs(fix16_ssub(ps->q16horiz, snapHoriz)) > F16(16))
                {
                    ps->q16ang   = ps->oq16ang   = snapAng;
                    ps->q16horiz = ps->oq16horiz = snapHoriz;
                }
            }
            else
            {
                ps->q16ang   = ps->oq16ang   = snapAng;
                ps->q16horiz = ps->oq16horiz = snapHoriz;
            }
        }
        if ((unsigned)ps->i < MAXSPRITES)
        {
            if (applyPos)
            {
                setsprite(ps->i, &sp);
                actor[ps->i].bpos = sp;
            }
            // SELF PAIN REPLAY (stream): monster damage is computed host-side,
            // so our own health drop used to arrive as a silent number -- no
            // tint, no grunt, no flinch ("animations for getting hit by
            // monsters aren't showing up"). Instead of writing the drop, ARM
            // the engine's own pending-hit channel with the delta and let the
            // local APLAYER CON consume it (ifhitweapon -> A_IncurDamage): the
            // full authentic pain pipeline runs -- palfrom tint, pain sound,
            // knockback nudge, even the real death branch on a lethal drop --
            // and health still lands exactly on host truth this same tic.
            // Damage the local sim already predicted (falls, melee bites via
            // CON addphealth) matched the host's number, so its delta is 0
            // here -- no double pain.
            int const oldExtra = sprite[ps->i].extra;
            // Stale-pack kill guard: a pack STAMPED before our own (re)spawn
            // still carries the pre-spawn extra (0 for a pending seat, low for
            // a corpse) -- replaying that as damage killed the fresh spawn on
            // the spot. Self health only from packs at/after the spawn tic.
            if (self && g_netStreamMode && s_pendingSnapTic < g_netSelfSpawnTic)
                ;   // skip both the selfpain arm and the raw write below
            else
            if (self && g_netStreamMode && !ps->dead_flag && oldExtra > 0
                && sprExtra < oldExtra && actor[ps->i].htextra < 0)
            {
                actor[ps->i].htextra  = (int16_t)min<int>(oldExtra - sprExtra, 32000);
                actor[ps->i].htpicnum = SHOTSPARK1;
                actor[ps->i].htowner  = ps->i;   // self-owner: skips the coop friendly-fire nullify
                actor[ps->i].htang    = (int16_t)((fix16_to_int(ps->q16ang) + 1024) & 2047);
                // extra stays at oldExtra: A_IncurDamage subtracts the delta this tic.
                LOG_F(INFO, "[selfpain] ARM dmg=%d hp %d -> %d", oldExtra - sprExtra, oldExtra, (int)sprExtra);
            }
            else
                sprite[ps->i].extra = sprExtra;   // health is host truth, always
        }
        if (applyPos && (unsigned)cursect < (unsigned)numsectors)
            ps->cursectnum = cursect;
        // Scoreboard is host truth (guest hit rolls drift). Self death
        // transitions stay local-sim-owned: painting dead_flag backwards
        // (host pack staler than a local respawn) would kill the view.
        ps->frag        = frag;
        ps->fraggedself = fragged;
        if (!self)
            ps->dead_flag = dead;
    }
    // ANIMWALL TAG PHASES -- LEGACY ONLY: they exist to realign RNG-coupled
    // lockstep state. Wall animation phases are pure visuals; under stream
    // mode they are client-owned ("make sure animations are client side only
    // wherever possible") -- parse past, never apply.
    {
        int const awc = (uint8_t)buf[jj++];
        for (int aw = 0; aw < awc; aw++)
        {
            int16_t const tag = (int16_t)B_UNBUF16(&buf[jj]); jj += 2;
            if (!g_netStreamMode && aw < g_animWallCnt)
                animwall[aw].tag = tag;
        }
    }
    // Prediction re-base yanks the rendered view onto the authoritative ps --
    // correct after a real correction, POISON as a 3Hz habit: with self state
    // untouched there is nothing to re-base on, and doing it anyway was the
    // live "still snapping my view/angle" (the replay window races the
    // sampler mid-frame). Stream mode re-bases ONLY when the pack actually
    // teleport-corrected us; legacy keeps the old always-re-base.
    if (!g_netStreamMode || selfCorrected)
    {
        Net_InitializePrediction();   // fresh prediction base off corrected state
        Net_ResetSyncCheck();         // stale verdicts described the pre-snap world
    }
    // Stream mode applies ~3x/sec: prints are forensics-only there.
    if (!g_netStreamMode || g_netForensics)
    {
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[eng] softsnap applied ALIGNED at tic ' + $0 + ' (' + $1 + ' players)'); },
               movefifoplc, cnt);
#else
        initprintf("net: soft state snap applied at tic %d (%d players)\n", movefifoplc, cnt);
#endif
    }
}

// ── STATE-AUTHORITY STREAM (the OpenArena model; see g_netStreamMode) ───────
// The host's world is the only truth. Every stream lap it broadcasts (a) the
// player pack (Net_SendStateSnap: position/health/score for every seat) and
// (b) sprite deltas against a host-side shadow of what was last streamed,
// plus a round-robin keyframe sweep that repaints the whole active set every
// few seconds -- so entry-time divergence (independently premapped guests)
// and any dropped delta self-heal by construction. Guests keep simulating as
// a PREDICTOR (animation between paints, local feel); the stream overwrites
// their drift. Records address sprites BY HOST INDEX: guests materialize
// missing indices exactly there (Net_RotateFreeSpriteToHead) and delete what
// the host deleted, so "a hydrant shot on the host" exists/dies on the guest
// within one stream lap -- the 30-second class of sync complaints becomes a
// <=200ms paint. Player-owned sprites are excluded (the player pack owns
// them); SE effectors are excluded (runtime-motion carriers whose fields are
// sim-internal, and painting them would fight sector machinery).
enum
{
    NET_STREAM_PLAYER_TICS = 10,   // player pack cadence (~3Hz)
    NET_STREAM_SPRITE_TICS = 5,    // sprite delta cadence (~6Hz)
    NET_STREAM_DIRTY_CAP   = 128,  // dirty records per lap (64 measured saturated in combat)
    NET_STREAM_SWEEP_CAP   = 48,   // keyframe records per lap
    NET_SPRREC_BYTES       = 37,
    // Record flags. OWNERSHIP MATRIX (live directive: "gun animations are
    // owned by the client. Doors are owned by the host. Item states are owned
    // by the host."): full records carry identity (picnum/cstat/...) and fire
    // only on REAL host-side change -- item taken/respawned paints promptly.
    // Sweep refreshes are KINEMATIC (position/statnum/health only) so they
    // can never restart an animation the guest already has right (live: "an
    // abnormal amount of updates, syncing the same game animations more than
    // once"). Weapon/HUD anim state is never streamed at all.
    NET_SPRF_DELETE    = 0x1,
    NET_SPRF_KINEMATIC = 0x2,
    // Sector stream (doors/elevators/crushers = ceiling+floor heights).
    NET_STREAM_SECT_DIRTY = 48,
    NET_STREAM_SECT_SWEEP = 16,
    NET_SECREC_BYTES      = 10,
    // Wall stream (wall-motion doors: vertex x/y). Small caps: only door
    // walls are eligible, and a whole swing door is ~6 walls.
    NET_STREAM_WALL_DIRTY = 24,
    NET_STREAM_WALL_SWEEP = 8,
    NET_WALLREC_BYTES     = 10,
};
// Statnums that never stream: SE effectors are sim-internal machinery, and
// STAT_MISC is pure cosmetics (casings, debris, teleport FX) every guest's
// local sim spawns for itself -- painting those by-index would fight the
// guest's own short-lived spawns at colliding indices, for zero visual gain.
// (Pair33 measured casing churn saturating the old 64-record budget.)
static inline bool Net_StreamSkipsStat(int st)
{
    return st == STAT_EFFECTOR || st == STAT_MISC;
}
struct NetSprShadow { vec3_t pos; int16_t ang, sect, stat, picnum, cstat, extra; };
static NetSprShadow s_sprShadow[MAXSPRITES];
// Guest-side: enemies whose host-authoritative death we have already replayed.
// The guest re-simulates actors as a predictor; a Duke death is a CON-driven
// transition to a corpse (NOT a deletesprite) that the stream does not carry, so
// merely freezing the sprite pins its last ALIVE frame (an unkillable statue) and
// the guest can't finish it (monster damage is host-only). Instead, when the host
// reports an enemy dead (extra<=0) we replay a lethal hit so the guest's OWN CON
// runs the full death -> a real corpse, exactly as if it had killed the enemy
// itself. This latch fires that replay exactly once per death (cleared on
// respawn/slot-reuse/level-entry so a later enemy at the same index is killed too).
static uint8_t s_hostDead[MAXSPRITES];
// First movefifoplc at which a LIVE host record contradicted a LOCAL corpse at
// this index (0 = no standing contradiction). See Net_CorpseReviveDue.
static int32_t s_liveMismatchPlc[MAXSPRITES];
struct NetSecShadow { int32_t cz, fz; };
static NetSecShadow s_secShadow[MAXSECTORS];
// WALL-MOTION DOORS (swing ST23, slide ST25/ST9, splitting-ST ST26) move wall
// VERTICES -- the sector stream (heights only) never carried them, so a card
// door the host opened stayed closed on every guest's screen forever (live
// 2026-08-14: "it shows as open on the host, which is the one that didn't open
// it" -- remote seats' inputs are zeroed on guests, so host-side operates
// never replay there). Scope is door walls ONLY: subways/continuous rotators
// are the guest's own in-phase local sim and painting them would fight it.
struct NetWallShadow { int32_t x, y; };
static NetWallShadow s_wallShadow[MAXWALLS];
static uint8_t s_wallDoorish[MAXWALLS];
static int32_t s_wallCursorD, s_wallCursorS;
static int32_t s_wallEligBuiltPlc = -1;   // <0: (re)build eligibility + shadows
static int32_t s_lastPlayerStreamPlc = -1;
static int32_t s_lastSpriteStreamPlc = -1;
static int32_t s_sprShadowPrimedPlc  = -1;   // <0: prime shadows before deltas
static int32_t s_dirtyCursor, s_sweepCursor;
static int32_t s_secDirtyCursor, s_secSweepCursor;
static uint32_t s_strmLap;                   // sweep goes FULL every 8th lap (repairs
                                             // missing sprites; kinematic otherwise)

static inline void Net_ShadowFrom(int idx)
{
    auto &sh = s_sprShadow[idx];
    sh.pos.x  = sprite[idx].x;
    sh.pos.y  = sprite[idx].y;
    sh.pos.z  = sprite[idx].z;
    sh.ang    = sprite[idx].ang;
    sh.sect   = sprite[idx].sectnum;
    sh.stat   = sprite[idx].statnum;
    sh.picnum = sprite[idx].picnum;
    sh.cstat  = sprite[idx].cstat;
    sh.extra  = sprite[idx].extra;
}

static inline bool Net_IsPlayerSprite(int idx)
{
    for (int p = 0; p < MAXPLAYERS; p++)
        if (g_player[p].connected && g_player[p].ps != NULL && g_player[p].ps->i == idx)
            return true;
    return false;
}

static int Net_WriteSpriteRec(char *buf, int j, int idx, uint8_t flags)
{
    bool const del = (flags & NET_SPRF_DELETE) != 0;
    B_BUF16(&buf[j], (uint16_t)idx);                             j += 2;
    buf[j++] = (char)flags;
    B_BUF16(&buf[j], del ? 0xFFFFu : (uint16_t)sprite[idx].statnum); j += 2;
    B_BUF16(&buf[j], (uint16_t)sprite[idx].picnum);              j += 2;
    B_BUF16(&buf[j], (uint16_t)sprite[idx].sectnum);             j += 2;
    B_BUF32(&buf[j], sprite[idx].x);                             j += 4;
    B_BUF32(&buf[j], sprite[idx].y);                             j += 4;
    B_BUF32(&buf[j], sprite[idx].z);                             j += 4;
    B_BUF16(&buf[j], (uint16_t)sprite[idx].ang);                 j += 2;
    B_BUF16(&buf[j], (uint16_t)sprite[idx].xvel);                j += 2;
    B_BUF16(&buf[j], (uint16_t)sprite[idx].zvel);                j += 2;
    B_BUF16(&buf[j], (uint16_t)sprite[idx].cstat);               j += 2;
    B_BUF16(&buf[j], (uint16_t)sprite[idx].owner);               j += 2;
    B_BUF16(&buf[j], (uint16_t)sprite[idx].extra);               j += 2;
    buf[j++] = (char)sprite[idx].shade;
    buf[j++] = (char)sprite[idx].pal;
    buf[j++] = (char)sprite[idx].xrepeat;
    buf[j++] = (char)sprite[idx].yrepeat;
    return j;
}

static void Net_StreamAuthoritativeState(void)
{
    if (myconnectindex != connecthead || numplayers < 2)
        return;
    auto const myps = g_player[myconnectindex].ps;
    if (myps == NULL || !(myps->gm & MODE_GAME))
        return;

    {
        // Test rig (NN_TESTEOL=N): the HOST's sim hits the exit (what a
        // replayed guest press produces anyway) -> broadcast -> both sides
        // must enter the next level together. Fires after the guest's own
        // deferred attempt (plc 250) so the suppression is proven first.
        // N = total transitions (re-arms per level: movefifoplc regresses at
        // entry) so a single run can prove consecutive-EOL behavior (the
        // barrier epoch fence: counter inflation once made every barrier
        // after the first transition release instantly).
        static int s_teFiresH = -1;   // transitions remaining
        static int s_teArmedH = 1;    // one shot per level
        static int s_tePlc = -1;      // NN_TESTEOL_PLC overrides the firing tic (soak runs)
        if (s_tePlc < 0)
        {
            const char *p = getenv("NN_TESTEOL_PLC");
            s_tePlc = (p && atoi(p) > 0) ? atoi(p) : 500;
        }
        if (s_teFiresH < 0)
        {
            const char *e = getenv("NN_TESTEOL");
            s_teFiresH = e ? atoi(e) : 0;
            if (s_teFiresH < 0) s_teFiresH = 0;
        }
        if (movefifoplc < s_tePlc)
            s_teArmedH = 1;
        if (s_teFiresH > 0 && s_teArmedH && movefifoplc > s_tePlc)
        {
            s_teFiresH--;
            s_teArmedH = 0;
            LOG_F(INFO, "[testeol] host firing P_EndLevel (%d more after this)", s_teFiresH);
            P_EndLevel();
        }
    }

    // Level (re)entry: the consume cursor regressed -> shadow is a stale map.
    if (movefifoplc < s_lastPlayerStreamPlc || movefifoplc < s_lastSpriteStreamPlc)
    {
        s_sprShadowPrimedPlc  = -1;
        s_lastPlayerStreamPlc = s_lastSpriteStreamPlc = -1;
    }

    int i, humanGuests = 0;
    TRAVERSE_CONNECT(i)
        if (i != myconnectindex && !(g_netBotMask & (1 << i)))
            humanGuests++;
    if (!humanGuests)
    {
        s_sprShadowPrimedPlc = -1;   // next guest starts from a fresh prime
        return;
    }
    // NEW GUEST SEATED: status events sync exactly once, so a guest who
    // missed some (the catchup guard drops stream packets while it loads)
    // gets a ONE-SHOT full world paint -- invalidate the shadows and let the
    // dirty caps pace the burst out over ~1s. This is the only time identity
    // is ever re-sent without a host-side change.
    {
        static int s_lastHumanGuests;
        if (humanGuests > s_lastHumanGuests && s_sprShadowPrimedPlc >= 0)
        {
            for (int idx = 0; idx < MAXSPRITES; idx++)
                s_sprShadow[idx].stat = -1;
            for (int sct = 0; sct < numsectors; sct++)
                s_secShadow[sct].cz = s_secShadow[sct].fz = INT32_MIN;
        }
        s_lastHumanGuests = humanGuests;
    }

    // Player pack: position/velocity/facing/health/score truth, every seat.
    if (s_lastPlayerStreamPlc < 0 || movefifoplc - s_lastPlayerStreamPlc >= NET_STREAM_PLAYER_TICS)
    {
        s_lastPlayerStreamPlc = movefifoplc;
        TRAVERSE_CONNECT(i)
            if (i != myconnectindex && !(g_netBotMask & (1 << i)))
                Net_SendStateSnap(i);
    }

    if (s_sprShadowPrimedPlc < 0)
    {
        // Prime: shadows := live world, deltas measure from here. The sweeps
        // below repaint entry-time divergence within a few seconds.
        for (int idx = 0; idx < MAXSPRITES; idx++)
        {
            if (sprite[idx].statnum < MAXSTATUS)
                Net_ShadowFrom(idx);
            else
                s_sprShadow[idx].stat = -1;
        }
        for (int sct = 0; sct < numsectors; sct++)
        {
            s_secShadow[sct].cz = sector[sct].ceilingz;
            s_secShadow[sct].fz = sector[sct].floorz;
        }
        s_wallEligBuiltPlc = -1;   // walls re-prime with everything else
        s_sprShadowPrimedPlc  = movefifoplc;
        s_lastSpriteStreamPlc = movefifoplc;
        return;
    }
    if (movefifoplc - s_lastSpriteStreamPlc < NET_STREAM_SPRITE_TICS)
        return;
    s_lastSpriteStreamPlc = movefifoplc;

    static char strm[6600];   // 2 + 176 records * 36B = 6338 max
    int j = 0;
    strm[j++] = (char)PACKET_TYPE_SPRITE_STREAM;
    int const cntPos = j++;
    int recs = 0;

    // Pass 1 -- dirty scan, rotating start (a burst can't starve high
    // indices). Also emits deletions: shadow active, world free.
    int scanned = 0;
    for (; scanned < MAXSPRITES && recs < NET_STREAM_DIRTY_CAP; scanned++)
    {
        int const idx = (s_dirtyCursor + scanned) & (MAXSPRITES - 1);
        auto &sh = s_sprShadow[idx];
        if (sprite[idx].statnum >= MAXSTATUS)
        {
            // Freed. Emit a delete only if the guest was ever SENT this
            // sprite (last shadowed stat was a streamed one) -- cosmetic
            // (skipped-stat) churn must not flood the record budget.
            if (sh.stat >= 0 && !Net_StreamSkipsStat(sh.stat))
            {
                j = Net_WriteSpriteRec(strm, j, idx, NET_SPRF_DELETE);
                recs++;
            }
            sh.stat = -1;
            continue;
        }
        if (Net_StreamSkipsStat(sprite[idx].statnum) || Net_IsPlayerSprite(idx))
        {
            // Streamed -> skipped transition (actor became debris): the guest
            // holds a stale streamed copy -- delete it; its own local sim
            // owns the cosmetic from here.
            if (sh.stat >= 0 && !Net_StreamSkipsStat(sh.stat) && !Net_IsPlayerSprite(idx))
            {
                j = Net_WriteSpriteRec(strm, j, idx, NET_SPRF_DELETE);
                recs++;
            }
            Net_ShadowFrom(idx);
            continue;
        }
        // STATUS SYNCS ONCE (live directive): identity changes (picked up,
        // destroyed, awakened, spawned) go as ONE full record on the reliable
        // ordered channel; motion drift goes kinematic and can never restart
        // an animation the guest owns.
        bool const statusChanged = (sh.stat < 0
            || sprite[idx].statnum != sh.stat || sprite[idx].picnum != sh.picnum
            || sprite[idx].cstat != sh.cstat);
        bool const moved = (sprite[idx].x != sh.pos.x || sprite[idx].y != sh.pos.y
            || sprite[idx].z != sh.pos.z || sprite[idx].ang != sh.ang
            || sprite[idx].sectnum != sh.sect
            // extra too: kills/damage must reach the guest within a lap (kinematic
            // records carry extra) -- a stationary monster's death otherwise waits
            // on the slow round-robin sweep, seconds after the kill.
            || sprite[idx].extra != sh.extra);
        if (statusChanged || moved)
        {
            j = Net_WriteSpriteRec(strm, j, idx, statusChanged ? 0 : NET_SPRF_KINEMATIC);
            Net_ShadowFrom(idx);
            recs++;
        }
    }
    s_dirtyCursor = (s_dirtyCursor + scanned) & (MAXSPRITES - 1);

    // Pass 2 -- keyframe sweep over active sprites, round-robin. ALWAYS
    // kinematic (position/statnum/health): identity syncs exactly once, on
    // change, and a routine refresh can never restart an animation the guest
    // owns. Guests who missed events (seated mid-match while the catchup
    // guard dropped stream packets) get the one-shot full world paint below.
    uint8_t const sweepFlags = NET_SPRF_KINEMATIC;
    int sweeps = 0, walked = 0;
    for (; walked < MAXSPRITES && sweeps < NET_STREAM_SWEEP_CAP; walked++)
    {
        int const idx = (s_sweepCursor + walked) & (MAXSPRITES - 1);
        if (sprite[idx].statnum >= MAXSTATUS || Net_StreamSkipsStat(sprite[idx].statnum)
            || Net_IsPlayerSprite(idx))
            continue;
        j = Net_WriteSpriteRec(strm, j, idx, sweepFlags);
        if (sweepFlags == 0)
            Net_ShadowFrom(idx);
        sweeps++;
    }
    s_sweepCursor = (s_sweepCursor + walked) & (MAXSPRITES - 1);

    if (recs + sweeps)
    {
        strm[cntPos] = (char)(recs + sweeps);
        TRAVERSE_CONNECT(i)
            if (i != myconnectindex && !(g_netBotMask & (1 << i)))
                oldnet_sendpacket(i, (unsigned char *)strm, j);
    }

    // Sector stream -- doors/elevators/crushers ("doors are owned by the
    // host", live directive). Ceiling+floor heights: the dirty pass catches
    // motion the moment it starts; the small sweep repairs baseline drift
    // and pins any door a guest's local sim moved out of phase.
    static char sec[700];
    int sj = 0;
    sec[sj++] = (char)PACKET_TYPE_SECTOR_STREAM;
    int const secCntPos = sj++;
    int srecs = 0, sscanned = 0;
    for (; sscanned < numsectors && srecs < NET_STREAM_SECT_DIRTY; sscanned++)
    {
        int const sct = (s_secDirtyCursor + sscanned) % numsectors;
        if (sector[sct].ceilingz != s_secShadow[sct].cz
            || sector[sct].floorz != s_secShadow[sct].fz)
        {
            B_BUF16(&sec[sj], (uint16_t)sct);        sj += 2;
            B_BUF32(&sec[sj], sector[sct].ceilingz); sj += 4;
            B_BUF32(&sec[sj], sector[sct].floorz);   sj += 4;
            s_secShadow[sct].cz = sector[sct].ceilingz;
            s_secShadow[sct].fz = sector[sct].floorz;
            srecs++;
        }
    }
    s_secDirtyCursor = (numsectors > 0) ? (s_secDirtyCursor + sscanned) % numsectors : 0;
    int ssweeps = 0, swalked = 0;
    for (; swalked < numsectors && ssweeps < NET_STREAM_SECT_SWEEP; swalked++)
    {
        int const sct = (s_secSweepCursor + swalked) % numsectors;
        B_BUF16(&sec[sj], (uint16_t)sct);        sj += 2;
        B_BUF32(&sec[sj], sector[sct].ceilingz); sj += 4;
        B_BUF32(&sec[sj], sector[sct].floorz);   sj += 4;
        ssweeps++;
    }
    s_secSweepCursor = (numsectors > 0) ? (s_secSweepCursor + swalked) % numsectors : 0;
    if (srecs + ssweeps)
    {
        sec[secCntPos] = (char)(srecs + ssweeps);
        TRAVERSE_CONNECT(i)
            if (i != myconnectindex && !(g_netBotMask & (1 << i)))
                oldnet_sendpacket(i, (unsigned char *)sec, sj);
    }

    // Wall stream -- see s_wallDoorish: vertex paints for wall-motion doors,
    // the geometry the height stream can't express.
    if (s_wallEligBuiltPlc < 0)
    {
        Bmemset(s_wallDoorish, 0, sizeof(s_wallDoorish));
        for (int sct = 0; sct < numsectors; sct++)
        {
            switch (sector[sct].lotag)
            {
                case ST_9_SLIDING_ST_DOOR: case ST_23_SWINGING_DOOR:
                case ST_25_SLIDING_DOOR:   case ST_26_SPLITTING_ST_DOOR:
                {
                    int const we = sector[sct].wallptr + sector[sct].wallnum;
                    for (int w = sector[sct].wallptr; w >= 0 && w < we && w < numwalls; w++)
                        s_wallDoorish[w] = 1;
                    break;
                }
                default: break;
            }
        }
        for (int w = 0; w < numwalls; w++)
        {
            s_wallShadow[w].x = wall[w].x;
            s_wallShadow[w].y = wall[w].y;
        }
        s_wallEligBuiltPlc = movefifoplc;
        {
            int elig = 0;
            for (int w = 0; w < numwalls; w++)
                elig += s_wallDoorish[w];
            LOG_F(INFO, "[wallstrm] %d door walls eligible for streaming", elig);
        }
    }
    static char wstm[700];
    int wj = 0;
    wstm[wj++] = (char)PACKET_TYPE_WALL_STREAM;
    int const wCntPos = wj++;
    int wrecs = 0, wscanned = 0;
    for (; wscanned < numwalls && wrecs < NET_STREAM_WALL_DIRTY; wscanned++)
    {
        int const w = (s_wallCursorD + wscanned) % numwalls;
        if (!s_wallDoorish[w]
            || (wall[w].x == s_wallShadow[w].x && wall[w].y == s_wallShadow[w].y))
            continue;
        B_BUF16(&wstm[wj], (uint16_t)w); wj += 2;
        B_BUF32(&wstm[wj], wall[w].x);   wj += 4;
        B_BUF32(&wstm[wj], wall[w].y);   wj += 4;
        s_wallShadow[w].x = wall[w].x;
        s_wallShadow[w].y = wall[w].y;
        wrecs++;
    }
    s_wallCursorD = (numwalls > 0) ? (s_wallCursorD + wscanned) % numwalls : 0;
    int wsweep = 0, wwalked = 0;
    for (; wwalked < numwalls && wsweep < NET_STREAM_WALL_SWEEP; wwalked++)
    {
        int const w = (s_wallCursorS + wwalked) % numwalls;
        if (!s_wallDoorish[w])
            continue;
        B_BUF16(&wstm[wj], (uint16_t)w); wj += 2;
        B_BUF32(&wstm[wj], wall[w].x);   wj += 4;
        B_BUF32(&wstm[wj], wall[w].y);   wj += 4;
        wsweep++;
    }
    s_wallCursorS = (numwalls > 0) ? (s_wallCursorS + wwalked) % numwalls : 0;
    if (wrecs + wsweep)
    {
        wstm[wCntPos] = (char)(wrecs + wsweep);
        TRAVERSE_CONNECT(i)
            if (i != myconnectindex && !(g_netBotMask & (1 << i)))
                oldnet_sendpacket(i, (unsigned char *)wstm, wj);
    }
#ifdef __EMSCRIPTEN__
    if (g_netForensics)
    {
        static int32_t s_lastStrmLog;
        if (movefifoplc - s_lastStrmLog >= 150)
        {
            s_lastStrmLog = movefifoplc;
            EM_ASM({ console.log('[strm] plc=' + $0 + ' dirty=' + $1 + ' sweep=' + $2 + ' bytes=' + $3); },
                   movefifoplc, recs, sweeps, j);
        }
    }
#endif
}

// CLIENT-SIDE PICKUP CREDIT (guest). Items are host-owned and the host
// streams a DELETE when one is taken -- but the player pack carries NO
// weapon/inventory state, so a guest that walked onto an item watched it
// vanish without ever getting it (user: "weapon pickup ... needs to be client
// side credited"). When a pickup is deleted right on top of MY player, grant
// it here. Amounts follow the stock pickup values; exact parity is not
// critical -- getting the weapon in hand is the point.
static void Net_GrantPickup(DukePlayer_t *ps, int picnum)
{
    int const spid = ps->i;
    switch (tileGetMapping(picnum))
    {
        case FIRSTGUNSPRITE__:   P_AddWeapon(ps, CHAINGUN_WEAPON, 1);   P_AddAmmo(ps, CHAINGUN_WEAPON, 50);   break;
        case CHAINGUNSPRITE__:   P_AddWeapon(ps, CHAINGUN_WEAPON, 1);   P_AddAmmo(ps, CHAINGUN_WEAPON, 50);   break;
        case SHOTGUNSPRITE__:    P_AddWeapon(ps, SHOTGUN_WEAPON, 1);    P_AddAmmo(ps, SHOTGUN_WEAPON, 10);    break;
        case RPGSPRITE__:        P_AddWeapon(ps, RPG_WEAPON, 1);        P_AddAmmo(ps, RPG_WEAPON, 5);         break;
        case DEVISTATORSPRITE__: P_AddWeapon(ps, DEVISTATOR_WEAPON, 1); P_AddAmmo(ps, DEVISTATOR_WEAPON, 30); break;
        case FREEZESPRITE__:     P_AddWeapon(ps, FREEZE_WEAPON, 1);     P_AddAmmo(ps, FREEZE_WEAPON, 99);     break;
        case SHRINKERSPRITE__:   P_AddWeapon(ps, SHRINKER_WEAPON, 1);   P_AddAmmo(ps, SHRINKER_WEAPON, 5);    break;
        case GROWSPRITEICON__:   P_AddWeapon(ps, GROW_WEAPON, 1);       P_AddAmmo(ps, GROW_WEAPON, 5);        break;
        case TRIPBOMBSPRITE__:   P_AddWeapon(ps, TRIPBOMB_WEAPON, 1);   P_AddAmmo(ps, TRIPBOMB_WEAPON, 3);    break;
        case HEAVYHBOMB__:       P_AddWeapon(ps, HANDBOMB_WEAPON, 1);   P_AddAmmo(ps, HANDBOMB_WEAPON, 1);    break;

        case AMMO__:             P_AddAmmo(ps, PISTOL_WEAPON, 48);      break;
        case SHOTGUNAMMO__:      P_AddAmmo(ps, SHOTGUN_WEAPON, 10);     break;
        case BATTERYAMMO__:      P_AddAmmo(ps, CHAINGUN_WEAPON, 50);    break;
        case AMMOLOTS__:         P_AddAmmo(ps, CHAINGUN_WEAPON, 50);    break;
        case RPGAMMO__:          P_AddAmmo(ps, RPG_WEAPON, 5);          break;
        case HBOMBAMMO__:        P_AddAmmo(ps, HANDBOMB_WEAPON, 1);     break;
        case CRYSTALAMMO__:      P_AddAmmo(ps, SHRINKER_WEAPON, 5);     break;
        case DEVISTATORAMMO__:   P_AddAmmo(ps, DEVISTATOR_WEAPON, 30);  break;
        case FREEZEAMMO__:       P_AddAmmo(ps, FREEZE_WEAPON, 49);      break;
        case GROWAMMO__:         P_AddAmmo(ps, GROW_WEAPON, 20);        break;

        case ATOMICHEALTH__:     sprite[spid].extra = min<int>(sprite[spid].extra + 50, 200); break;
        case SIXPAK__:           sprite[spid].extra = min<int>(max<int>(sprite[spid].extra, 0) + 30, 100); break;
        case COLA__:             sprite[spid].extra = min<int>(max<int>(sprite[spid].extra, 0) + 10, 100); break;
        case SHIELD__:           ps->inv_amount[GET_SHIELD]   = min<int>(ps->inv_amount[GET_SHIELD] + 100, 100); break;
        case FIRSTAID__:         ps->inv_amount[GET_FIRSTAID] = 100;  break;
        case STEROIDS__:         ps->inv_amount[GET_STEROIDS] = 400;  break;
        case JETPACK__:          ps->inv_amount[GET_JETPACK]  = 1600; break;
        case HOLODUKE__:         ps->inv_amount[GET_HOLODUKE] = 1600; break;
        case HEATSENSOR__:       ps->inv_amount[GET_HEATS]    = 1200; break;
        case BOOTS__:            ps->inv_amount[GET_BOOTS]    = 200;  break;
        case AIRTANK__:          ps->inv_amount[GET_SCUBA]    = 6400; break;
        default: break;
    }
}
// A locally-grabbed item is suppressed (kept deleted) until this plc, so the
// host's continued streaming of it -- the host may still think it's there,
// e.g. a BFG risen out of its reach on a platform -- does not un-delete it.
static int32_t s_itemConsumedUntil[MAXSPRITES];
static int16_t s_itemConsumedPic[MAXSPRITES];
// GUEST: live enemies GLIDE onto streamed positions instead of hard-snapping.
// The local CON keeps simulating them (animation/attacks stay alive) while the
// host's kinematic records land every ~5 tics; teleporting to each record made
// the tug-of-war visible -- WORST on flying monsters, which have no floor pin:
// local zvel integration + an out-of-phase hover bob turn every snap into a
// vertical sawtooth ("strange movement when a monster is flying"). Mid-range
// errors now lerp 25%/tic (settled in ~8 tics); big jumps (teleporter, respawn)
// and all non-kinematic transitions still snap exactly.
static vec3_t  s_enGlideTgt[MAXSPRITES];
static uint8_t s_enGlideOn[MAXSPRITES];

// Ticks a pickup has been continuously in grab range. The scan must be the
// FALLBACK, not the winner: item pickups are CON actors whose own script plays
// the quote/voice and runs addweapon (the raise animation) on touch -- the scan
// grabbing instantly beat that ~6-tic CON delay every time, so guests got raw
// silent grants ("pickup notifications and weapon change animations missing").
// Waiting ~10 tics lets the local CON pickup fire with full vanilla feedback;
// the scan then only cleans up what CON could not reach (height gap, host race).
static uint8_t s_pickupNearAge[MAXSPRITES];

static void Net_ClientCreditPickup(int idx)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex == connecthead)
        return;                                // host runs the real pickup itself
    auto const ps = g_player[myconnectindex].ps;
    if (ps == NULL || (unsigned)ps->i >= MAXSPRITES || ps->dead_flag
        || !Bot_IsPickup(sprite[idx].picnum))
        return;
    int32_t const d = klabs(sprite[idx].x - ps->pos.x) + klabs(sprite[idx].y - ps->pos.y);
    if (d > 1536)
        return;                                // not near it: someone else took it
    Net_GrantPickup(ps, sprite[idx].picnum);
}

// CLIENT-AUTHORITATIVE pickup (guest). Waiting for the host to DELETE an item
// cannot work when the host disagrees it was collected -- the user's BFG that
// rises on a platform: the host thinks it is out of reach (height), never
// registers the pickup, so the guest that walked onto it (on its own screen)
// never gets it. So the guest grabs its OWN pickups by HORIZONTAL proximity,
// tolerant of the vertical disagreement, and suppresses the item locally so
// the host's continued stream of it does not resurrect it before it respawns.
static void Net_ClientPickupScan(void)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex == connecthead)
        return;
    auto const ps = g_player[myconnectindex].ps;
    if (ps == NULL || (unsigned)ps->i >= MAXSPRITES || ps->dead_flag
        || (unsigned)ps->cursectnum >= (unsigned)numsectors)
        return;
    int16_t sects[24]; int ns = 0;
    sects[ns++] = ps->cursectnum;
    int const wend = sector[ps->cursectnum].wallptr + sector[ps->cursectnum].wallnum;
    for (int w = sector[ps->cursectnum].wallptr; w < wend && ns < 24; w++)
    {
        int const nx = wall[w].nextsector;
        if (nx < 0 || (unsigned)nx >= (unsigned)numsectors) continue;
        int dup = 0; for (int q = 0; q < ns; q++) if (sects[q] == nx) { dup = 1; break; }
        if (!dup) sects[ns++] = (int16_t)nx;
    }
    for (int q = 0; q < ns; q++)
        for (int jj = headspritesect[sects[q]]; jj >= 0; )
        {
            int const nextjj = nextspritesect[jj];
            extern uint16_t g_coopWeapGrab[MAXSPRITES];
            if ((unsigned)jj < MAXSPRITES && Bot_IsPickup(sprite[jj].picnum)
                && !(sprite[jj].cstat & 32768)
                // Coop weapon-stay: if our grab-bit is set on this sprite, the
                // local CON already REFUSED it (this life took it) -- the raw
                // fallback must not overrule that (it was a silent ammo farm:
                // stand on any taken weapon 10 tics and it re-granted). The bit
                // clears on respawn (P_ResetWeapons), so a fresh life re-takes
                // it through the normal CON path with full feedback.
                && !((g_gametypeFlags[ud.coop] & GAMETYPE_WEAPSTAY)
                     && (g_coopWeapGrab[jj] & (1u << myconnectindex))))
            {
                int32_t const dh = klabs(sprite[jj].x - ps->pos.x) + klabs(sprite[jj].y - ps->pos.y);
                // Horizontal grab; VERY tolerant vertically (a full lift of
                // travel) so the rising-platform height gap never blocks it.
                if (dh <= 896 && klabs(sprite[jj].z - ps->pos.z) <= (200 << 8))
                {
                    // FALLBACK ONLY (see s_pickupNearAge): give the item's own CON
                    // pickup -- quote, voice, addweapon raise -- ~10 tics to fire
                    // first; if the item is still here, CON couldn't reach it
                    // (height gap / host race) and we grab it raw.
                    if (s_pickupNearAge[jj] < 10)
                    {
                        s_pickupNearAge[jj]++;
                    }
                    else
                    {
                        s_pickupNearAge[jj] = 0;
                        Net_GrantPickup(ps, sprite[jj].picnum);
                        s_itemConsumedUntil[jj] = movefifoplc + 360;   // ~12s hide
                        s_itemConsumedPic[jj]   = sprite[jj].picnum;
                        A_DeleteSprite(jj);
                    }
                }
                else
                    s_pickupNearAge[jj] = 0;
            }
            jj = nextjj;
        }
}
// GUEST cosmetic FX for host-side detonations/gibs. Explosion visuals and gibs are
// STAT_MISC -- the stream deliberately skips them -- so the guest only ever SEES an
// explosion its own local sim causes. But damage is host-side: a chain set off by
// the host player (or by this guest's REPORTED barrel shot, or host radius damage)
// exists only on the host, and the chained barrels/C9/bombs arrive here as bare
// DELETE records -- they silently vanish, no flash, no boom ("chain explosion
// graphics not triggering"). So: when a delete lands on a LOCAL copy that is still
// intact and is a detonator-class sprite, play the explosion HERE as pure
// cosmetics -- EXPLOSION2 + sound, NO radius damage, no gameplay. If the local sim
// already chained it, the local copy is already gone (killit) and the record hits
// an empty slot -> no double. Own bombs/rockets are skipped (owner == self): the
// local sim already played those. A LIVE enemy deleted outright is a host-side GIB
// kill that outran the health stream (killit lands same-tic) -> play guts+squish so
// RPG chain kills don't just blink out.
static void Net_ClientDeleteFX(int idx, bool wasEnemy, int prevExtra)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex == connecthead)
        return;
    if (sprite[idx].statnum == STAT_MISC || sprite[idx].statnum == STAT_EFFECTOR)
        return;                                   // cosmetics/effectors: never ours to dress
    // NO owner==self skip: the "local sim already played it" case is exactly the
    // case where the local copy is ALREADY GONE (it detonated/retired itself), so
    // the intact-copy check below self-deduplicates. Live-caught race: the host's
    // detonation delete can land BEFORE the guest's own detonate tic consumes --
    // the bomb vanished un-exploded and the self-skip then ate the only FX
    // ("pipe bomb explosion missing when I trigger it").
    if (wasEnemy && prevExtra > 0)
    {
        A_DoGuts(idx, JIBS6, 3);                  // host gibbed a monster our copy held alive
        A_PlaySound(SQUISHED, idx);
        return;
    }
    switch (tileGetMapping(sprite[idx].picnum))
    {
        case HEAVYHBOMB__:
            // Same tile is also the pipebomb PICKUP: only a THROWN bomb (owner is
            // a player sprite) detonates; a consumed pickup just vanishes quietly.
            if ((unsigned)sprite[idx].owner >= MAXSPRITES
                || sprite[sprite[idx].owner].picnum != APLAYER)
                break;
            fallthrough__;
        case EXPLODINGBARREL__:
        case SEENINE__:
        case OOZFILTER__:
        case TRIPBOMB__:
        case RPG__:                               // a host-fired rocket's impact
        {
            int const e = A_Spawn(idx, EXPLOSION2);
            if (e >= 0)
                sprite[e].ang = sprite[idx].ang;
            A_PlaySound(PIPEBOMB_EXPLODE, idx);
            break;
        }
        default:
            break;
    }
}

// REPLAY-KILL (guest). The guest runs enemy CON locally so monsters animate,
// attack, and can play a REAL death -- but it deals no damage (A_DamageObject is
// host-gated), so its CON can never reach the dead state on its own. When a stream
// record shows an enemy dead, arm a pending lethal hit (htextra/htpicnum): the
// guest's next A_Execute consumes it via ifhitweapon -> A_IncurDamage -> ifdead and
// runs the full death -- anim, gibs, sound -- within a tic of the host's kill.
// s_hostDead is the one-shot guard (cleared on extra>0 / delete / fresh slot /
// level entry so a reused index dies again properly).
//
// SUBTLE + LOAD-BEARING: A_IncurDamage BAILS (and eats the pending hit) when the
// sprite's extra is already NEGATIVE (actors.cpp) -- and the host's don't-clamp
// means streamed death extras ARE negative. So while the replay is pending we pin
// the local extra at 0; once the CON consumes the hit (htextra goes -1), stream
// records may apply anything.
// prevExtra = the LOCAL extra this sprite had BEFORE the record was applied. The
// replay fires only on a genuine alive->dead transition (prevExtra > 0): map
// actors whose extra is the unset default (-1 on host AND guest -- E1L1's RATs)
// must not be "killed" at join, and an already-dead local copy has nothing to
// replay. A fresh materialization of a dead enemy passes prevExtra=1 ("assume it
// lived") so late-join corpses drop instead of standing.
static inline void Net_LatchHostDead(int idx, int extra, bool isEnemy, int prevExtra)
{
    if (extra > 0)
        s_hostDead[idx] = 0;
    // Exclude only TRUE freezer statues (pal 1 AND extra exactly 0 -- the engine's
    // own frozen test). A blanket pal!=1 skipped every MULTIPLAYER-ONLY monster:
    // pal-marked map enemies are Duke's MP-extra spawn mechanism (SP deletes them,
    // MP keeps them WITH their pal), so their deaths never replayed on the guest --
    // "the guest already killed them but they don't die".
    else if (isEnemy && !(sprite[idx].pal == 1 && extra == 0))
    {
        if (!s_hostDead[idx] && prevExtra > 0)
        {
            s_hostDead[idx] = 1;
            actor[idx].htextra  = 1;                       // pending lethal hit
            actor[idx].htpicnum = SHOTSPARK1;              // plain death (RPG gibs arrive as host deletes)
            if (g_player[myconnectindex].ps != NULL)
                actor[idx].htowner = g_player[myconnectindex].ps->i;
            LOG_F(INFO, "[replaykill] idx=%d pic=%d extra=%d", idx, (int)sprite[idx].picnum, extra);
        }
        if (s_hostDead[idx] && actor[idx].htextra >= 0 && sprite[idx].extra < 0)
            sprite[idx].extra = 0;                         // keep the pending hit consumable
    }
}

// Stream-mode guest: enemy DEATH is host-authoritative. The guest's local sim
// applies damage for instant feedback (pain, blood, stagger) and the stream
// repaints extra with host truth within a lap, so ordinary chip damage
// self-heals -- but the CON death transition is IRREVERSIBLE, so the killing
// blow may only land via the host's replay-kill latch (s_hostDead). The old
// "guests deal no damage" gate was `if (g_netClient) return;` in
// A_DamageObject -- an ENET object that is NULL in stream mode, so local kills
// ran host-blind: when the host's copy survived (krand damage fuzz, real-ping
// position skew, blast obstruction), the guest kept a permanent corpse whose
// authoritative original fought on ("attacked by already dead monsters",
// 2026-08-16). A_IncurDamage calls this to clamp lethal local damage to 1hp.
int32_t Net_StreamGuestBlocksLethal(int spriteNum)
{
    return g_netStreamMode && numplayers > 1 && myconnectindex != connecthead
        && !g_netJoinCatchup
        && (unsigned)spriteNum < MAXSPRITES && !s_hostDead[spriteNum];
}

// A live host record (extra>0) over a LOCAL corpse: the guest's copy died by a
// path the clamp doesn't cover (squish, freeze shatter, CON strength writes) or
// predates the clamp, while the authoritative monster is up and fighting. The
// CON death state can't be rewound in place, so the repair is a rebuild at the
// same slot (delete + re-insert = the late-join materialize recipe, full spawn
// init). Grace-gated: a record generated just before the host applied its own
// kill of this monster is stale by at most RTT + stream cadence, so only a
// SUSTAINED contradiction (~2.5s of "host says alive") is a real divergence.
// SPRITE_STREAM rides the reliable-ordered channel, so the records themselves
// cannot reorder into a false contradiction.
// localDead: the CON death is a state transition, not just extra<=0 -- and the
// kinematic sweep re-pumps HOST extra (positive) into the local corpse every
// record, so extra alone stops detecting it after the first repaint. The death
// state's `cstat 0` (corpses stop blocking; live badguys are 257) is the
// durable local signal; prevExtra<=0 covers the tics before the first repaint.
enum { NET_REVIVE_GRACE = 64 };   // movefifoplc tics (~26/s): ~2.5s
static int Net_CorpseReviveDue(int idx, int localDead, int recExtra, bool wasEnemy)
{
    if (!wasEnemy || !localDead || recExtra <= 0 || s_hostDead[idx])
    {
        s_liveMismatchPlc[idx] = 0;
        return 0;
    }
    if (s_liveMismatchPlc[idx] == 0)
    {
        s_liveMismatchPlc[idx] = movefifoplc ? movefifoplc : 1;
        return 0;
    }
    if (movefifoplc - s_liveMismatchPlc[idx] < NET_REVIVE_GRACE)
        return 0;
    s_liveMismatchPlc[idx] = 0;
    return 1;
}

static void Net_ApplySpriteStream(const char *buf, int len)
{
    if (len < 2)
        return;
    int j = 1;
    int const cnt = (uint8_t)buf[j++];
    for (int e = 0; e < cnt; e++)
    {
        if (j + NET_SPRREC_BYTES > len)
            break;
        int      const idx    = (uint16_t)B_UNBUF16(&buf[j]); j += 2;
        uint8_t  const flags  = (uint8_t)buf[j++];
        uint16_t const stat   = (uint16_t)B_UNBUF16(&buf[j]); j += 2;
        int16_t  const picnum = (int16_t)B_UNBUF16(&buf[j]);  j += 2;
        int16_t  const sect   = (int16_t)B_UNBUF16(&buf[j]);  j += 2;
        int32_t  const x      = (int32_t)B_UNBUF32(&buf[j]);  j += 4;
        int32_t  const y      = (int32_t)B_UNBUF32(&buf[j]);  j += 4;
        int32_t  const z      = (int32_t)B_UNBUF32(&buf[j]);  j += 4;
        int16_t  const ang    = (int16_t)(B_UNBUF16(&buf[j]) & 2047); j += 2;
        int16_t  const xvel   = (int16_t)B_UNBUF16(&buf[j]);  j += 2;
        int16_t  const zvel   = (int16_t)B_UNBUF16(&buf[j]);  j += 2;
        uint16_t const cstat  = (uint16_t)B_UNBUF16(&buf[j]); j += 2;
        int16_t  const owner  = (int16_t)B_UNBUF16(&buf[j]);  j += 2;
        int16_t  const extra  = (int16_t)B_UNBUF16(&buf[j]);  j += 2;
        int8_t   const shade  = (int8_t)buf[j++];
        uint8_t  const pal    = (uint8_t)buf[j++];
        uint8_t  const xrep   = (uint8_t)buf[j++];
        uint8_t  const yrep   = (uint8_t)buf[j++];

        if ((unsigned)idx >= MAXSPRITES)
            continue;
        if (Net_IsPlayerSprite(idx))
            continue;                      // the player pack owns those
        // Enemy-ness and LOCAL health of the guest's CURRENT sprite, captured
        // before the record overwrites them -- this pair is what detects a genuine
        // alive->dead transition for the replay-kill below.
        bool const guestWasEnemy = (sprite[idx].statnum < MAXSTATUS) && A_CheckEnemySprite(&sprite[idx]);
        int        guestPrevExtra = (sprite[idx].statnum < MAXSTATUS) ? (int)sprite[idx].extra : 0;
        if ((flags & NET_SPRF_DELETE) || stat == 0xFFFFu)
        {
            if (sprite[idx].statnum < MAXSTATUS)
            {
                Net_ClientCreditPickup(idx);   // guest grabs its own pickups before they vanish
                Net_ClientDeleteFX(idx, guestWasEnemy, guestPrevExtra);   // explosion/gib visuals for host-side detonations
                A_DeleteSprite(idx);
            }
            s_enGlideOn[idx] = 0;
            s_hostDead[idx] = 0;               // slot freed/reused: drop the replay-kill latch
            s_liveMismatchPlc[idx] = 0;
            s_itemConsumedUntil[idx] = 0;      // gone on the host too: stop suppressing
            continue;
        }
        // A pickup this guest already grabbed (client-authoritative): the host
        // may keep streaming it (it still thinks it's there -- the rising BFG),
        // so keep it hidden locally until the suppression window expires.
        if (s_itemConsumedUntil[idx] > movefifoplc && picnum == s_itemConsumedPic[idx])
        {
            if (sprite[idx].statnum < MAXSTATUS)
                A_DeleteSprite(idx);
            continue;
        }
        if (stat >= MAXSTATUS || Net_StreamSkipsStat(stat))
            continue;
        if ((unsigned)sect >= (unsigned)numsectors)
            continue;

        if (flags & NET_SPRF_KINEMATIC)
        {
            // Position/statnum/health refresh ONLY -- identity (picnum,
            // cstat, angle, looks) stays client-owned so a routine sweep can
            // never restart an animation or resurrect a locally-consumed
            // item's look. No materialization without identity.
            if (sprite[idx].statnum >= MAXSTATUS)
                continue;
            if (sprite[idx].statnum != (int16_t)stat)
                changespritestat((int16_t)idx, (int16_t)stat);
            bool const kinLocalDead = (guestPrevExtra <= 0) || !(sprite[idx].cstat & 1);
            sprite[idx].extra = extra;
            if (Net_CorpseReviveDue(idx, kinLocalDead, extra, guestWasEnemy))
            {
                // Rebuild with LOCAL identity (kinematic records carry none) at
                // the record's position: delete + re-insert re-runs the spawn
                // init, un-wedging the CON from its terminal death state.
                int16_t const rvPic = sprite[idx].picnum, rvOwn = sprite[idx].owner;
                int8_t  const rvShd = sprite[idx].shade;
                uint8_t const rvPal = sprite[idx].pal, rvXr = sprite[idx].xrepeat, rvYr = sprite[idx].yrepeat;
                LOG_F(INFO, "[revive] idx=%d pic=%d: host streams it ALIVE (extra=%d) over a local corpse -- rebuilding",
                      idx, (int)rvPic, extra);
                A_DeleteSprite(idx);
                s_enGlideOn[idx] = 0;
                if (Net_RotateFreeSpriteToHead((int16_t)idx) == 0)
                {
                    int const ni = A_InsertSprite(sect, x, y, z, rvPic, rvShd, rvXr, rvYr, ang, xvel, zvel, rvOwn, (int16_t)stat);
                    if (ni == idx)
                    {
                        sprite[idx].extra = extra;
                        sprite[idx].pal   = rvPal;
                        // A_Spawn with no spawner keeps the inserted cstat (0);
                        // the full-record materialize gets cstat from its record,
                        // a kinematic one has none -- set the live-badguy default
                        // ourselves or the rebuild is another "corpse" (cstat 0)
                        // and the revive loops forever.
                        sprite[idx].cstat = 257;
                    }
                    else if (ni >= 0)
                        A_DeleteSprite(ni);   // impossible by construction; stay consistent
                }
                continue;
            }
            vec3_t kp = { x, y, z };
            // LIVE enemy at a mid-range error: glide instead of snapping (see
            // s_enGlideTgt). Everything else -- non-enemies, corpses, tiny
            // errors, teleport-sized jumps -- keeps the exact snap.
            if (guestWasEnemy && extra > 0)
            {
                int64_t const gdx = (int64_t)kp.x - sprite[idx].x, gdy = (int64_t)kp.y - sprite[idx].y;
                int64_t const gdz = ((int64_t)kp.z - sprite[idx].z) >> 4;
                int64_t const gd2 = gdx * gdx + gdy * gdy + gdz * gdz;
                if (gd2 > (int64_t)96 * 96 && gd2 < (int64_t)3072 * 3072)
                {
                    s_enGlideTgt[idx] = kp;
                    s_enGlideOn[idx]  = 1;
                    Net_LatchHostDead(idx, extra, guestWasEnemy, guestPrevExtra);
                    continue;
                }
            }
            s_enGlideOn[idx] = 0;
            setsprite((int16_t)idx, &kp);
            actor[idx].bpos = kp;
            Net_LatchHostDead(idx, extra, guestWasEnemy, guestPrevExtra);
            continue;
        }

        if (sprite[idx].statnum < MAXSTATUS
            && Net_CorpseReviveDue(idx, (guestPrevExtra <= 0) || !(sprite[idx].cstat & 1), extra, guestWasEnemy))
        {
            LOG_F(INFO, "[revive] idx=%d pic=%d: full record ALIVE (extra=%d) over a local corpse -- rematerializing",
                  idx, (int)sprite[idx].picnum, extra);
            A_DeleteSprite(idx);   // falls into the fresh-slot materialize below
            guestPrevExtra = 1;    // it lives on the host; treat any later death as replayable
        }
        if (sprite[idx].statnum >= MAXSTATUS)
        {
            // Fresh slot: unambiguously a NEW object, so drop any stale replay-kill
            // latch left by a previous occupant (a corpse the guest deleted locally
            // before the host reused this index). The latch below re-sets it iff
            // this new sprite is itself a dead enemy -- otherwise a live enemy that
            // reuses the index would never get its death replayed.
            s_hostDead[idx] = 0;
            s_liveMismatchPlc[idx] = 0;
            // Materialize at the host's exact index (freelist rotation makes
            // the engine's next insert claim precisely this slot).
            if (Net_RotateFreeSpriteToHead((int16_t)idx) != 0)
                continue;
            int const ni = A_InsertSprite(sect, x, y, z, picnum, shade, xrep, yrep,
                                          ang, xvel, zvel, owner, (int16_t)stat);
            if (ni != idx)
            {
                // Impossible by construction; keep the world consistent anyway.
                if (ni >= 0)
                    A_DeleteSprite(ni);
                continue;
            }
            // Fresh materialization: "assume it lived" so a corpse arriving whole
            // (late join) gets its death replayed and drops instead of standing.
            guestPrevExtra = 1;
        }
        else if (sprite[idx].statnum != (int16_t)stat)
            changespritestat((int16_t)idx, (int16_t)stat);

        sprite[idx].picnum  = picnum;
        sprite[idx].cstat   = cstat;
        sprite[idx].ang     = ang;
        sprite[idx].xvel    = xvel;
        sprite[idx].zvel    = zvel;
        sprite[idx].owner   = owner;
        sprite[idx].extra   = extra;
        sprite[idx].shade   = shade;
        sprite[idx].pal     = pal;
        sprite[idx].xrepeat = xrep;
        sprite[idx].yrepeat = yrep;
        vec3_t p = { x, y, z };
        setsprite((int16_t)idx, &p);
        actor[idx].bpos = p;
        s_enGlideOn[idx] = 0;   // full record = real transition: the exact snap above stands
        // Replay-kill latch: re-check enemy-ness on the just-applied picnum too so
        // a corpse materialized fresh (the guest never held it alive) is handled.
        Net_LatchHostDead(idx, extra, guestWasEnemy || A_CheckEnemySprite(&sprite[idx]), guestPrevExtra);
    }
}

// Level (re)entry: sprite indices get reused, so drop every stale replay-kill
// latch (else a new enemy at a reused index would never get its death replayed).
// GUEST, per tic (after G_MoveWorld, so it wins over the local sim's move): pull
// gliding enemies toward their streamed targets. bpos keeps the tic-start value so
// the renderer interpolates each step -- the correction reads as drift, not pops.
void Net_GlideEnemies(void)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex == connecthead)
        return;
    for (int i = 0; i < MAXSPRITES; i++)
    {
        if (!s_enGlideOn[i])
            continue;
        if (sprite[i].statnum >= MAXSTATUS || !A_CheckEnemySprite(&sprite[i]) || sprite[i].extra <= 0)
        {
            s_enGlideOn[i] = 0;   // died/removed/transitioned: transitions snap exactly
            continue;
        }
        vec3_t const from = { sprite[i].x, sprite[i].y, sprite[i].z };
        int32_t const dx = s_enGlideTgt[i].x - from.x, dy = s_enGlideTgt[i].y - from.y, dz = s_enGlideTgt[i].z - from.z;
        int64_t const d2 = (int64_t)dx * dx + (int64_t)dy * dy + ((int64_t)dz >> 4) * ((int64_t)dz >> 4);
        vec3_t to;
        if (d2 <= (int64_t)64 * 64)
        {
            to = s_enGlideTgt[i];
            s_enGlideOn[i] = 0;   // settled
        }
        else
            to = { from.x + (dx >> 2), from.y + (dy >> 2), from.z + (dz >> 2) };   // 25%/tic
        actor[i].bpos = from;     // render-interpolate this step
        setsprite((int16_t)i, &to);
    }
}

void Net_StreamClearDeadActors(void)
{
    Bmemset(s_hostDead, 0, sizeof(s_hostDead));
    Bmemset(s_liveMismatchPlc, 0, sizeof(s_liveMismatchPlc));
    Bmemset(s_enGlideOn, 0, sizeof(s_enGlideOn));
    s_accSentMask = s_accBcastMask = 0;   // fresh level = fresh shared key ring
    s_wallEligBuiltPlc = -1;              // door-wall eligibility is per-map
    if (s_eolPending)
    {
        // Level flip: drain everything queued during the bonus screen while
        // the drop guards are still armed (stale pre-flip stream packs must
        // never touch the fresh world), then let live packs through.
        Net_GetPackets();
        s_eolPending = 0;
        LOG_F(INFO, "[eol] entry flush done");
    }
}

static void Net_ApplySectorStream(const char *buf, int len)
{
    if (len < 2)
        return;
    int j = 1;
    int const cnt = (uint8_t)buf[j++];
    for (int e = 0; e < cnt; e++)
    {
        if (j + NET_SECREC_BYTES > len)
            break;
        int     const sct = (uint16_t)B_UNBUF16(&buf[j]); j += 2;
        int32_t const cz  = (int32_t)B_UNBUF32(&buf[j]);  j += 4;
        int32_t const fz  = (int32_t)B_UNBUF32(&buf[j]);  j += 4;
        if ((unsigned)sct >= (unsigned)numsectors)
            continue;
        sector[sct].ceilingz = cz;
        sector[sct].floorz   = fz;
    }
}

// GUEST: absolute wall-vertex paints for wall-motion doors. dragpoint (the same
// primitive the door SEs use) moves the point AND every welded neighbor, so a
// painted swing door carries its frame walls exactly like the host's sim did.
static void Net_ApplyWallStream(const char *buf, int len)
{
    if (len < 2)
        return;
    int j = 1;
    int const cnt = (uint8_t)buf[j++];
    for (int e = 0; e < cnt; e++)
    {
        if (j + NET_WALLREC_BYTES > len)
            break;
        int     const w = (uint16_t)B_UNBUF16(&buf[j]); j += 2;
        int32_t const x = (int32_t)B_UNBUF32(&buf[j]);  j += 4;
        int32_t const y = (int32_t)B_UNBUF32(&buf[j]);  j += 4;
        if ((unsigned)w >= (unsigned)numwalls)
            continue;
        if (wall[w].x != x || wall[w].y != y)
            dragpoint((int16_t)w, x, y, 0);
    }
}

static void Net_ResendNewGameIfUnacked(void)
{
    if (myconnectindex != connecthead || numplayers < 2 || s_newGameLen <= 0)
        return;
    auto const myps = g_player[myconnectindex].ps;
    if (myps == NULL || !(myps->gm & MODE_GAME))
        return;
    int32_t const now = (int32_t)totalclock;
    if (now < s_newGameClock)              // totalclock reset (level transition)
        s_newGameClock = now;
    if (now - s_newGameClock > 3600)       // ~2min: a guest this late re-enters via late join
        return;
    int i;
    TRAVERSE_CONNECT(i)
    {
        if (i == myconnectindex || (g_netBotMask & (1 << i)) || (s_newGameAckMask & (1u << i)))
            continue;
        if (s_newGameResendClock[i] > now)
            s_newGameResendClock[i] = 0;
        if (now - s_newGameResendClock[i] < 120)   // ~1s cadence
            continue;
        s_newGameResendClock[i] = now;
        oldnet_sendpacket(i, (unsigned char *)s_newGameBuf, s_newGameLen);
        initprintf("net: NEW_GAME redelivered to slot %d (no entry ack yet)\n", i);
    }
}

// Divergence response ladder: soft in-place corrections first; the full
// snapshot heal (guest reloads the level) only after repeated failures.
// Returns 0 if some correction was dispatched.
int Net_CorrectDivergence(int k)
{
    if (myconnectindex != connecthead || (unsigned)k >= MAXPLAYERS || !g_player[k].connected)
        return -1;

    int32_t const now = (int32_t)totalclock;
    if (s_softStrikeClock[k] > now || now - s_softStrikeClock[k] > NET_SOFT_DECAY_TICKS)
        s_softStrikes[k] = 0;
    s_softStrikeClock[k] = now;

    g_netDesyncReporters &= ~(1 << k);
    // FRESH JOINER: skip the soft rung entirely. A join transient forks the
    // RNG-coupled PHASE state scattered through the world (animwall tags,
    // drip timers, every krand-absorbed actor field) -- an unbounded class no
    // in-place packet can carry, so the softs are doomed by construction
    // (measured: seed+tag snaps re-forked within a tic, 5s cadence, until the
    // 5th dispatch finally healed). One targeted heal right away turns ~20s
    // of rubber-banding into a single clean catch-up seconds after joining.
    if (s_joinTic[k] >= 0 && movefifoplc - s_joinTic[k] < 1800)   // ~60s post-seat
    {
        LOG_F(WARNING, "net: fresh joiner %d diverged -> straight to heal", k);   // LOG_F: must be visible in native logs (desync gate)
        s_softStrikes[k] = 0;
        return Net_StartHealFlow(k);
    }
    if (++s_softStrikes[k] <= NET_SOFT_STRIKES)
    {
        Net_SendStateSnap(k);
        Net_ResetSyncCheck();   // fresh evidence only, from corrected state
        return 0;
    }
    // Soft corrections are not holding: the worlds have genuinely forked.
    LOG_F(WARNING, "net: soft corrections exhausted for slot %d -> snapshot heal", k);   // LOG_F: must be visible in native logs (desync gate)
    s_softStrikes[k] = 0;
    return Net_StartHealFlow(k);
}

// TARGETED RESYNC: heal ONE diverged guest with the join streamer, minus the
// seat. The snapshot goes to the flagged peer only; veterans stream on
// untouched (deadline-fill covers the healing guest's column while it
// reloads). No epoch bump -- the running generation must keep flowing for
// everyone else, exactly like a join. The old broadcast heal reloaded EVERY
// peer through a barrier: one guest's divergence froze the whole match.
// Returns 0 if the flow started.
int Net_StartHealFlow(int k)
{
    if (myconnectindex != connecthead || s_joinFlowSlot >= 0
        || (unsigned)k >= MAXPLAYERS || k == myconnectindex
        || !g_player[k].connected || numplayers < 2)
        return -1;

    if (Net_SaveLateJoinSnapshot() != 0)
    {
        initprintf("net: heal snapshot save FAILED for slot %d\n", k);
        return -1;
    }

    int seatMask = 0;
    for (int i = 0; i < MAXPLAYERS && i < 16; i++)
        if (g_player[i].connected)
            seatMask |= (1 << i);
    // Same generation, like a join: the target slots back into the RUNNING
    // stream. (The receiver tells heal from join by finding ITSELF in the
    // seat mask -- a join snapshot never includes the joiner.)
    seatMask |= ((int)g_netMoveEpoch) << 16;

    s_joinFlowSlot   = k;
    s_joinFlowIsHeal = 1;
    s_healAckFence   = 0;                    // ignore stale pre-apply acks (see decl)
    s_joinFlowClock  = (int32_t)totalclock;
    s_joinFlowTries  = 0;
    s_joinFlowBase   = movefifoplc;
    s_slaveAck[k]    = movefifoplc;          // resend window rebases to the snapshot tic
    s_lastRealRecvClock[k] = (int32_t)totalclock;
    // Fresh evidence only: whatever this peer reported described the world
    // the snapshot replaces. (Our own latches clear when the flow ends.)
    g_netDesyncReporters &= ~(1 << k);
    desynched_players[k] = 0;
    netmenu_send_snapshot_to(seatMask, k, movefifoplc, 1);
    initprintf("net: targeted heal started for slot %d (snapshot at tic %d)\n", k, movefifoplc);
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[eng] healFlow start p=' + $0 + ' plc=' + $1); }, k, movefifoplc);
#endif
    return 0;
}

// Healing guest: back at the live edge -> leave watcher mode and resume
// playing. Unlike a join there is NO deterministic boundary to honor: our
// membership never changed and our column kept flowing (master fill), so the
// only handoff is local -- restart the sampler at the live cursor and let the
// first real record end the fill. movefifosendplc is the contiguous M2S
// high-water: draining the consume cursor to within a few tics of it IS
// "caught up" (the same measure the host applies to our acks).
void Net_CheckHealResume(void)
{
    if (!g_netJoinCatchup || !g_player[myconnectindex].connected
        || myconnectindex == connecthead || s_healBasePlc < 0)
        return;
    if (movefifoplc <= s_healBasePlc || movefifosendplc - movefifoplc > 8)
    {
        // Catchup progress heartbeat (~2s): a wedged join stalls RIGHT HERE
        // (live-reported 3rd DM seat never seated while the host re-streamed
        // snapshots) -- name the cursors so the wedge is diagnosable.
        static int32_t nextBeat;
        if ((int32_t)totalclock - nextBeat >= 0)
        {
            nextBeat = (int32_t)totalclock + 240;
            LOG_F(INFO, "[join] catching up: plc=%d send=%d base=%d",
                  movefifoplc, movefifosendplc, s_healBasePlc);
        }
        return;
    }

    g_netJoinCatchup = 0;
    s_healBasePlc    = -1;
    screenpeek       = myconnectindex;
    g_netSampleHead  = movefifoplc;
    s_ackOfMyInput   = movefifoplc;
    // The catchup sampler never ran: reset the lag/jitter bookkeeping exactly
    // like the join seat does, or the first timer-nudge jerks totalclock.
    for (int p2 = 0; p2 < MAXPLAYERS; p2++)
        g_player[p2].myminlag = 0x7fffffff;
    mymaxlag = otherminlag = 0;
    Net_InitializePrediction();
    g_netCompareFloorTic = movefifoplc + 30;   // settle grace before compares re-arm
    initprintf("net: heal caught up at tic %d -- resuming play\n", movefifoplc);
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[eng] healResume plc=' + $0); }, movefifoplc);
#endif
}

#endif  // transport track

void Net_PeerEvent(int peerToken, int eventType)
{
    if ((unsigned)peerToken >= MAXPLAYERS)
        return;

    // LATE JOIN: a peer-up for a NEW peer while we are IN GAME must not touch the
    // connect chain here -- this runs inside net_poll, possibly mid-simulation, and
    // growing numplayers under the tic loop starved it on input from a peer not yet
    // in the level (live-reported host freeze). Instead RECORD the joiner; the host
    // seats it at a safe frame point (M_DisplayMenus NETMENU block) by relaunching
    // the current map for everyone, so all peers re-enter tic 0 together.
    if (eventType == NET_PEER_UP && !g_player[peerToken].connected
        && g_player[myconnectindex].ps != NULL
        && (g_player[myconnectindex].ps->gm & MODE_GAME))
    {
        g_netLateJoinMask |= (1 << peerToken);
        initprintf("net: late join queued for slot %d (relaunch pending)\n", peerToken);
        return;
    }

    if (eventType == NET_PEER_DOWN && peerToken == connecthead && myconnectindex != connecthead)
    {
        Net_SetLocalBot(0);
        g_netHostGone = 1; // guest lost its host -> menus.cpp consumer exits to the main menu
    }

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
    // A joiner died before it was seated (still queued, or mid-flow). Before
    // the seat is stamped, nobody's world knows the player: cancel silently.
    // After the stamp, every sim WILL seat the ghost at the joinTic (the
    // announcement is already in flight and must stay deterministic) -- queue
    // the peer-down so the fold drops it right after it seats.
    if (eventType == NET_PEER_DOWN && myconnectindex == connecthead
        && !g_player[peerToken].connected
        && ((g_netLateJoinMask & (1 << peerToken)) || peerToken == s_joinFlowSlot))
    {
        g_netLateJoinMask &= ~(1 << peerToken);
        if (peerToken == s_joinFlowSlot)
        {
            if (s_joinTic[peerToken] < 0)
            {
                initprintf("net: joiner %d left mid-catchup; join cancelled\n", peerToken);
                s_joinFlowSlot = -1;
            }
            else
            {
                s_peerDownMask |= (1 << peerToken);
                initprintf("net: joiner %d left after seat stamp; drop queued behind the seat\n", peerToken);
            }
        }
        else
            initprintf("net: queued joiner %d left before its flow started\n", peerToken);
        return;
    }

    // Mid-game SLAVE loss on the MASTER: never resize the session here -- this
    // runs inside net_poll, possibly mid-simulation, and each peer would apply
    // it at a different tic. Record it; Net_CheckPeerHealth folds it into a
    // goneTic drop that every peer excises at the SAME tic.
    if (eventType == NET_PEER_DOWN && myconnectindex == connecthead
        && peerToken != myconnectindex && g_player[peerToken].connected
        && g_player[myconnectindex].ps != NULL
        && (g_player[myconnectindex].ps->gm & MODE_GAME))
    {
        s_peerDownMask |= (1 << peerToken);
        initprintf("net: peer %d down mid-game; deterministic drop queued\n", peerToken);
        return;
    }
#endif

    g_player[peerToken].connected = (eventType == NET_PEER_UP) ? 1 : 0;
    Net_RebuildConnectChain();
}

void Net_SetLocalIndex(int slot)
{
    if ((unsigned)slot >= MAXPLAYERS)
        return;

    // The local player's IDENTITY moves to the host-assigned slot -- but the
    // pre-game UI mode bits (gm: MODE_MENU/MODE_TYPE) are PER-PLAYER state.
    // The new slot's player starts with gm==0, so switching without carrying
    // gm across silently closes the joiner's menu: M_DisplayMenus early-outs
    // on (gm & MODE_MENU)==0 and NetMenu_OnJoined's MODE_MENU gate then skips
    // the advance to the lobby. The joiner ends up staring at the background
    // wallpaper while fully joined on the wire.
    int const prev = myconnectindex;
    // Slots >= 2 have no ps allocated on a fresh boot: without this, the gm
    // carry below SILENTLY skipped and every later
    // g_player[myconnectindex].ps->gm deref read the null page -- the
    // M_DisplayMenus NETMENU consumers (snapshot apply included) ran on
    // garbage mode bits and a mid-game joiner's apply starved for minutes
    // (soak-caught in the 3-player latejoin run).
    G_MaybeAllocPlayer(slot);
    myconnectindex = screenpeek = slot;
    g_player[slot].connected    = 1;
    if (slot != prev && g_player[slot].ps != NULL && g_player[prev].ps != NULL
        && !(g_player[slot].ps->gm & MODE_GAME))
    {
        g_player[slot].ps->gm = g_player[prev].ps->gm;
        g_player[prev].ps->gm = 0;
    }
    Net_RebuildConnectChain();
}

// Snapshot netcode sent a combined "client info" packet on join; the lockstep
// model sends name + options as its own reliable packets.
void Net_SendClientInfo(void)
{
    Net_SendPlayerName();
    Net_SendPlayerOptions();
}

void Net_SendQuit(void)
{
    if (netQuitSend)
    {
        //game.cpp@app_crashhandler() calls Net_SendQuit(). It has been called already when the crash has happen within Net_SendQuit().
        //Abort the second call
        return;
    }
    netQuitSend = 1;
    if (g_gameQuit == 0 && (numplayers > 1))
    {
        if (g_player[myconnectindex].ps->gm & MODE_GAME)
        {
            g_gameQuit = 1;
            quittimer = (int32_t)totalclock+(CLOCKTICKSPERSECOND*2);
        }
        else
        {
            int i;

            tempbuf[0] = PACKET_TYPE_MENU_LEVEL_QUIT;
            tempbuf[1] = myconnectindex;

            TRAVERSE_CONNECT(i)
            {
                if (i != myconnectindex) oldnet_sendpacket(i, (unsigned char*)tempbuf,2);
                if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
            }
            G_GameExit(" ");
        }
    }
    else if (numplayers < 2)
        G_GameExit(" ");

    if ((totalclock > quittimer) && (g_gameQuit == 1))
        G_GameExit("Timed out.");
}

void Net_SendWeaponChoice(void)
{
    int i,l;

    buf[0] = PACKET_TYPE_WEAPON_CHOICE;
    buf[1] = myconnectindex;
    l = 2;

    for (i=0;i<10;i++)
    {
        g_player[myconnectindex].wchoice[i] = g_player[0].wchoice[i];
        buf[l++] = (char)g_player[0].wchoice[i];
    }

    TRAVERSE_CONNECT(i)
    {
        if (i != myconnectindex) oldnet_sendpacket(i, (unsigned char*)&buf[0],l);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }
}

void Net_SendVersion(void)
{
    int i;

    if (numplayers < 2) return;

    buf[0] = PACKET_TYPE_VERSION;
    buf[1] = myconnectindex;
    buf[2] = (char)atoi(s_buildRev);
    buf[3] = BYTEVERSION;

    TRAVERSE_CONNECT(i)
    {
        if (i != myconnectindex) oldnet_sendpacket(i, (unsigned char*)&buf[0],4);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }

    Net_WaitForPlayers();
}

void Net_SendPlayerOptions(void)
{
    int i,l;

    buf[0] = PACKET_TYPE_PLAYER_OPTIONS;
    buf[1] = myconnectindex;
    l = 2;

    //null terminated player name to send
//    for (i=0;szPlayerName[i];i++) buf[l++] = Btoupper(szPlayerName[i]);
//    buf[l++] = 0;

    buf[l++] = g_player[myconnectindex].ps->auto_aim = ud.config.AutoAim;
    buf[l++] = g_player[myconnectindex].ps->weaponswitch = ud.weaponswitch;
    // Local color is menu-constrained (MEOSV_PLAYER_COLOR) and always valid, so it
    // needs no validation; the deferred validator (playerColor_getValidPal) applies
    // only to untrusted REMOTE colors on the recv path (~L740). Routing the local
    // color through it fired the "player-color validation not ported" ERROR at SP boot.
    buf[l++] = g_player[myconnectindex].ps->palookup = g_player[myconnectindex].pcolor = ud.color;
    buf[l++] = g_player[myconnectindex].pteam = ud.team;

    TRAVERSE_CONNECT(i)
    {
        if (i != myconnectindex) oldnet_sendpacket(i, (unsigned char*)&buf[0],l);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }
}

void Net_SendFragLimit(void)
{
    if (myconnectindex != connecthead)
        return;

    packbuf[0] = PACKET_TYPE_FRAGLIMIT_CHANGED;
    packbuf[1] = ud.fraglimit;

    int32_t i;
    TRAVERSE_CONNECT(i)
    {
        if (i != myconnectindex) oldnet_sendpacket(i, (unsigned char*)packbuf, 2);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }
}

// TODO: Give player number parameter. (Useful for sending bot names)
void Net_SendPlayerName(void)
{
    int i,l;

    for (l=0;(unsigned)l<sizeof(szPlayerName)-1;l++)
        g_player[myconnectindex].user_name[l] = Btoupper(szPlayerName[l]);

    if (numplayers < 2)
        return;

    buf[0] = PACKET_TYPE_PLAYER_NAME;
    buf[1] = myconnectindex;
    l = 2;

    //null terminated player name to send
    for (i=0;szPlayerName[i];i++) buf[l++] = Btoupper(szPlayerName[i]);
    buf[l++] = 0;

    TRAVERSE_CONNECT(i)
    {
        if (i != myconnectindex) oldnet_sendpacket(i, (unsigned char*)&buf[0],l);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }
}

void Net_SendUserMapName(void)
{
    if (numplayers > 1)
    {
        int j;
        int ch;

        packbuf[0] = PACKET_TYPE_USER_MAP;
        packbuf[1] = 0;

        Bcorrectfilename(boardfilename,0);

        j = Bstrlen(boardfilename);
        boardfilename[j++] = 0;
        Bstrcat(packbuf+1,boardfilename);

        TRAVERSE_CONNECT(ch)
        {
            if (ch != myconnectindex) oldnet_sendpacket(ch, (unsigned char*)packbuf,j);
            if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
        }
    }
}

void Net_SendInitialSettings(void)
{
    if (myconnectindex != connecthead) // First player dictates initial settings.
        return;

    packbuf[0] = PACKET_TYPE_INIT_SETTINGS;

    int32_t j = 1;
    packbuf[j++] = ud.m_level_number;
    packbuf[j++] = ud.m_volume_number;
    packbuf[j++] = ud.m_player_skill;
    packbuf[j++] = ud.m_coop;
    packbuf[j++] = ud.warp_on;
    packbuf[j++] = ud.fraglimit;
    packbuf[j++] = ud.multimode;
    packbuf[j++] = ud.playerai;
    B_BUF32(&packbuf[j], ud.m_dmflags); j += sizeof(int32_t);
    B_BUF32(&packbuf[j], botNameSeed); j += sizeof(int32_t);

    int i;
    TRAVERSE_CONNECT(i)
    {
        if (i != myconnectindex)
            oldnet_sendpacket(i, (unsigned char*)packbuf, j);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }
}

void Net_SendNewGame(uint32_t flags)
{
    if (numplayers < 2 || myconnectindex != connecthead) // Only hosts should be allowed to do this.
        return;

    packbuf[0] = PACKET_TYPE_NEW_GAME;
    
    int32_t j = 1;
    packbuf[j++] = ud.m_level_number;
    packbuf[j++] = ud.m_volume_number;
    packbuf[j++] = ud.m_player_skill;
    packbuf[j++] = ud.m_coop;
    B_BUF32(&packbuf[j], ud.m_dmflags); j += sizeof(int32_t);
#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
    // Transport track: the high 16 bits of `flags` carry the SEAT MASK -- which
    // slots are in the session. Guests need it because their only direct peer is
    // the host (STAR): without it a guest's numplayers stays 2 and a 3-player
    // launch (or any late join) desyncs. Stock NetDuke32 senders leave these bits
    // 0 and the stock receiver ignores them, so the wire stays compatible.
    {
        uint32_t seatMask = 0;
        for (int32_t k = 0; k < MAXPLAYERS && k < 16; k++)
            if (g_player[k].connected)
                seatMask |= (1u << k);
        flags |= seatMask << 16;
    }
#endif
    B_BUF32(&packbuf[j], flags);        j += sizeof(int32_t);
    // Per-launch token: lets guests dedupe redeliveries (see the resend
    // machinery) -- a resend can never restart an already-entered level.
    packbuf[j++] = (char)++s_newGameToken;
    // Fixed-width authoritative team vector. CPU seats cannot send PLAYER_OPTIONS,
    // and human option packets may race launch, so NEW_GAME owns the complete
    // pre-entry team state. Exactly 16 bytes keeps the extension deterministic;
    // receivers accept legacy packets that end immediately after the token.
    for (int32_t k = 0; k < NET_TEAM_VECTOR_SIZE; k++)
    {
        int32_t const team = (k < MAXPLAYERS) ? g_player[k].pteam : 0;
        packbuf[j++] = (char)Net_ClampTeam(team);
    }

    // Stash for redelivery to slow-booting guests (join_ok -> launch can beat
    // a real browser's engine boot; the missed packet = an empty world).
    s_newGameLen = min<int>(j, (int)sizeof(s_newGameBuf));
    Bmemcpy(s_newGameBuf, packbuf, s_newGameLen);
    s_newGameClock   = (int32_t)totalclock;
    s_newGameAckMask = 0;
    for (int k = 0; k < MAXPLAYERS; k++)
        s_newGameResendClock[k] = 0;

    int i;
    TRAVERSE_CONNECT(i)
    {
        if (i != myconnectindex) oldnet_sendpacket(i, (unsigned char*)packbuf,j);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }
}

// STREAM MODE: the host is the only authority on level transitions. Called at
// the host's MODE_EOL consumption (game.cpp main loop) BEFORE the bonus/enter
// flow, so every guest flips levels WITH us. The classic build never sent
// PACKET_TYPE_EOL (lockstep peers all hit the exit on the same tic); stream
// guests don't share that symmetry -- 2026-08-15 a guest-side exit split the
// session (guest transitioned alone, host input-starved in the old level,
// match tore down to the lobby). ud.* progression is already computed by the
// trigger (P_EndLevel / fist secret exit / CON) when this runs.
void Net_SendEol(void)
{
    if (!g_netStreamMode || numplayers < 2 || myconnectindex != connecthead)
        return;
    char buf[6];
    buf[0] = PACKET_TYPE_EOL;
    buf[1] = (char)ud.level_number;
    buf[2] = (char)ud.from_bonus;
    buf[3] = (char)ud.secretlevel;
    buf[4] = (char)ud.volume_number;
    buf[5] = (char)ud.eog;
    int32_t i;
    TRAVERSE_CONNECT(i)
        if (i != myconnectindex)
            oldnet_sendpacket(i, (unsigned char *)buf, sizeof(buf));
    LOG_F(INFO, "[eol] host broadcast: next E%dL%d from_bonus=%d secret=%d eog=%d",
          ud.volume_number + 1, ud.level_number + 1, ud.from_bonus, ud.secretlevel, (int)ud.eog);
    // EPOCH FENCE: readiness is per level entry, not lifetime-cumulative. The
    // master echo + release broadcast raise a guest's copy of OUR flag by ~2
    // per crossing while its own flag gains 1, so the cumulative counters
    // inflate until every barrier releases instantly -- by the second
    // transition guests free-ran into the new level while the host was still
    // loading (live-reported: no wait screen, first snapshot yanks the world).
    // This EOL packet rides the reliable-ordered channel, so no new-entry
    // READY can cross the fence in either direction: guests only send after
    // receiving it, and we only broadcast after zeroing here.
    for (i = 0; i < MAXPLAYERS; i++)
        g_player[i].playerreadyflag = 0;
    // Every seat now unloads its world: hold the silence axe for all of them
    // until their first post-transition record (or NET_EOL_GRACE).
    s_eolWaitMask = 0;
    TRAVERSE_CONNECT(i)
        if (i != myconnectindex && !(g_netBotMask & (1 << i)))
            s_eolWaitMask |= (1 << i);
    s_eolGraceClock = (int32_t)totalclock;
    Net_TestSlowLoad("host");
}

void Net_EndOfLevel(bool secret)
{
    if (/*(myconnectindex != connecthead) ||*/ oldnet_predicting)
        return;

    g_player[myconnectindex].ps->gm = MODE_EOL;

    if (secret)
    {
        ud.from_bonus = ud.level_number + 1;

        if (ud.secretlevel > 0 && ud.secretlevel < MAXLEVELS)
            ud.level_number = ud.secretlevel - 1;
    }
    else
    {
        if (ud.from_bonus)
        {
            ud.level_number = ud.from_bonus;
            ud.from_bonus = 0;
        }
        else
        {
            ud.level_number++;

            if (ud.level_number > MAXLEVELS - 1)
                ud.level_number = 0;
        }
    }

#if 0 // [JM] Disabled for Duke64 TC compat. Might break other things, but here's hoping it does not.
    packbuf[0] = PACKET_TYPE_EOL;
    packbuf[1] = ud.level_number;
    packbuf[2] = ud.from_bonus;
    packbuf[3] = ud.secretlevel;

    int i;
    TRAVERSE_CONNECT(i)
    {
        if (i != myconnectindex) oldnet_sendpacket(i, (unsigned char*)packbuf, 4);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }
#endif
}

void Net_EnterMessage(void)
{
    short ch, hitstate, i, j, l;

    if (g_player[myconnectindex].ps->gm & MODE_SENDTOWHOM)
    {
        if (g_chatPlayer != -1 || ud.multimode < 3)
        {
            tempbuf[0] = PACKET_TYPE_MESSAGE;
            tempbuf[2] = 0;
            recbuf[0]  = 0;

            if (ud.multimode < 3)
                g_chatPlayer = 2;

            if (typebuf[0] == '/' && Btoupper(typebuf[1]) == 'M' && Btoupper(typebuf[2]) == 'E')
            {
                i = 3, j = Bstrlen(typebuf);
                Bstrcpy(tempbuf,typebuf);
                while (i < j)
                {
                    typebuf[i-3] = tempbuf[i];
                    i++;
                }
                typebuf[i-3] = '\0';
                Bsprintf(recbuf, "^07* %s^07", g_player[myconnectindex].user_name);
            }
            else
            {
                Bsprintf(recbuf, "%s^07: ", g_player[myconnectindex].user_name);
            }

            Bstrcat(recbuf,typebuf);
            j = Bstrlen(recbuf);
            recbuf[j] = 0;
            Bstrcat(tempbuf+2,recbuf);

            if (g_chatPlayer >= ud.multimode)
            {
                ChatPipe_SendMessage(typebuf);
                tempbuf[1] = 255;

                TRAVERSE_CONNECT(ch)
                {
                    if (ch != myconnectindex) oldnet_sendpacket(ch, (unsigned char*)tempbuf,j+2);
                    if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
                }

                G_AddUserQuote(recbuf);
                // (1) DEFERRED: on-screen chat quote scroll. quotebot/quotebotgoal/
                // G_GameTextLen live in netduke32's HUD-text subsystem (not ported).
                // The chat message itself was already sent over the wire above.
                NETDUKE32_MP_TODO("chat quote scroll");
            }
            else if (g_chatPlayer >= 0)
            {
                tempbuf[1] = (char)g_chatPlayer;
                if (myconnectindex != connecthead)
                    g_chatPlayer = connecthead;
                oldnet_sendpacket(g_chatPlayer, (unsigned char*)tempbuf,j+2);
            }

            g_chatPlayer = -1;
            g_player[myconnectindex].ps->gm &= ~(MODE_TYPE|MODE_SENDTOWHOM);
        }
        else if (g_chatPlayer == -1)
        {
            // (1) DEFERRED: on-screen "SEND MESSAGE TO..." recipient list rendering
            // (gametext/minitext/mpgametext at drifted signatures; HUD-text subsystem
            // not ported). Recipient selection via the keyboard below still functions.
            NETDUKE32_MP_TODO("chat recipient-select UI");

            if (KB_KeyWaiting())
            {
                i = KB_GetCh();

                if (i == 'A' || i == 'a' || i == 13)
                    g_chatPlayer = ud.multimode;
                else if (i >= '1' || i <= (ud.multimode + '1'))
                    g_chatPlayer = i - '1';
                else
                {
                    g_chatPlayer = ud.multimode;
                    if (i == 27)
                    {
                        g_player[myconnectindex].ps->gm &= ~(MODE_TYPE|MODE_SENDTOWHOM);
                        g_chatPlayer = -1;
                    }
                    else
                        typebuf[0] = 0;
                }

                KB_ClearKeyDown(sc_1);
                KB_ClearKeyDown(sc_2);
                KB_ClearKeyDown(sc_3);
                KB_ClearKeyDown(sc_4);
                KB_ClearKeyDown(sc_5);
                KB_ClearKeyDown(sc_6);
                KB_ClearKeyDown(sc_7);
                KB_ClearKeyDown(sc_8);
                KB_ClearKeyDown(sc_A);
                KB_ClearKeyDown(sc_Escape);
                KB_ClearKeyDown(sc_Enter);
            }
        }
    }
    else
    {
        if (ud.screen_size > 1) j = 200-45;
        else j = 200-8;
        if (xdim >= 640 && ydim >= 480)
            j = scale(j,ydim,200);
        hitstate = Net_EnterText(320>>1,j,typebuf,120,1);

        if (hitstate == 1)
        {
            KB_ClearKeyDown(sc_Enter);
            if (Bstrlen(typebuf) == 0)
            {
                g_player[myconnectindex].ps->gm &= ~(MODE_TYPE|MODE_SENDTOWHOM);
                return;
            }
            if (ud.automsg)
            {
                if (SHIFTS_IS_PRESSED) g_chatPlayer = -1;
                else g_chatPlayer = ud.multimode;
            }
            g_player[myconnectindex].ps->gm |= MODE_SENDTOWHOM;
        }
        else if (hitstate == -1)
            g_player[myconnectindex].ps->gm &= ~(MODE_TYPE|MODE_SENDTOWHOM);
        else pub = NUMPAGES;
    }
}

void Net_ClearFIFO(void)
{
    netInput = {}; // Clear all networked input.

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
    // New lockstep session: acks, drop boundaries, keepalive clocks, and the
    // health monitors all restart from zero alongside the FIFO cursors.
    Net_ResetProtocolState();
#endif

    memset(&syncData, 0, sizeof(syncData));
    Net_ResetSyncCheck();   // verdict, per-cat flags, and the compared-tic ring
    {
        extern int32_t g_worldExecs;   // epoch-relative world-step count (sync cat 20)
        g_worldExecs = 0;
    }

    bufferjitter = 1;
    mymaxlag = otherminlag = 0;
    movefifoplc = movefifosendplc = predictfifoplc = 0;
    Net_LocalSoundResetWatermark();   // [P2] plc restarted: forget emitted tics

    // Capture the live sprite array pointer BEFORE Net_InitializePrediction ->
    // Net_ResetPredictionData memcpys from it. Nothing else in the transport
    // tree calls Net_InitializeStructPointers, and an uncaptured (NULL)
    // original_sprite is restored into the global `sprite` pointer by
    // Net_UseOriginalPointers at the end of every prediction pass.
    Net_InitializeStructPointers();
    Net_InitializePrediction();

    for (int32_t i = 0; i < MAXPLAYERS; i++)
    {
        g_player[i].movefifoend = 0;
        g_player[i].myminlag = 0;
        g_player[i].lastSyncTick = -1;
    }
}

void Net_CheckPlayerQuit(int i)
{
    if (TEST_SYNC_KEY(g_player[i].input.bits, SK_GAMEQUIT) == 0)
        return;

    g_player[i].connected = 0;
    g_netBotMask &= ~(1 << i);   // a freed seat must never keep a bot bit
    Bot_ResetSeat(i, BOT_RESET_LEVEL);

    G_CloseDemoWrite();

    // If we're the master, keep executing until all slaves can get the packet.
    if (i == connecthead)
        return;

    if (i == myconnectindex)
        G_GameExit(" ");

    if (screenpeek == i)
    {
        screenpeek = G_GetNextPlayer(i);
        if (screenpeek < 0)
            screenpeek = connecthead;
    }

    if (i == connecthead)
        connecthead = connectpoint2[connecthead];
    else
    {
        int j;
        TRAVERSE_CONNECT(j)
        {
            if (connectpoint2[j] == i)
                connectpoint2[j] = connectpoint2[i];
        }
    }

    numplayers--;
    ud.multimode--;

    if (numplayers < 2)
        S_PlaySound(GENERIC_AMBIENCE17);

    pub = NUMPAGES;
    pus = NUMPAGES;
    G_UpdateScreenArea();

    P_QuickKill(g_player[i].ps);
    A_DeleteSprite(g_player[i].ps->i);

    Bsprintf(buf, "%s^00 is history!", g_player[i].user_name);
    G_AddUserQuote(buf);
    Bstrcpy(apStrings[QUOTE_RESERVED2], buf);

    if (vote.starter == i)
    {
        for (ALL_PLAYERS(i))
            g_player[i].gotvote = 0;

        vote = votedata_t();
    }

    g_player[myconnectindex].ps->ftq = QUOTE_RESERVED2, g_player[myconnectindex].ps->fta = 180;
}

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
// ── Deterministic mid-game player removal ───────────────────────────────────
// Classic Net_CheckPlayerQuit's body, callable without the in-band quit bit.
// EVERY peer must call this at the SAME tic (either from the consumed input
// stream carrying SK_GAMEQUIT, or at the master-stamped goneTic boundary), so
// the world edit below is part of the deterministic simulation.
void Net_ExcisePlayer(int i)
{
    if ((unsigned)i >= MAXPLAYERS || !g_player[i].connected || i == myconnectindex || numplayers < 2)
        return;

    initprintf("net: excising player %d at tic %d\n", i, movefifoplc);
#ifdef __EMSCRIPTEN__
    EM_ASM({ console.log('[eng] excise p=' + $0 + ' plc=' + $1 + ' gone=' + $2 + ' quitbit=' + $3); },
           i, movefifoplc, s_goneTic[i],
           TEST_SYNC_KEY(g_player[i].input.bits, SK_GAMEQUIT) ? 1 : 0);
#endif

    g_player[i].connected      = 0;
    g_player[i].playerquitflag = 0;
    g_netBotMask &= ~(1 << i);   // a freed seat must never keep a bot bit
                                 // (bot yields ride this same excise path)
    Bot_ResetSeat(i, BOT_RESET_LEVEL);

    G_CloseDemoWrite();

    if (screenpeek == i)
    {
        screenpeek = G_GetNextPlayer(i);
        if (screenpeek < 0)
            screenpeek = connecthead;
    }

    if (i == connecthead)
        connecthead = connectpoint2[connecthead];
    else
    {
        int jj;
        TRAVERSE_CONNECT(jj)
        {
            if (connectpoint2[jj] == i)
                connectpoint2[jj] = connectpoint2[i];
        }
    }

    numplayers--;
    ud.multimode--;

    if (numplayers < 2)
        S_PlaySound(GENERIC_AMBIENCE17);

    pub = NUMPAGES;
    pus = NUMPAGES;
    G_UpdateScreenArea();

    if (g_player[i].ps != NULL)
    {
        P_QuickKill(g_player[i].ps);
        if ((unsigned)g_player[i].ps->i < MAXSPRITES)
            A_DeleteSprite(g_player[i].ps->i);

        Bsprintf(buf, "%s^00 is history!", g_player[i].user_name);
        G_AddUserQuote(buf);
        Bstrcpy(apStrings[QUOTE_RESERVED2], buf);
        g_player[myconnectindex].ps->ftq = QUOTE_RESERVED2;
        g_player[myconnectindex].ps->fta = 180;
    }

    if (vote.starter == i)
    {
        for (int32_t ALL_PLAYERS(k))
            g_player[k].gotvote = 0;
        vote = votedata_t();
    }

    // The master also tears down the wire pair; the kicked peer experiences a
    // normal host-side disconnect (its own UI path handles it). Guests have no
    // link to a fellow guest (star topology), so this is master-only.
    if (myconnectindex == connecthead)
        net_kick(i);
}

// Voluntary leaves, consumed at the deterministic point: every peer reads the
// same input stream, so the first tic carrying SK_GAMEQUIT excises the quitter
// identically everywhere. Called from G_DoMoveThings right after input latch.
void Net_ConsumeQuitInputs(void)
{
    if (numplayers < 2)
        return;

    int i, drops[MAXPLAYERS], n = 0;
    TRAVERSE_CONNECT(i)
        if (i != myconnectindex && TEST_SYNC_KEY(g_player[i].input.bits, SK_GAMEQUIT))
            drops[n++] = i;

    for (int k = 0; k < n; k++)
    {
        i = drops[k];
        if (i == connecthead && myconnectindex != connecthead)
        {
            // The HOST quit from its menu: graceful match teardown (menus.cpp
            // host-gone consumer), never a client-side G_GameExit.
            Net_SetLocalBot(0);
            g_netHostGone = 1;
            continue;
        }
        if (myconnectindex == connecthead && s_goneTic[i] < 0)
        {
            // Stop REQUIRING their input immediately (they exit after this
            // tic); announcing keeps their final records decodable in repair
            // windows for any slave that still needs them.
            s_goneTic[i] = movefifosendplc;
            s_goneAnnounceUntil[i] = (int32_t)totalclock + 600;
        }
        Net_ExcisePlayer(i);
    }
}

// Involuntary drops: excise exactly at the master-stamped boundary. Called from
// G_MoveLoop before the consume gate, so the gate never waits on the departed.
void Net_ApplyPendingDrops(void)
{
    if (numplayers < 2)
        return;

    for (int i = 0; i < MAXPLAYERS; i++)
        if (s_goneTic[i] >= 0 && g_player[i].connected && s_goneTic[i] <= movefifoplc)
            Net_ExcisePlayer(i);
}

// Barrier entry (level launch, snapshot join, resync): pending drops can no
// longer excise in-band -- fold them into the roster before the cursors reset.
// The master's view is authoritative; it propagates via seat masks/snapshots.
void Net_FlushPendingDrops(void)
{
    if (myconnectindex != connecthead)
    {
        // Receivers take the roster from the mask; just forget local markers.
        for (int i = 0; i < MAXPLAYERS; i++)
            s_goneTic[i] = -1;
        s_peerDownMask = 0;
        return;
    }

    int changed = 0;
    for (int i = 0; i < MAXPLAYERS; i++)
    {
        if (i == myconnectindex)
        {
            s_goneTic[i] = -1;
            continue;
        }
        if ((s_goneTic[i] >= 0 || (s_peerDownMask & (1 << i))) && g_player[i].connected)
        {
            g_player[i].connected = 0;
            g_netBotMask &= ~(1 << i);   // a freed seat must never keep a bot bit
            Bot_ResetSeat(i, BOT_RESET_LEVEL);
            changed = 1;
            initprintf("net: flushed pending drop of player %d at barrier\n", i);

            // If we are live in a level (snapshot boundary, not a fresh
            // launch), also clean their avatar out of the world so the
            // snapshot doesn't embed a ghost statue.
            auto const ps = g_player[i].ps;
            if (g_player[myconnectindex].ps != NULL
                && (g_player[myconnectindex].ps->gm & MODE_GAME)
                && ps != NULL && (unsigned)ps->i < MAXSPRITES
                && sprite[ps->i].picnum == APLAYER && sprite[ps->i].yvel == i)
            {
                P_QuickKill(ps);
                A_DeleteSprite(ps->i);
            }
        }
        s_goneTic[i] = -1;
    }
    s_peerDownMask = 0;

    if (changed)
        Net_RebuildConnectChain();
}
#endif  // transport track

// Entry-barrier who-is-in bitmask, relayed by the host at ~1Hz
// (PACKET_TYPE_READY_ROSTER). Guests only ever hear the HOST's readiness
// (slaves report to the master alone), so without this relay a guest's wait
// screen cannot name which OTHER seat is still loading. Display only.
uint32_t g_netBarrierReadyMask;

void Net_WaitForPlayers()
{
    int i;

    if (numplayers < 2)
        return;

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
    // Barrier-free joiner: G_LoadPlayer's load path reaches this barrier via
    // Net_WaitForServer, but there IS no rendezvous -- the match is running and
    // the catchup stream carries us to it. (A ClearFIFO here would also wipe
    // the absolute-timeline cursors the apply just primed.)
    if (g_netJoinCatchup)
        return;
#endif

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
    // A drop pending at a barrier crossing can no longer excise in-band (the
    // FIFO cursors reset below). Fold it into the roster NOW, before the reset
    // and before the readiness loop would wait on a dead peer forever.
    Net_FlushPendingDrops();
    if (numplayers < 2)
        return;   // the flush may have shrunk the session to just us
#endif

    // Tic-0 lockstep state reset. The classic connect path ran Net_ClearFIFO when
    // the netgame formed; on the transport track NOTHING called it, so
    // Net_InitializePrediction never ran: originalPlayer stayed NULL and the first
    // G_MoveLoop prediction pass (game.cpp:7450) restored g_player[..].ps = NULL
    // and sprite = NULL on every GUEST (the host never predicts), crashing within
    // frames of entry. Every peer passes through this barrier at level entry with
    // numplayers already correct, so init here is symmetric on host and guests.
    Net_ClearFIFO();

    g_player[myconnectindex].playerreadyflag++;
    // CPU seats are always ready: mirror the master's flag so the barrier's
    // wait loop and its status display treat them as arrived. (Slaves only
    // wait for the master, so guests need no bot awareness here at all.)
    if (myconnectindex == connecthead)
        for (int bk = 0; bk < MAXPLAYERS; bk++)
            if (g_netBotMask & (1 << bk))
                g_player[bk].playerreadyflag = g_player[myconnectindex].playerreadyflag;
    packbuf[0] = PACKET_TYPE_PLAYER_READY;
    packbuf[1] = (char)numplayers;   // entry audit: the world size we entered with
    if (myconnectindex != connecthead)
        oldnet_sendpacket(connecthead, (unsigned char*)packbuf, 2);

    auto oldPal = g_player[myconnectindex].ps->palette;
    P_SetGamePalette(g_player[myconnectindex].ps, TITLEPAL, 11);

    // Lobby-style wait ("there really should be a wait while the host loads",
    // 2026-08-16): the barrier itself has always held the match until every
    // seat is in -- what was missing is TELLING the player that. Elapsed clock,
    // a per-seat Ready/Loading roster on EVERY machine (not just the host),
    // and a 5s [barrier] log so headless runs can see the wait too.
    g_netBarrierReadyMask = (1u << myconnectindex);
    uint32_t const waitStart = timerGetTicks();
    uint32_t nextRosterSend = 0, nextWaitLog = 0;

    while (1)
    {
        //if (quitevent) G_GameExit(""); // This sucks
        gameHandleEvents();

#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
        // WASM (seam) OR native WebRTC transport: drain the seam each frame so peers'
        // PACKET_TYPE_PLAYER_READY is processed (net_poll -> Net_ReceiveFrame runs the
        // packet switch inline). Without this the tic-0 barrier never observes the others
        // as ready and spins forever; videoNextPage below yields so new frames arrive
        // between iterations. Mirrors the net_poll drain in Net_GetPackets. Guarded so
        // stock native NetDuke32's barrier loop (no seam transport) is byte-for-byte unchanged.
        net_poll();
#endif

        if (!engineFPSLimit())
            continue;

        videoClearScreen(0);

        rotatesprite(0, 0, 65536L, 0, BETASCREEN, 0, 0, 2 + 8 + 16 + 64, 0, 0, xdim - 1, ydim - 1);
        rotatesprite(160 << 16, (104) << 16, 60 << 10, 0, DUKENUKEM, 0, 0, 2 + 8, 0, 0, xdim - 1, ydim - 1);
        rotatesprite(160 << 16, (129) << 16, 30 << 11, 0, THREEDEE, 0, 0, 2 + 8, 0, 0, xdim - 1, ydim - 1);
        if (PLUTOPAK)   // JBF 20030804
            rotatesprite(160 << 16, (151) << 16, 30 << 11, 0, PLUTOPAKSPRITE + 1, 0, 0, 2 + 8, 0, 0, xdim - 1, ydim - 1);

        uint32_t const nowMs    = timerGetTicks();
        int const      waitSecs = (int)((nowMs - waitStart) / 1000);

        if (myconnectindex == connecthead && nowMs >= nextRosterSend)
        {
            // ~1Hz who-is-in relay so every guest's roster below is live.
            nextRosterSend = nowMs + 1000;
            uint32_t mask = 0;
            TRAVERSE_CONNECT(i)
                if (g_player[i].playerreadyflag >= g_player[myconnectindex].playerreadyflag)
                    mask |= (1u << i);
            g_netBarrierReadyMask = mask;
            packbuf[0] = PACKET_TYPE_READY_ROSTER;
            B_BUF32(&packbuf[1], mask);
            TRAVERSE_CONNECT(i)
                if (i != myconnectindex && !(g_netBotMask & (1 << i)))
                    oldnet_sendpacket(i, (unsigned char *)packbuf, 5);
        }

        {
            char waitLine[48];
            Bsprintf(waitLine, "WAITING FOR PLAYERS  %d:%02d", waitSecs / 60, waitSecs % 60);
            gametext(160, 183, waitLine, 14, 2);
            minitext(70, 193, "The match starts when every seat shows Ready", 12, 2 + 8 + 16);
        }

        {
            int ypos = 8;
            gametext(8, ypos, "^12Player Status:", -127, 2);
            bool const flagsKnown = (myconnectindex == connecthead);
            TRAVERSE_CONNECT(i)
            {
                ypos += 8;
                gametext(8, ypos, g_player[i].user_name, -127, 2);
                int ready;
                if (i == myconnectindex)
                    ready = 1;
                else if (flagsKnown || i == connecthead)
                    ready = (g_player[i].playerreadyflag >= g_player[myconnectindex].playerreadyflag);
                else
                    // Other guests' READY never reaches us directly -- the
                    // host's READY_ROSTER relay is the only source. Until it
                    // arrives, "Loading" is the honest default.
                    ready = (g_netBarrierReadyMask >> i) & 1;
                gametext(107, ypos, ready ? "^7- ^8Ready!" : "^7- ^10Loading", -127, 2);
            }
        }

        if (nowMs >= nextWaitLog)
        {
            nextWaitLog = nowMs + 5000;
            uint32_t pend = 0;
            TRAVERSE_CONNECT(i)
                if (i != myconnectindex && g_player[i].playerreadyflag < g_player[myconnectindex].playerreadyflag)
                    pend |= (1u << i);
            LOG_F(INFO, "[barrier] waiting %ds for seats mask=0x%x", waitSecs, pend);
        }

        videoNextPage();

        // Check if ready, if not, break from loop.
        TRAVERSE_CONNECT(i)
        {
            if (g_player[i].playerreadyflag < g_player[myconnectindex].playerreadyflag)
                break;

            //slaves in M/S mode only wait for master
            if (myconnectindex != connecthead)
            {
                i = -1; // we're a slave
                break;
            }
        }

        // -1 Means we iterated through all players without breaking in above loop. All players ready.
        if (i <= -1)
        {
            // master signals release once it hears from all slaves. STREAM
            // MODE: an EXPLICIT, value-carrying LEVEL_GO -- the ONLY packet a
            // stream guest's barrier accepts, and it cannot exist before this
            // point (loaded + every seat reported). Classic mode keeps the
            // legacy READY broadcast.
            if (myconnectindex == connecthead)
            {
                TRAVERSE_CONNECT(i)
                {
                    if (i == myconnectindex || (g_netBotMask & (1 << i)))
                        continue;
                    if (g_netStreamMode)
                    {
                        packbuf[0] = PACKET_TYPE_LEVEL_GO;
                        packbuf[1] = g_player[myconnectindex].playerreadyflag;
                        oldnet_sendpacket(i, (unsigned char *)packbuf, 2);
                    }
                    else
                    {
                        packbuf[0] = PACKET_TYPE_PLAYER_READY;
                        oldnet_sendpacket(i, (unsigned char *)packbuf, 1);
                    }
                }
            }

            LOG_F(INFO, "[barrier] all seats ready after %ds", (int)((timerGetTicks() - waitStart) / 1000));
            P_SetGamePalette(g_player[myconnectindex].ps, oldPal, 11);
            return;
        }
    }
}

void allowtimetocorrecterrorswhenquitting(void)
{
    int i, j, oldtotalclock;

    ready2send = 0;

    for (j=MAXPLAYERS-1;j>=0;j--)
    {
        oldtotalclock = (int32_t)totalclock;

        while (totalclock < oldtotalclock+TICSPERFRAME)
            gameHandleEvents();

        if (KB_KeyPressed(sc_Escape))
            return;

        packbuf[0] = PACKET_TYPE_NULL_PACKET;
        TRAVERSE_CONNECT(i)
        {
            if (i != myconnectindex)
                oldnet_sendpacket(i, (unsigned char*)packbuf,1);
            if (myconnectindex != connecthead)
                break; //slaves in M/S mode only send to master
        }
    }
}

void Net_Disconnect(bool showScores)
{
    Net_SetLocalBot(0);
    allowtimetocorrecterrorswhenquitting();
    net_transport_shutdown();

    g_networkBroadcastMode = NETMODE_OFFLINE;

    if (!quickExit && showScores)
    {
        if (playerswhenstarted > 1 && g_player[myconnectindex].ps->gm & MODE_GAME && GTFLAGS(GAMETYPE_SCORESHEET))
        {
            G_BonusScreen(1);
            // [NetDuke32 port] this tree keeps screen mode in ud.setup.* (config->setup
            // rename); matches G_BonusScreen's restore call elsewhere in game.cpp.
            videoSetGameMode(ud.setup.fullscreen, ud.setup.xdim, ud.setup.ydim, ud.setup.bpp, ud.detail);
        }
    }

    numplayers = ud.multimode = playerswhenstarted = 1;
    myconnectindex = screenpeek = 0;
    oldnet_gotinitialsettings = 0;

    G_BackToMenu();
}

void Net_InitiateVote()
{
    for (int32_t ALL_PLAYERS(i))
        g_player[i].gotvote = 0;

    g_player[myconnectindex].gotvote = vote.yes_votes = 1;
    
    packbuf[0] = PACKET_TYPE_MAP_VOTE_INITIATE;
    int32_t j = 1;
    packbuf[j++] = vote.starter           = myconnectindex;
    packbuf[j++] = vote.episode           = ud.m_volume_number;
    packbuf[j++] = vote.level             = ud.m_level_number;
    packbuf[j++] = vote.skill             = ud.m_player_skill;
    packbuf[j++] = vote.gametype          = ud.m_coop;
    B_BUF32(&packbuf[j], ud.m_dmflags); j += sizeof(int32_t);

    int32_t c = 0;
    TRAVERSE_CONNECT(c)
    {
        if (c != myconnectindex) oldnet_sendpacket(c, (unsigned char*)packbuf, j);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }
}

void Net_CancelVote()
{
    OSD_Printf("Canceling vote.\n");

    vote = votedata_t();
    for (int32_t ALL_PLAYERS(i))
    {
        g_player[i].gotvote = 0;
    }

    packbuf[0] = PACKET_TYPE_MAP_VOTE_CANCEL;
    packbuf[1] = myconnectindex;

    int32_t c = 0;
    TRAVERSE_CONNECT(c)
    {
        if (c != myconnectindex) oldnet_sendpacket(c, (unsigned char*)packbuf, 2);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }
}