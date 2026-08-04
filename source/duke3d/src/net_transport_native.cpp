//-------------------------------------------------------------------------
//
// Native (non-Emscripten) transport backend for the NetDuke32 seam.
//
// Implements net_transport.h over libdatachannel (WebRTC data channels) plus
// a C++ Nostr signaling client (secp256k1 + AES-256-GCM), wire-compatible with
// the browser transport in platform/emscripten/net/*.ts so a native peer and a
// browser peer interoperate. A desktop host additionally maps a port with
// miniupnpc (browsers cannot) to improve reachability past NAT.
//
// This REPLACES net_transport_stub.cpp; it is compiled ONLY when NETNATIVE=1
// (see GNUmakefile duke3d_excl). The default native build keeps the no-op stub,
// so a machine without libdatachannel still builds green.
//
// Staged build-out:
//   Stage 0 (this file, initial): the seam + lifecycle + the thread-safe inbound
//           queue + per-peer channel map. Compiles and links against
//           libdatachannel; no peers are established yet.
//   Stage 1: Nostr signaling client (nostr.cpp) — secp256k1 schnorr + AES-GCM.
//   Stage 2: WebRTC offer/answer/ICE over Stage 1 (mirrors peer.ts).
//   Stage 3: join handshake + slot assignment (mirrors duke-net.ts) -> the
//            device-id <-> connectindex map that keys g_peers.
//   Stage 4: miniupnpc port mapping + ICE host candidate injection.
//-------------------------------------------------------------------------

#include "net_transport.h"

#ifdef NETNATIVE

#include <rtc/rtc.hpp>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

// ── Inbound queue: libdatachannel threads (producer) -> game thread (consumer) ─
//
// The seam contract (net_transport.h / seam_library.js): net_poll(), called on
// the game thread at Net_GetPackets, must deliver every queued inbound frame and
// peer up/down event NOW by invoking Net_ReceiveFrame / Net_PeerEvent. But
// libdatachannel fires its onMessage / onStateChange callbacks on ITS OWN worker
// threads. So those callbacks only enqueue here under a mutex; the game thread
// drains in net_poll(). Order is preserved (a peer's UP event is enqueued before
// any of its frames), matching the JS drain loop.
struct InboundItem
{
    enum Kind
    {
        Frame,
        PeerEvent
    } kind;
    int peer;                    // connectindex (== peerToken)
    int channel;                 // net_channel_t   (Frame only)
    int event;                   // net_peerevent_t (PeerEvent only)
    std::vector<uint8_t> data;   // frame bytes     (Frame only)
};

std::mutex g_inboundMtx;
std::deque<InboundItem> g_inbound;

void enqueueFrame(int peer, int channel, const std::byte *d, size_t len)
{
    InboundItem it;
    it.kind    = InboundItem::Frame;
    it.peer    = peer;
    it.channel = channel;
    it.event   = 0;
    it.data.assign(reinterpret_cast<const uint8_t *>(d), reinterpret_cast<const uint8_t *>(d) + len);
    std::lock_guard<std::mutex> lk(g_inboundMtx);
    g_inbound.push_back(std::move(it));
}

void enqueuePeerEvent(int peer, int event)
{
    InboundItem it;
    it.kind    = InboundItem::PeerEvent;
    it.peer    = peer;
    it.channel = 0;
    it.event   = event;
    std::lock_guard<std::mutex> lk(g_inboundMtx);
    g_inbound.push_back(std::move(it));
}

// ── Per-peer connection: one PeerConnection + three data channels, keyed by the
//    Duke connectindex (peerToken). The device-id <-> connectindex map is owned
//    by the join handshake (Stage 3); until then g_peers is empty and the seam
//    send paths are no-ops, exactly like the stub. ──────────────────────────────
struct PeerConn
{
    int connectindex = -1;
    std::string deviceId;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::DataChannel> dc[NET_CHAN_MAX];   // move / rel / bulk
    bool attached = false;   // false = transport lobby protocol; true = raw netcode frames
};

std::mutex g_peersMtx;
std::map<int, std::shared_ptr<PeerConn>> g_peers;   // connectindex -> conn
bool g_inited = false;

