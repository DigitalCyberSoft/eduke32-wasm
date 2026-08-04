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
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>   // getpid (per-process nnlog path)
#include <vector>

using namespace nn;

// NetMenu_* setters (defined in menus.cpp under NETMENU): the native transport drives
// the in-engine multiplayer menu directly (the browser uses the JS bridge instead).
extern "C" {
    void NetMenu_SetStatus(const char *s);
    void NetMenu_SetRoster(const char *json);
    void NetMenu_OnJoined(int myConnectIndex);
}

namespace {

// Temporary boot tracing (file-based so it survives any engine stdout handling).
// TODO(netcode): remove once native launch is confirmed live.
inline const char *nnlogPath()
{
    // Per-process file so a host and a guest running side by side don't interleave
    // into one log (that made a barrier trace look self-contradictory). Keyed on
    // NN_ROLE when the harness sets it; PID otherwise.
    static std::string path = [] {
        const char *role = getenv("NN_ROLE");
        if (role && *role)
            return std::string("/tmp/nnet_debug.") + role + ".log";
        return std::string("/tmp/nnet_debug.") + std::to_string((long)getpid()) + ".log";
    }();
    return path.c_str();
}
inline void nnlog(const std::string &m)
{
    if (FILE *f = fopen(nnlogPath(), "a"))
    {
        fprintf(f, "%s\n", m.c_str());
        fclose(f);
    }
}

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
    // Join the presence thread + tear down peers here too: if the engine exits without
    // calling net_transport_shutdown(), static destruction of g_t must not destroy a
    // joinable std::thread ("terminate called without an active exception" -> SIGABRT).
    ~NativeTransport() { shutdown(); }

    // Infrastructure only: connect the relay + peer manager. Does NOT host or join
    // until a menu action (net_native_host / net_native_join_code) or an NN_ROLE env
    // (headless testing) drives it. Returns false if crypto/relay setup fails.
    bool configure()
    {
        myName_ = envOr("NN_NAME", "Player");
        minPlayers_ = std::atoi(envOr("NN_PLAYERS", "2").c_str());
        if (minPlayers_ < 2)
            minPlayers_ = 2;
        autoLaunch_ = (getenv("NN_AUTOLAUNCH") != nullptr) || (getenv("NN_ROLE") != nullptr);

        const char *relayEnv = getenv("NN_RELAY");
        if (relayEnv && *relayEnv)
            relays_ = { relayEnv };
        else // default to a few public Nostr relays (same set as the browser transport)
            relays_ = { "wss://relay.damus.io", "wss://nos.lol", "wss://relay.primal.net" };

        myDeviceId_ = "native-" + toHex(randomBytes(8));
        sigCtx_ = sjson_create_context(0, 0, nullptr);
        nostr_ = std::make_unique<NostrClient>(relays_);
        pm_ = std::make_unique<PeerManager>(myDeviceId_, rtc::Configuration{});
        wire();
        nostr_->start();
        running_ = true;

        // Headless auto-start via env (the interactive menu uses net_native_host/join).
        const char *role = getenv("NN_ROLE");
        if (role && std::string(role) == "host")
            hostMatch(0, envOr("NN_NAME", "Duke Match"), minPlayers_, myName_, 0);
        else if (role && std::string(role) == "guest")
        {
            std::string key = envOr("NN_KEY", ""), hid = envOr("NN_HOSTID", "");
            if (!key.empty())
                joinWithKey(key, hid, myName_); // hid may be empty -> discover the host via presence
        }
        return true;
    }

    void hostMatch(int isPublic, const std::string &name, int maxPlayers, const std::string &player, int localOnly)
    {
        isHost_ = true;
        isPublic_ = isPublic != 0;
        localOnly_ = localOnly != 0;
        myConnectIndex_ = 0;
        myName_ = player.empty() ? "Host" : player;
        matchName_ = name.empty() ? "Duke Match" : name;
        maxPlayers_ = maxPlayers < 2 ? 2 : maxPlayers;
        roomKey_ = envOr("NN_KEY", "");
        if (roomKey_.empty())
            roomKey_ = base64Encode(randomBytes(32));
        invite_ = makeInvite();
        printf("[nnet] HOSTID %s\n[nnet] KEY %s\n[nnet] INVITE %s\n", myDeviceId_.c_str(), roomKey_.c_str(), invite_.c_str());
        fflush(stdout);
        // Mark our OWN slot 0 connected (game thread, via net_poll) so Net_RebuildConnectChain
        // makes connecthead=0. Without this g_player[0].connected stays 0, connecthead != 0, and
        // Net_SendNewGame's `myconnectindex != connecthead` guard drops the NEW_GAME broadcast.
        // (The guest already does this via Net_SetLocalIndex on join_ok; the host needs it too.)
        {
            std::lock_guard<std::mutex> lk(mtx_);
            InboundItem sli;
            sli.kind = InboundItem::SetLocalIndex;
            sli.peer = 0;
            inbound_.push_back(std::move(sli));
        }
        ensureSignaling();
        setStatus("Hosting. Invite code (share it): " + invite_);
        updateRoster();
    }

