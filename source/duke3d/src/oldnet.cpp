#define OLDNET_CPP_

#include "duke3d.h"
#include "oldnet.h"

// Forward decl: file-static, defined mid-file; the seat-mask receive path in the
// packet dispatch runs before it textually.
static void Net_RebuildConnectChain(void);
#include "net_predict.h"
#include "net_transport.h"
#include "chatpipe.h"
#include "demo.h"  // G_CloseDemoWrite (Net_CheckPlayerQuit)
#include "savegame.h"  // late-join snapshot: sv_saveandmakesnapshot / G_LoadPlayer
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
// Transport seam. The netcode never talks to enet/UDP/sockets directly; every
// outgoing packet is classified onto a logical channel + reliability here and
// handed to the pluggable transport (net_transport.h). Inbound frames arrive
// via Net_ReceiveFrame() below. peerToken == connectindex.
// ---------------------------------------------------------------------------
static void oldnet_sendpacket(int other, unsigned char *bufptr, int len)
{
    int channel, reliable;

    switch (bufptr[0])
    {
        case PACKET_TYPE_MASTER_TO_SLAVE:
        case PACKET_TYPE_SLAVE_TO_MASTER:
        case PACKET_TYPE_PING:
            channel  = NET_CHAN_MOVE;   // per-tic input: unreliable, unordered
            reliable = 0;
            break;

        case PACKET_TYPE_USER_MAP:
        case PACKET_TYPE_LOAD_GAME:
            channel  = NET_CHAN_BULK;   // potentially large: isolated bulk channel
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
int32_t g_netJoinCatchup;            // joiner: applied the snapshot, streaming to live

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

static struct JoinTicInit { JoinTicInit() { for (auto &t : s_joinTic) t = -1; } } s_joinTicInit;

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
    s_joinAwaitReal  = 0;
    s_fillActive     = 0;
    s_joinFlowSlot   = -1;
    s_joinFlowClock  = 0;
    s_joinFlowTries  = 0;
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
    bool const isSlave = (numplayers > 1 && myconnectindex != connecthead);
    bool const seated  = g_player[myconnectindex].connected && !g_netJoinCatchup;
    int32_t const localHead = isSlave ? g_netSampleHead : g_player[myconnectindex].movefifoend;
    bool const capped = (localHead - movefifoplc >= 100);

    if (!capped && seated)
    {
        if (isSlave)
        {
            g_netSendRing[g_netSampleHead & (MOVEFIFOSIZ - 1)] = netInput;
            g_netSampleHead++;
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
        // the host latches g_foundSyncError and pushes the healing snapshot.
        if (g_foundSyncError || Net_SyncErrorDetected())
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
            if (g_player[myconnectindex].ps != NULL
                && (g_player[myconnectindex].ps->gm & MODE_GAME)
                && now - lastpackettime > NET_HOST_SILENT)
                g_netHostGone = 1;
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
                    if (d != 0) { g_netEpochDrops++; break; }   // wrong generation either way
                }
                if ((unsigned)other >= MAXPLAYERS)
                    break;
                j = 2;
                int32_t const start = (int32_t)B_UNBUF32(&packbuf[j]); j += 4;
                int32_t const count = (uint8_t)packbuf[j++];
                {
                    int32_t const ack = (int32_t)B_UNBUF32(&packbuf[j]); j += 4;
                    if (ack > s_slaveAck[other])   // monotonic: reorder-safe
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
                ud.m_monsters_off       = 1;   // match the host's deathmatch setup
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
                // Latch the verdict; the auto-resync consumer (menus.cpp,
                // host-gated, cooldown) pushes the healing snapshot.
                if (myconnectindex == connecthead)
                {
                    g_foundSyncError = true;
#ifdef __EMSCRIPTEN__
                    EM_ASM({ console.log('[eng] desync report from peer ' + $0 + ' -> resync'); }, other);
#endif
                }
                break;
            }
            case PACKET_TYPE_PLAYER_READY:
            {
                if (g_player[other].playerreadyflag == 0)
                    LOG_F(INFO, "Player %d is ready", other);
                g_player[other].playerreadyflag++;
#if defined(__EMSCRIPTEN__) || defined(NETNATIVE)
                // MASTER ECHO (late-join rendezvous): the master's one-shot READY
                // broadcast can land while a late joiner is still LOADING the
                // snapshot -- the load then restores the saved (zero) readyflags
                // over it and the joiner waits forever for a packet that already
                // came. Echoing READY back at the sender is idempotent (flags are
                // >= comparisons) and makes the rendezvous order-independent.
                if (myconnectindex == connecthead && other != myconnectindex)
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
                g_player[myconnectindex].ps->gm = MODE_EOL;
                ud.level_number = packbuf[1];
                ud.from_bonus = packbuf[2];
                ud.secretlevel = packbuf[3];
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
                            g_netHostGone = 1;
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

// DEBUG bisection switch (desync validation): bit0 = Net_CorrectPrediction
// runs, bit1 = predicted-view render swap active. Defaults to both ON.
int32_t g_netPredictMode = 3;
// Forensic console dumps (MISMATCH/INPDUMP/SPAWNDUMP/RNGDUMP/STATDUMP) --
// default OFF: comparisons and auto-resync run regardless; the dump bursts
// correlated with renderer deaths on the bench. Soak enables for hunts.
int32_t g_netForensics = 0;
#ifdef __EMSCRIPTEN__
extern "C" void Web_SetPredictMode(int mode)
{
    g_netPredictMode = mode;
    EM_ASM({ console.log('[eng] predictMode=' + $0); }, mode);
}
extern "C" void Web_SetForensics(int on)
{
    g_netForensics = on;
    EM_ASM({ console.log('[eng] forensics=' + $0); }, on);
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
    // Multi-target on purpose: the ccall can land while the engine is
    // suspended inside a predicted-view swap (ps would be the predicted COPY,
    // wiped at the next reconcile) and the player sprite is rewritten from ps
    // every tic -- but walls and sectors have no predicted copies and nothing
    // rewrites them, so Sync_Map flags them for as long as the fork lives.
    ps->pos.x += 256;
    if ((unsigned)ps->i < MAXSPRITES)
        sprite[ps->i].x += 256;
    if (numwalls > 0)
        wall[0].x += 16;
    if (numsectors > 0)
        sector[0].floorz += 256;
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

// Materialize a late joiner in the running level. Mirrors what level entry does
// for players 1+ (G_ResetAllPlayers: players are memcpys of a reference ps with
// identity/position fixups) plus the sprite insert. Under the barrier-free join
// EVERY peer runs this at the same consumed tic (Net_ApplyPendingJoins), so it
// must be bit-deterministic: the template is CONNECTHEAD's ps -- identical
// lockstep state on every peer -- never myconnectindex's (which differs).
void Net_InsertLatePlayer(int k)
{
    if ((unsigned)k >= MAXPLAYERS || g_playerSpawnCnt <= 0)
        return;

    G_MaybeAllocPlayer(k);

    auto &plr = g_player[k];
    Bmemcpy(plr.ps, g_player[connecthead].ps, sizeof(DukePlayer_t));
    Bmemset(plr.frags, 0, sizeof(plr.frags));

    auto &spawn = g_playerSpawnPoints[k % g_playerSpawnCnt];
    int const i = A_InsertSprite(spawn.sect, spawn.x, spawn.y, spawn.z,
                                 APLAYER, 0, 0, 0, spawn.ang, 0, 0, 0, 10);
    sprite[i].yvel = k; // classic contract: a player sprite's yvel is its player index

    auto &p = *plr.ps;
    p.i          = i;
    p.opos = p.pos = spawn.xyz;
    p.bobpos     = p.pos.xy;
    p.cursectnum = spawn.sect;
    p.oq16ang = p.q16ang = fix16_from_int(spawn.ang);
    p.dead_flag  = 0;
    p.newowner   = -1;
    p.frag = p.fraggedself = 0;
    p.gm         = MODE_GAME;
    sprite[i].cstat = CSTAT_SPRITE_BLOCK + CSTAT_SPRITE_BLOCK_HITSCAN;

    P_ResetWeapons(k);
    P_ResetInventory(k);

    initprintf("net: inserted late player %d at spawn %d\n", k, k % g_playerSpawnCnt);
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

    // The snapshot carries the HOST's view of every per-player struct; OUR identity
    // is local state and must survive the load.
    myconnectindex = myIdx;
    screenpeek     = joinMode ? connecthead : myPeek;   // joiner: spectate while syncing
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
        // Our slot is NOT in the world yet; input/HUD paths still dereference our
        // ps. Shadow the host's player until the deterministic seat replaces it.
        G_MaybeAllocPlayer(myIdx);
        if (g_player[connecthead].ps != NULL)
            Bmemcpy(g_player[myIdx].ps, g_player[connecthead].ps, sizeof(DukePlayer_t));
        g_player[myIdx].ps->gm = MODE_GAME;
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
            // I am the joiner: leave spectator mode and start staging real
            // inputs from this very tic.
            g_netJoinCatchup = 0;
            screenpeek       = myconnectindex;
            g_netSampleHead  = movefifoplc;
            s_ackOfMyInput   = movefifoplc;
            // The pre-seat sampler never ran, so the lag/jitter bookkeeping
            // accumulated garbage (localNow was -1): reset it or the first
            // timer-nudge block would jerk totalclock by a bogus offset.
            for (int p2 = 0; p2 < MAXPLAYERS; p2++)
                g_player[p2].myminlag = 0x7fffffff;
            mymaxlag = otherminlag = 0;
            Net_InitializePrediction();
        }
#ifdef __EMSCRIPTEN__
        EM_ASM({ console.log('[eng] joinApplied p=' + $0 + ' tic=' + $1 + ' np=' + $2 + ' me=' + $3); },
               k, movefifoplc, numplayers, (int)(k == myconnectindex));
#else
        initprintf("net: player %d seated at tic %d (%d players)\n", k, movefifoplc, numplayers);
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
        if (g_player[k].connected)   // insert applied on our own sim: flow done
        {
            s_joinFlowSlot = -1;
            return;
        }
        if (s_joinTic[k] >= 0)       // stamped; every sim seats when it crosses the tic
            return;

        int32_t const gap = movefifosendplc - s_slaveAck[k];
        if (s_slaveAck[k] > s_joinFlowBase && gap <= 8)
        {
            // Caught up FOR REAL (acks advanced past the snapshot base AND
            // near-live): seat at the aggregation head plus a margin every peer
            // will cross AFTER the announcement (it rides every M2S packet).
            int32_t const tic = movefifosendplc + NET_JOIN_MARGIN;
            Net_ScheduleJoin(k, tic);
            s_joinAnnounceUntil[k] = now + NET_JOIN_ANNOUNCE;
            s_joinAwaitReal |= (1 << k);
            initprintf("net: joiner %d caught up (gap %d) -> seat at tic %d\n", k, gap, tic);
            return;
        }
        if (now - s_joinFlowClock > NET_JOIN_RETRY && gap > NET_JOIN_RING_MAX)
        {
            // Too slow for the 256-tic ring (cold art/texture caches). Each
            // retry re-bases the stream on a FRESH snapshot; the joiner gets
            // warmer every pass. Persistent failure -> kick.
            if (++s_joinFlowTries >= NET_JOIN_TRIES)
            {
                initprintf("net: joiner %d cannot catch up -> kick\n", k);
                net_kick(k);
                s_joinFlowSlot = -1;
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
                netmenu_send_snapshot_to(seatMask, k, movefifoplc, 1);
                initprintf("net: join retry %d for slot %d (snapshot at tic %d)\n",
                           s_joinFlowTries, k, movefifoplc);
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
        g_netHostGone = 1; // guest lost its host -> menus.cpp consumer exits to the main menu

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

    int i;
    TRAVERSE_CONNECT(i)
    {
        if (i != myconnectindex) oldnet_sendpacket(i, (unsigned char*)packbuf,j);
        if (myconnectindex != connecthead) break; //slaves in M/S mode only send to master
    }
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
    packbuf[0] = PACKET_TYPE_PLAYER_READY;
    if (myconnectindex != connecthead)
        oldnet_sendpacket(connecthead, (unsigned char*)packbuf, 1);

    auto oldPal = g_player[myconnectindex].ps->palette;
    P_SetGamePalette(g_player[myconnectindex].ps, TITLEPAL, 11);

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

        gametext(160, 190, "WAITING FOR PLAYERS", 14, 2);

        if (myconnectindex == connecthead)
        {
            int ypos = 8;
            gametext(8, ypos, "^12Player Status:", -127, 2);
            TRAVERSE_CONNECT(i)
            {
                ypos += 8;
                gametext(8, ypos, g_player[i].user_name, -127, 2);

                if (g_player[i].playerreadyflag >= g_player[myconnectindex].playerreadyflag)
                    gametext(107, ypos, "^7- ^8Ready!", -127, 2);
                else
                    gametext(107, ypos, "^7- ^10Loading", -127, 2);
            }
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
            // master sends ready packet once it hears from all slaves
            if (myconnectindex == connecthead)
            {
                TRAVERSE_CONNECT(i)
                {
                    packbuf[0] = PACKET_TYPE_PLAYER_READY;

                    if (i != myconnectindex)
                        oldnet_sendpacket(i, (unsigned char*)packbuf, 1);
                }
            }

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