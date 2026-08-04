//-------------------------------------------------------------------------
//
// Native (non-Emscripten) transport backend for the NetDuke32 seam.
//
// Implements net_transport.h over libdatachannel (WebRTC data channels) + a C++
// Nostr signaling client (secp256k1 + AES-256-GCM), wire-compatible with the
// browser transport (platform/emscripten/net/*.ts) so a native peer and a
// browser peer interoperate. Folds the browser stack's seam.ts + match.ts +
// duke-net.ts (join handshake, star topology, token<->slot map) into one C++
// unit; the netcode above the seam is unchanged.
//
// Compiled ONLY when NETNATIVE=1 (see GNUmakefile); replaces net_transport_stub.
//
// Configuration (env, set by the engine's -nnhost/-nnjoin args or a test):
//   NN_ROLE   = host | guest
//   NN_RELAY  = ws(s):// relay url  (default: the local test relay is passed in)
//   NN_KEY    = base64 32-byte room key (host generates one if unset and prints it)
//   NN_HOSTID = host device id (guest only; the star center to dial)
//   NN_NAME   = player name
//   NN_PLAYERS= players to wait for before the host may launch (default 2)
//
// Threading: libdatachannel + relay callbacks run on worker threads; they only
// enqueue. Net_ReceiveFrame / Net_PeerEvent / Net_SetLocalIndex are invoked ONLY
// from net_poll(), i.e. the game thread, so the netcode is never touched
// concurrently.
//-------------------------------------------------------------------------

#include "net_transport.h"

#ifdef NETNATIVE

#include "nn_relay.hpp"
#include "nn_signaling.hpp"

#include <sjson.h>   // impl provided by eduke32's sjson.o (tests supply their own)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace nn;

namespace {

// ── Inbound queue: worker threads -> game-thread net_poll ────────────────────
struct InboundItem
{
    enum Kind
    {
        Frame,
        PeerEvent,
        SetLocalIndex
    } kind;
    int peer = 0;    // connectindex/slot (Frame, PeerEvent) or slot (SetLocalIndex)
    int channel = 0; // Frame
    int event = 0;   // PeerEvent
    std::vector<uint8_t> data;
};

class NativeTransport
{
public:
    bool start()
    {
        const char *role = getenv("NN_ROLE");
        if (!role)
            return false; // native transport not requested: seam is inert (like the stub)
        isHost_ = std::string(role) == "host";
        relay_ = envOr("NN_RELAY", "");
        myName_ = envOr("NN_NAME", isHost_ ? "Host" : "Guest");
        minPlayers_ = std::atoi(envOr("NN_PLAYERS", "2").c_str());
        if (minPlayers_ < 2)
            minPlayers_ = 2;
        if (relay_.empty())
            return false;

        myDeviceId_ = "native-" + toHex(randomBytes(8));

        if (isHost_)
        {
            roomKey_ = envOr("NN_KEY", "");
            if (roomKey_.empty())
                roomKey_ = base64Encode(randomBytes(32));
            myConnectIndex_ = 0; // host is always slot 0 (connecthead)
            // Print the room info so a launcher/test can hand it to guests.
            printf("[nnet] HOSTID %s\n[nnet] KEY %s\n", myDeviceId_.c_str(), roomKey_.c_str());
            fflush(stdout);
        }
        else
        {
            roomKey_ = envOr("NN_KEY", "");
            hostId_ = envOr("NN_HOSTID", "");
            if (roomKey_.empty() || hostId_.empty())
            {
                fprintf(stderr, "[nnet] guest needs NN_KEY + NN_HOSTID\n");
                return false;
            }
        }

        sigCtx_ = sjson_create_context(0, 0, nullptr);
        nostr_ = std::make_unique<NostrClient>(std::vector<std::string>{ relay_ });
        pm_ = std::make_unique<PeerManager>(myDeviceId_, rtc::Configuration{});
        wire();
        nostr_->start();
        nostr_->subscribeEphemeral(SIGNALING_KIND, roomKey_, [this](const std::string &payload) { onSignal(payload); });
        running_ = true;
        presenceThread_ = std::thread([this] { presenceLoop(); });
        return true;
    }

    void shutdown()
    {
        running_ = false;
        if (presenceThread_.joinable())
            presenceThread_.join();
        if (pm_)
            pm_->closeAll();
        if (nostr_)
            nostr_->stop();
        if (sigCtx_)
        {
            sjson_destroy_context(sigCtx_);
            sigCtx_ = nullptr;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        inbound_.clear();
        tokenToDevice_.clear();
        deviceToToken_.clear();
    }

    // ── seam: outbound (game thread) ─────────────────────────────────────────
    void send(int token, int channel, const void *data, int len)
    {
        std::string dev;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = tokenToDevice_.find(token);
            if (it == tokenToDevice_.end())
                return;
            dev = it->second;
        }
        pm_->sendNet(dev, channel, reinterpret_cast<const uint8_t *>(data), (size_t)len);
    }

