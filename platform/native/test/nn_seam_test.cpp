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
static std::atomic<bool> g_gotFrame{ false };
static std::string g_frameData;

extern "C" void Net_ReceiveFrame(int peer, int channel, const uint8_t *data, int len)
{
    g_frameData.assign((const char *)data, len);
    g_gotFrame = true;
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

int main()
{
    net_transport_init(); // reads NN_ROLE/NN_KEY/NN_RELAY/NN_HOSTID, starts host or guest

    bool sent = false;
    for (int t = 0; t < 25000; t += 50)
    {
        net_poll(); // game thread: applies peer events + frames
        if (g_peerUps.load() > 0 && !sent)
        {
            const char *msg = "HELLO-FROM-SEAM";
            net_broadcast(NET_CHAN_REL, 1, msg, (int)strlen(msg)); // reliable control channel
            sent = true;
        }
        if (g_gotFrame.load())
            break;
        std::this_thread::sleep_for(50ms);
    }
    net_poll(); // final drain

    bool ok = g_peerUps.load() > 0 && g_gotFrame.load();
    if (ok)
        printf("SEAM OK\n");
    else
        printf("SEAM FAIL (peerUps=%d gotFrame=%d)\n", g_peerUps.load(), (int)g_gotFrame.load());
    fflush(stdout);

    net_transport_shutdown();
    return ok ? 0 : 1;
}