    void joinMatch(const std::string &code, const std::string &player)
    {
        std::string key;
        for (char c : code)
            if (!isspace((unsigned char)c))
                key += c;
        Bytes raw;
        if (key.empty() || !base64Decode(key, raw) || raw.size() != 32)
        {
            setStatus("!Invalid invite code");
            return;
        }
        joinWithKey(key, "", player); // host id discovered via presence
    }

    void joinWithKey(const std::string &key, const std::string &hid, const std::string &player)
    {
        isHost_ = false;
        roomKey_ = key;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            hostId_ = hid;
        }
        myName_ = player.empty() ? "Guest" : player;
        ensureSignaling();
        if (!hid.empty())
            pm_->connect(hid);
        setStatus("Connecting to host...");
    }

    void leave()
    {
        if (pm_)
            pm_->closeAll();
        std::lock_guard<std::mutex> lk(mtx_);
        tokenToDevice_.clear();
        deviceToToken_.clear();
        inbound_.clear();
        attached_ = false;
        launched_ = false;
    }

    void menuPump() // game thread: apply queued menu updates via the NetMenu_* setters
    {
        std::string status, roster;
        int joined = -1;
        bool hs = false, hr = false, hj = false;
        {
            std::lock_guard<std::mutex> lk(menuMtx_);
            if (hasStatus_) { status = pendingStatus_; hs = true; hasStatus_ = false; }
            if (hasRoster_) { roster = pendingRoster_; hr = true; hasRoster_ = false; }
            if (hasJoined_) { joined = pendingJoined_; hj = true; hasJoined_ = false; }
        }
        if (hs) NetMenu_SetStatus(status.c_str());
        if (hr) NetMenu_SetRoster(roster.c_str());
        if (hj) NetMenu_OnJoined(joined);
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
        // Only auto-launch in headless mode (NN_AUTOLAUNCH / NN_ROLE). Interactive
        // hosting waits for the user to pick Launch Game in the Multiplayer menu.
        if (!autoLaunch_ || !isHost_ || launched_)
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
            try
            {
                nostr_->publishEphemeral(SIGNALING_KIND, roomKey_, buildPresenceMsg(myDeviceId_, myName_, isHost_));
                if (!isHost_)
                {
                    std::string h;
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        h = hostId_;
                    }
                    if (!h.empty())
                        pm_->connect(h); // guest dials the discovered star center
                }
            }
            catch (const std::exception &e)
            {
                nnlog(std::string("presence loop caught: ") + e.what());
            }
            catch (...)
            {
            }
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
            if (isHost_)
            {
                pm_->connect(m.from); // host dials every guest it hears
            }
            else if (m.host) // a host announced itself: this is our star center
            {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (hostId_.empty())
                        hostId_ = m.from;
                }
                pm_->connect(m.from);
            }
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
        if (isHost_)
            return; // host waits for the guest's join control message
        // Guest: in a STAR topology the peer we connected to IS the host, so send join
        // unconditionally (do NOT gate on hostId_ == peer: the host's OFFER can arrive and
        // open channels BEFORE its presence is processed, leaving hostId_ momentarily empty
        // -> the join was never sent -> the host never assigned a slot. That was the race).
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (hostId_.empty())
                hostId_ = peer;
        }
        pm_->sendControl(peer, "{\"t\":\"join\",\"name\":" + jsonStr(myName_) + ",\"grp\":{\"crc\":0}}");
    }

    void onControl(const std::string &peer, const std::string &ctrl)
    {
        // Parse under sigMtx_ (sjson is not thread-safe + the context is shared),
        // extract the fields we need, then release before touching other locks.
        std::string type, joinName;
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
            const char *jn = sjson_get_string(root, "name", nullptr);
            if (jn)
                joinName = jn;
        }
        if (type == "join" && isHost_)
            hostHandleJoin(peer, joinName);
        else if (type == "join_ok" && !isHost_)
            guestHandleJoinOk(peer, yourSlot, hostSlot, joinName); // joinName = host's name
        // rtt_ping/pong + kick: later refinement; not needed to establish a game.
    }

    void hostHandleJoin(const std::string &peer, const std::string &name)
    {
        int slot;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (deviceToToken_.count(peer))
                return; // already joined
            slot = nextFreeSlot();
            tokenToDevice_[slot] = peer;
            deviceToToken_[peer] = slot;
            deviceName_[peer] = name.empty() ? "Player" : name;
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
        updateRoster();
        setStatus("Player joined - press Launch Game to start.");
    }

    void guestHandleJoinOk(const std::string &peer, int yourSlot, int hostSlot,
                           const std::string &hostName)
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
        {
            std::lock_guard<std::mutex> lk(mtx_);
            deviceName_[peer] = hostName.empty() ? "Host" : hostName;
        }
        pm_->setAttached(peer, true);
        printf("[nnet] guest: joined host %s as slot %d\n", peer.c_str(), yourSlot);
        fflush(stdout);
        setJoined(yourSlot); // NetMenu_OnJoined -> advance the menu to the lobby
        updateRoster();      // guests list [host, me] (star: peers via presence later)
        setStatus("Connected - waiting for the host to start...");
    }

    int nextFreeSlot() // caller holds mtx_; host is 0, guests take the lowest free >=1
    {
        for (int s = 1; s < 16; s++)
            if (!tokenToDevice_.count(s))
                return s;
        return 1;
    }

    void ensureSignaling() // subscribe to the room + start presence (once per match)
    {
        if (signalingStarted_)
            return;
        signalingStarted_ = true;
        nostr_->subscribeEphemeral(SIGNALING_KIND, roomKey_, [this](const std::string &payload) { onSignal(payload); });
        if (!presenceThread_.joinable())
            presenceThread_ = std::thread([this] { presenceLoop(); });
    }

    // The invite IS the room key (base64, ~44 chars: fits the 80-char join field). The
    // joiner discovers the host via presence (host flag), so the code needs no host id.
    std::string makeInvite() { return roomKey_; }

    void setStatus(const std::string &s)
    {
        std::lock_guard<std::mutex> lk(menuMtx_);
        pendingStatus_ = s;
        hasStatus_ = true;
    }
    void setJoined(int slot)
    {
        std::lock_guard<std::mutex> lk(menuMtx_);
        pendingJoined_ = slot;
        hasJoined_ = true;
    }
    void updateRoster() // build [{name,connected,ping}] in SLOT order for NetMenu_SetRoster
    {
        std::string json = "[";
        {
            std::lock_guard<std::mutex> lk(mtx_);
            std::map<int, std::string> rows; // slot -> display name
            for (auto &kv : tokenToDevice_)
            {
                auto n = deviceName_.find(kv.second);
                rows[kv.first] = n != deviceName_.end() ? n->second : std::string("Player");
            }
            rows[myConnectIndex_] = myName_; // self at the host-assigned slot
            bool first = true;
            for (auto &kv : rows)
            {
                if (!first) json += ",";
                first = false;
                json += "{\"name\":" + jsonStr(kv.second) + ",\"connected\":true,\"ping\":" +
                        (kv.first == myConnectIndex_ ? std::string("0") : std::string("-1")) + "}";
            }
        }
        json += "]";
        std::lock_guard<std::mutex> lk(menuMtx_);
        pendingRoster_ = json;
        hasRoster_ = true;
    }

    bool isHost_ = false;
    std::atomic<bool> running_{ false };
    std::atomic<bool> launched_{ false };
    bool attached_ = false;
    bool signalingStarted_ = false;
    bool isPublic_ = false;
    bool localOnly_ = false;
    bool autoLaunch_ = false;
    int myConnectIndex_ = 0;
    int minPlayers_ = 2;
    int maxPlayers_ = 8;
    std::string roomKey_, hostId_, myName_, myDeviceId_, matchName_, invite_;
    std::vector<std::string> relays_;

    std::unique_ptr<NostrClient> nostr_;
    std::unique_ptr<PeerManager> pm_;
    sjson_context *sigCtx_ = nullptr;
    std::mutex sigMtx_;

    std::mutex mtx_;
    std::deque<InboundItem> inbound_;
    std::map<int, std::string> tokenToDevice_;
    std::map<std::string, int> deviceToToken_;
    std::map<std::string, std::string> deviceName_; // peer deviceId -> display name
    std::thread presenceThread_;

    // Menu update queue (produced on worker threads, applied by menuPump on the game thread).
    std::mutex menuMtx_;
    std::string pendingStatus_, pendingRoster_;
    int pendingJoined_ = -1;
    bool hasStatus_ = false, hasRoster_ = false, hasJoined_ = false;
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
    bool ok = false;
    try
    {
        ok = g_t->configure();
    }
    catch (const std::exception &e)
    {
        nnlog(std::string("configure() threw: ") + e.what());
    }
    catch (...)
    {
        nnlog("configure() threw: unknown");
    }
    if (!ok)
        g_t.reset(); // setup failed: run inert, exactly like the stub
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

// ── Menu action bridge (called from menus.cpp on native; see NETMENU) ────────
void net_native_host(int isPublic, const char *name, int maxPlayers, const char *player, int localOnly)
{
    if (g_t)
        g_t->hostMatch(isPublic, name ? name : "", maxPlayers, player ? player : "", localOnly);
}
void net_native_join_code(const char *code, const char *player)
{
    if (g_t)
        g_t->joinMatch(code ? code : "", player ? player : "");
}
void net_native_leave(void)
{
    if (g_t)
        g_t->leave();
}
void net_native_browse(int /*start*/) { /* native public-match browse not wired yet */ }
void net_native_set_ingame(int /*in_game*/) { /* native advert / accept-gate not wired yet */ }
void net_native_menu_pump(void)
{
    if (g_t)
        g_t->menuPump();
}

} // extern "C"

#endif // NETNATIVE