    void broadcast(int channel, const void *data, int len)
    {
        std::vector<std::string> devs;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto &kv : tokenToDevice_)
                devs.push_back(kv.second);
        }
        for (auto &d : devs)
            pm_->sendNet(d, channel, reinterpret_cast<const uint8_t *>(data), (size_t)len);
    }

    // ── seam: net_poll drains to the netcode (game thread) ───────────────────
    void poll()
    {
        std::deque<InboundItem> batch;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            batch.swap(inbound_);
        }
        for (auto &it : batch)
        {
            switch (it.kind)
            {
                case InboundItem::SetLocalIndex: Net_SetLocalIndex(it.peer); break;
                case InboundItem::PeerEvent: Net_PeerEvent(it.peer, it.event); break;
                case InboundItem::Frame:
                    Net_ReceiveFrame(it.peer, it.channel, it.data.data(), (int)it.data.size());
                    break;
            }
        }
    }

    // ── launch readiness (host only; polled by the engine main loop) ─────────
    bool shouldLaunch()
    {
        if (!isHost_ || launched_)
            return false;
        std::lock_guard<std::mutex> lk(mtx_);
        return (int)(tokenToDevice_.size() + 1) >= minPlayers_;
    }
    void markLaunched() { launched_ = true; }
    bool isHost() const { return isHost_; }

private:
    static std::string envOr(const char *k, const std::string &def)
    {
        const char *v = getenv(k);
        return v ? std::string(v) : def;
    }

    void wire()
    {
        pm_->sendSdp = [this](const std::string &to, const std::string &sdp, bool isOffer) {
            nostr_->publishEphemeral(SIGNALING_KIND, roomKey_, buildSdpMsg(myDeviceId_, to, sdp, isOffer));
        };
        pm_->sendIce = [this](const std::string &to, const std::string &cand, const std::string &mid) {
            nostr_->publishEphemeral(SIGNALING_KIND, roomKey_, buildIceMsg(myDeviceId_, to, cand, mid));
        };
        pm_->onChannelsReady = [this](const std::string &peer) { onChannelsReady(peer); };
        pm_->onControl = [this](const std::string &peer, const std::string &ctrl) { onControl(peer, ctrl); };
        pm_->onNetFrame = [this](const std::string &peer, int ch, const uint8_t *d, size_t n) {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = deviceToToken_.find(peer);
            if (it == deviceToToken_.end())
                return;
            InboundItem item;
            item.kind = InboundItem::Frame;
            item.peer = it->second;
            item.channel = ch;
            item.data.assign(d, d + n);
            inbound_.push_back(std::move(item));
        };
    }

    void presenceLoop()
    {
        // Wait briefly for the relay to open, then announce presence until connected.
        for (int t = 0; t < 3000 && nostr_->openRelayCount() == 0 && running_; t += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        while (running_)
        {
            nostr_->publishEphemeral(SIGNALING_KIND, roomKey_, buildPresenceMsg(myDeviceId_, myName_));
            if (!isHost_ && !hostId_.empty())
                pm_->connect(hostId_); // guest dials the star center
            for (int t = 0; t < 1000 && running_; t += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void onSignal(const std::string &payload)
    {
        SignalMsg m;
        {
            std::lock_guard<std::mutex> lk(sigMtx_);
            if (!parseSignal(sigCtx_, payload, m))
                return;
        }
        if (m.from == myDeviceId_)
            return; // own echo
        if (m.type == "presence")
        {
            // Host dials every guest it hears; guests only ever dial the host.
            if (isHost_)
                pm_->connect(m.from);
            return;
        }
        if (m.to != myDeviceId_)
            return;
        if (m.type == "offer")
            pm_->handleOffer(m.from, m.sdp);
        else if (m.type == "answer")
            pm_->handleAnswer(m.from, m.sdp);
        else if (m.type == "ice")
            pm_->addIce(m.from, m.candidate, m.sdpMid);
    }

    void onChannelsReady(const std::string &peer)
    {
        // Guest: once the data channels to the host open, request a slot.
        if (!isHost_ && peer == hostId_)
            pm_->sendControl(peer, "{\"t\":\"join\",\"name\":" + jsonStr(myName_) + ",\"grp\":{\"crc\":0}}");
    }

    void onControl(const std::string &peer, const std::string &ctrl)
    {
        // Parse under sigMtx_ (sjson is not thread-safe + the context is shared),
        // extract the fields we need, then release before touching other locks.
        std::string type;
        int yourSlot = -1, hostSlot = 0;
        {
            std::lock_guard<std::mutex> lk(sigMtx_);
            sjson_reset_context(sigCtx_);
            sjson_node *root = sjson_decode(sigCtx_, ctrl.c_str());
            if (!root || root->tag != SJSON_OBJECT)
                return;
            const char *t = sjson_get_string(root, "t", nullptr);
            if (!t)
                return;
            type = t;
            yourSlot = sjson_get_int(root, "yourSlot", -1);
            hostSlot = sjson_get_int(root, "hostSlot", 0);
        }
        if (type == "join" && isHost_)
            hostHandleJoin(peer);
        else if (type == "join_ok" && !isHost_)
            guestHandleJoinOk(peer, yourSlot, hostSlot);
        // rtt_ping/pong + kick: later refinement; not needed to establish a game.
    }

    void hostHandleJoin(const std::string &peer)
    {
        int slot;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (deviceToToken_.count(peer))
                return; // already joined
            slot = nextFreeSlot();
            tokenToDevice_[slot] = peer;
            deviceToToken_[peer] = slot;
            // NET_PEER_UP at connectindex==slot (drained on the game thread).
            InboundItem up;
            up.kind = InboundItem::PeerEvent;
            up.peer = slot;
            up.event = NET_PEER_UP;
            inbound_.push_back(std::move(up));
        }
        pm_->setAttached(peer, true);
        std::string ok = "{\"t\":\"join_ok\",\"yourSlot\":" + std::to_string(slot) +
                         ",\"hostSlot\":0,\"name\":" + jsonStr(myName_) + "}";
        pm_->sendControl(peer, ok);
        printf("[nnet] host: %s joined as slot %d\n", peer.c_str(), slot);
        fflush(stdout);
    }

    void guestHandleJoinOk(const std::string &peer, int yourSlot, int hostSlot)
    {
        if (yourSlot < 0)
            return;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (attached_)
                return; // already joined
            myConnectIndex_ = yourSlot;
            tokenToDevice_[hostSlot] = peer;
            deviceToToken_[peer] = hostSlot;
            // Order matters: set our own slot, THEN the host's peer-up.
            InboundItem sli;
            sli.kind = InboundItem::SetLocalIndex;
            sli.peer = yourSlot;
            inbound_.push_back(std::move(sli));
            InboundItem up;
            up.kind = InboundItem::PeerEvent;
            up.peer = hostSlot;
            up.event = NET_PEER_UP;
            inbound_.push_back(std::move(up));
            attached_ = true;
        }
        pm_->setAttached(peer, true);
        printf("[nnet] guest: joined host %s as slot %d\n", peer.c_str(), yourSlot);
        fflush(stdout);
    }

    int nextFreeSlot() // caller holds mtx_; host is 0, guests take the lowest free >=1
    {
        for (int s = 1; s < 16; s++)
            if (!tokenToDevice_.count(s))
                return s;
        return 1;
    }

    bool isHost_ = false;
    std::atomic<bool> running_{ false };
    std::atomic<bool> launched_{ false };
    bool attached_ = false;
    int myConnectIndex_ = 0;
    int minPlayers_ = 2;
    std::string relay_, roomKey_, hostId_, myName_, myDeviceId_;

    std::unique_ptr<NostrClient> nostr_;
    std::unique_ptr<PeerManager> pm_;
    sjson_context *sigCtx_ = nullptr;
    std::mutex sigMtx_;

    std::mutex mtx_;
    std::deque<InboundItem> inbound_;
    std::map<int, std::string> tokenToDevice_;
    std::map<std::string, int> deviceToToken_;
    std::thread presenceThread_;
};

std::unique_ptr<NativeTransport> g_t;
bool g_inited = false;

} // namespace

