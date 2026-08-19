// Stage-3 seam test: links the REAL net_transport_native.cpp with stub netcode
// inbound functions (Net_ReceiveFrame / Net_PeerEvent / Net_SetLocalIndex) and a
// game-thread loop that pumps net_poll. Two of these (host + guest, via NN_ROLE)
// connect over the relay, run the join handshake, and exchange a frame through
// the seam. Proves the full transport end-to-end without the engine.
#include <cstdint>
#include <cstdlib>
#define SJSON_IMPLEMENT
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define ABORT_IF_F(cond, ...) \
    do { if (cond) abort(); } while (0)
#define sjson_malloc(user, size) (malloc(size))
#define sjson_free(user, ptr) (free(ptr))
#define sjson_realloc(user, ptr, size) (realloc(ptr, size))
#include <sjson.h>
#undef SJSON_IMPLEMENT

#include "net_transport.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace std::chrono_literals;

// ── stub netcode inbound (normally defined in oldnet.cpp) ────────────────────
static std::atomic<int> g_peerUps{ 0 };
static std::atomic<int> g_localIdx{ -1 };
static std::atomic<int> g_frames{ 0 };
static std::atomic<int> g_lastFramePeer{ -1 };
static std::string g_frameData;
static int g_botMask = 0;

// Engine-only helpers referenced by the native transport. The standalone seam
// fixture has no GRP or snapshot to expose; g_botMask models CPU reservations.
extern "C" const char *Net_NativeGrpPath(void) { return ""; }
extern "C" void Net_SnapshotReady(int, int, int) {}
extern "C" int Net_GetBotMask(void) { return g_botMask; }

extern "C" void Net_ReceiveFrame(int peer, int channel, const uint8_t *data, int len)
{
    g_frameData.assign((const char *)data, len);
    g_lastFramePeer = peer;
    g_frames++;
    printf("FRAME peer=%d ch=%d len=%d data=%s\n", peer, channel, len, g_frameData.c_str());
    fflush(stdout);
}
extern "C" void Net_PeerEvent(int peer, int eventType)
{
    if (eventType == NET_PEER_UP)
        g_peerUps++;
    printf("PEEREVENT peer=%d up=%d\n", peer, eventType);
    fflush(stdout);
}
extern "C" void Net_SetLocalIndex(int slot)
{
    g_localIdx = slot;
    printf("LOCALIDX %d\n", slot);
    fflush(stdout);
}

// menus.cpp provides these in the engine; stub them for the standalone seam test.
extern "C" void NetMenu_SetStatus(const char *) {}
extern "C" void NetMenu_SetRoster(const char *) {}
extern "C" void NetMenu_OnJoined(int) {}
extern "C" const char *Net_NativeGrpPath(void) { return ""; }
extern "C" uint32_t Bcrc32(const void *, int, uint32_t crc) { return crc; }
extern "C" void Net_SnapshotReady(int, int, int) {}

int main()
{
    // H02-H04 production allocator characterization: fixed 16-seat roster,
    // engine CPU reservations, clean exhaustion, and capacity clamps.
    if (net_native_clamp_capacity(-4) != 2 || net_native_clamp_capacity(99) != 16
        || !net_native_valid_guest_slot(1) || !net_native_valid_guest_slot(15)
        || net_native_valid_guest_slot(0) || net_native_valid_guest_slot(16)
        || net_native_allocate_guest_slot((1u << 1) | (1u << 2) | (1u << 4),
                                          (1u << 3) | (1u << 5)) != 6
        || net_native_allocate_guest_slot(0xfffeu, 0) != -1
        || net_native_allocate_guest_slot(0, 0xfffeu) != -1)
    {
        printf("ALLOCATOR FAIL\n");
        return 1;
    }
    printf("ALLOCATOR OK\n");

    if (const char *mask = getenv("NN_TEST_BOTMASK"))
        g_botMask = (int)strtol(mask, nullptr, 0);
    net_transport_init(); // reads NN_ROLE/NN_KEY/NN_RELAY/NN_HOSTID, starts host or guest

    bool sent = false, kicked = false, resent = false;
    for (int t = 0; t < 25000; t += 50)
    {
        net_poll(); // game thread: applies peer events + frames
        if (g_peerUps.load() > 0 && !sent)
        {
            const char *msg = "HELLO-FROM-SEAM";
            net_broadcast(NET_CHAN_REL, 1, msg, (int)strlen(msg)); // reliable control channel
            sent = true;
        }
        if (getenv("NN_TEST_KICK_REJOIN") && getenv("NN_ROLE")
            && !strcmp(getenv("NN_ROLE"), "host") && g_frames.load() > 0 && !kicked)
        {
            net_kick(g_lastFramePeer.load());
            kicked = true;
        }
        if (kicked && g_peerUps.load() > 1 && !resent)
        {
            const char *msg = "HELLO-AFTER-REJOIN";
            net_broadcast(NET_CHAN_REL, 1, msg, (int)strlen(msg));
            resent = true;
        }
        if ((!getenv("NN_TEST_KICK_REJOIN") && g_frames.load() > 0)
            || (getenv("NN_TEST_KICK_REJOIN") && resent && g_frames.load() > 1))
            break;
        std::this_thread::sleep_for(50ms);
    }
    net_poll(); // final drain

    bool const expectRejoin = getenv("NN_TEST_KICK_REJOIN") != nullptr;
    bool ok = g_peerUps.load() > (expectRejoin ? 1 : 0) && g_frames.load() > (expectRejoin ? 1 : 0);
    if (ok)
        printf(expectRejoin ? "KICK-REJOIN OK\n" : "SEAM OK\n");
    else
        printf("SEAM FAIL (peerUps=%d frames=%d)\n", g_peerUps.load(), g_frames.load());
    fflush(stdout);

    net_transport_shutdown();
    return ok ? 0 : 1;
}