// Suppress "defined but not used" while the Stage 1-3 producers are not wired yet.
// (enqueueFrame / enqueuePeerEvent are the callbacks the signaling + data-channel
//  layer will install; referenced here so the skeleton is warning-clean.)
[[maybe_unused]] auto *const g_enqueueFrameRef     = &enqueueFrame;
[[maybe_unused]] auto *const g_enqueuePeerEventRef = &enqueuePeerEvent;

}   // namespace

extern "C" {

void net_transport_init(void)
{
    if (g_inited)
        return;
    // Route libdatachannel's plog output at Warning so a data-channel problem is
    // visible without spamming the console. Quieter than the TS console.log trail.
    rtc::InitLogger(rtc::LogLevel::Warning);
    g_inited = true;
}

void net_transport_shutdown(void)
{
    {
        std::lock_guard<std::mutex> lk(g_peersMtx);
        for (auto &kv : g_peers)
        {
            if (kv.second && kv.second->pc)
            {
                try
                {
                    kv.second->pc->close();
                }
                catch (...)
                {
                    // already closing / torn down
                }
            }
        }
        g_peers.clear();
    }

    if (g_inited)
    {
        // rtc::Cleanup() is ASYNC (returns std::shared_future<void>); it joins the
        // global libdatachannel thread pool. It MUST be waited on -- returning while
        // those threads are still joinable aborts in static destruction (verified).
        rtc::Cleanup().wait();
        g_inited = false;
    }

    std::lock_guard<std::mutex> lk(g_inboundMtx);
    g_inbound.clear();
}

void net_send(int peerToken, int channel, int reliable, const void *data, int len)
{
    (void)reliable;   // reliability is fixed per channel by DC_INIT (netconfig.ts):
                      // duke-move unreliable, duke-rel/duke-bulk reliable+ordered.
    if (channel < 0 || channel >= NET_CHAN_MAX || len <= 0 || data == nullptr)
        return;

    std::shared_ptr<rtc::DataChannel> dc;
    {
        std::lock_guard<std::mutex> lk(g_peersMtx);
        auto it = g_peers.find(peerToken);
        if (it == g_peers.end() || !it->second)
            return;
        dc = it->second->dc[channel];
    }
    if (!dc || !dc->isOpen())
        return;
    try
    {
        dc->send(reinterpret_cast<const std::byte *>(data), static_cast<size_t>(len));
    }
    catch (...)
    {
        // channel closed under us: drop (unreliable semantics; the netcode resyncs)
    }
}

void net_broadcast(int channel, int reliable, const void *data, int len)
{
    (void)reliable;
    if (channel < 0 || channel >= NET_CHAN_MAX || len <= 0 || data == nullptr)
        return;

    // Snapshot the channel handles under the lock, then send without holding it
    // (send() can block on backpressure; never hold g_peersMtx across a send).
    std::vector<std::shared_ptr<rtc::DataChannel>> targets;
    {
        std::lock_guard<std::mutex> lk(g_peersMtx);
        targets.reserve(g_peers.size());
        for (auto &kv : g_peers)
            if (kv.second && kv.second->dc[channel])
                targets.push_back(kv.second->dc[channel]);
    }
    for (auto &dc : targets)
    {
        if (dc && dc->isOpen())
        {
            try
            {
                dc->send(reinterpret_cast<const std::byte *>(data), static_cast<size_t>(len));
            }
            catch (...)
            {
                // one peer's channel gone: skip it, keep broadcasting to the rest
            }
        }
    }
}

void net_poll(void)
{
    // Drain under the lock into a local batch, then dispatch WITHOUT the lock so a
    // producer thread never blocks on the netcode's frame-parse path.
    std::deque<InboundItem> batch;
    {
        std::lock_guard<std::mutex> lk(g_inboundMtx);
        batch.swap(g_inbound);
    }
    for (auto &it : batch)
    {
        if (it.kind == InboundItem::PeerEvent)
            Net_PeerEvent(it.peer, it.event);
        else
            Net_ReceiveFrame(it.peer, it.channel, it.data.data(), static_cast<int>(it.data.size()));
    }
}

}   // extern "C"

#endif   // NETNATIVE