extern "C" {

void net_transport_init(void)
{
    if (g_inited)
        return;
    rtc::InitLogger(rtc::LogLevel::Warning);
    g_t = std::make_unique<NativeTransport>();
    if (!g_t->start())
        g_t.reset(); // no NN_ROLE / bad config: run inert, exactly like the stub
    g_inited = true;
}

void net_transport_shutdown(void)
{
    if (g_t)
    {
        g_t->shutdown();
        g_t.reset();
    }
    if (g_inited)
    {
        rtc::Cleanup().wait(); // async: MUST wait or static dtors abort()
        g_inited = false;
    }
}

void net_send(int peerToken, int channel, int /*reliable*/, const void *data, int len)
{
    if (g_t && data && len > 0 && channel >= 0 && channel < NET_CHAN_MAX)
        g_t->send(peerToken, channel, data, len);
}

void net_broadcast(int channel, int /*reliable*/, const void *data, int len)
{
    if (g_t && data && len > 0 && channel >= 0 && channel < NET_CHAN_MAX)
        g_t->broadcast(channel, data, len);
}

void net_poll(void)
{
    if (g_t)
        g_t->poll();
}

// ── Engine helpers (used by the native launch trigger; see game.cpp) ─────────
int net_native_enabled(void) { return g_t ? 1 : 0; }
int net_native_is_host(void) { return (g_t && g_t->isHost()) ? 1 : 0; }
int net_native_should_launch(void) { return (g_t && g_t->shouldLaunch()) ? 1 : 0; }
void net_native_mark_launched(void)
{
    if (g_t)
        g_t->markLaunched();
}

} // extern "C"

#endif // NETNATIVE
