//-------------------------------------------------------------------------
// nn_peer.hpp - WebRTC peer manager for the native transport, mirroring
// platform/emscripten/net/peer.ts:
//   * THREE data channels per peer, created by the offerer: duke-move is
//     unreliable/unordered (the tic-indexed move protocol self-repairs loss),
//     duke-rel and duke-bulk are reliable/ordered.
//   * Deterministic initiator: only the smaller device id offers (glare avoid).
//   * Non-trickle ICE: wait for gathering to complete, send a self-contained SDP.
//   * Per-peer phase gate ("attached"): before attach, strings on duke-rel are
//     transport control and binary on duke-bulk is a GRP chunk; after attach,
//     all three channels carry raw netcode frames. Strings are ALWAYS control.
//
// Signaling is delivered in/out via callbacks the owner wires to the Nostr
// layer (publish/subscribe encrypted ephemeral offer/answer/ice messages).
//-------------------------------------------------------------------------
#ifndef NN_PEER_HPP_
#define NN_PEER_HPP_

#include "nn_nostr.hpp"   // jsonStr, nowSec

#include <rtc/rtc.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>

namespace nn {

enum { CH_MOVE = 0, CH_REL = 1, CH_BULK = 2, CH_MAX = 3 };
inline const char *dcLabel(int ch) { return ch == 0 ? "duke-move" : ch == 1 ? "duke-rel" : "duke-bulk"; }
inline int dcChannel(const std::string &label)
{
    return label == "duke-move" ? CH_MOVE : label == "duke-rel" ? CH_REL : label == "duke-bulk" ? CH_BULK : -1;
}

class PeerManager
{
public:
    // Outbound signaling (owner wires these to publishEphemeral of a signaling msg).
    // gen: for ANSWERS, the ufrag of the offer generation being answered (pairing
    // against relay-redelivered stale answers); empty for offers.
    std::function<void(const std::string &toDevice, const std::string &sdp, bool isOffer, const std::string &gen)> sendSdp;
    std::function<void(const std::string &toDevice, const std::string &cand, const std::string &mid)> sendIce;

    // Inbound (invoked on libdatachannel worker threads; consumer must be thread-safe).
    std::function<void(const std::string &peer, const std::string &control)> onControl;      // string on duke-rel
    std::function<void(const std::string &peer, const uint8_t *data, size_t len)> onBulkChunk; // pre-attach binary on duke-bulk
    std::function<void(const std::string &peer, int channel, const uint8_t *data, size_t len)> onNetFrame; // post-attach binary
    std::function<void(const std::string &peer)> onChannelsReady;
    std::function<void(const std::string &peer, bool connected)> onConnectionChange;

    // Swap the ICE configuration used for NEW connections (existing pcs keep theirs).
    // Called at host/join time, before any peer connects: a Local Only match passes a
    // STUN-less config so only same-network candidate pairs can ever complete.
    void setConfig(rtc::Configuration config)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        config_ = std::move(config);
    }

    explicit PeerManager(std::string myDeviceId, rtc::Configuration config = {})
        : myId_(std::move(myDeviceId)), config_(std::move(config))
    {
    }
    ~PeerManager() { closeAll(); }

    const std::string &deviceId() const { return myId_; }

    // Begin connecting to a peer. Only the smaller device id offers.
    void connect(const std::string &peerId)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Conn *c = ensure(peerId);
        if (!c || myId_ >= peerId)
            return; // pool failure, or the larger id waits for the offer
        if (c->offered || anyChannelOpen(c))
            return;
        c->offered = true;
        createOffer(conns_[peerId]);
    }

    void handleOffer(const std::string &peerId, const std::string &sdp)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Conn *c = ensure(peerId);
        if (!c)
            return; // pool exhausted: logged in ensure(); reaper frees ports
        // Offer GENERATIONS, keyed by the ICE ufrag (unique per RTCPeerConnection):
        //  - the CURRENT conn's ufrag  -> relay redelivery: re-send our answer.
        //  - a PREVIOUSLY SEEN ufrag   -> redelivery of a superseded generation: inert.
        //  - a NEVER-SEEN ufrag        -> the peer built a fresh pc (in-tab reconnect
        //    after a drop; device ids are per-page-load so the id is reused). The old
        //    conn is dead BY DEFINITION -- one JS context never runs two sessions.
        //    Answering re-offers with the dead session's SDP made reconnects
        //    unjoinable ("Reconnects aren't working", 2026-08-16); health checks
        //    lose the race (the re-offer can beat the old conn's death detection),
        //    so generation identity, not liveness, decides.
        std::string const ufrag = sdpUfrag(sdp);
        auto &seen = seenOfferUfrags_[peerId];
        if (c->remoteSet && ufrag == c->offerUfrag)
        {
            if (c->answered && c->pc && c->pc->localDescription() && sendSdp)
                sendSdp(peerId, std::string(c->pc->localDescription()->generateSdp()), false, c->offerUfrag);
            return;
        }
        if (std::find(seen.begin(), seen.end(), ufrag) != seen.end())
            return; // superseded generation: never resurrect it
        if (c->remoteSet)
        {
            printf("[nnet] re-offer from %.16s: retiring stale conn, negotiating fresh\n", peerId.c_str());
            auto it = conns_.find(peerId);
            if (it != conns_.end())
            {
                retired_.push_back(it->second); // torn down by reapDead() outside mtx_
                conns_.erase(it);
            }
            c = ensure(peerId);
            if (!c)
                return;
        }
        try
        {
            c->pc->setRemoteDescription(rtc::Description(sdp, "offer"));
            c->remoteSet = true;
            c->offerUfrag = ufrag;
            seen.push_back(ufrag);
            if (seen.size() > 32)
                seen.erase(seen.begin());
            flushIce(c);
        }
        catch (...)
        {
        }
    }

    void handleAnswer(const std::string &peerId, const std::string &sdp, const std::string &gen = std::string())
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = conns_.find(peerId);
        if (it == conns_.end())
            return;
        Conn *c = it->second.get();
        if (c->remoteSet)
            return; // already applied (duplicate answer resend)
        // Generation pairing: an answer tagged for a DIFFERENT offer than ours
        // is a relay-redelivered stale one -- applying it poisons this pc.
        if (!gen.empty() && c->pc && c->pc->localDescription())
        {
            std::string const myUfrag = sdpUfrag(std::string(c->pc->localDescription()->generateSdp()));
            if (gen != myUfrag)
            {
                printf("[nnet] stale answer for %.16s (gen mismatch) -- ignored\n", peerId.c_str());
                return;
            }
        }
        try
        {
            c->pc->setRemoteDescription(rtc::Description(sdp, "answer"));
            c->remoteSet = true;
            flushIce(c);
        }
        catch (...)
        {
        }
    }

    void addIce(const std::string &peerId, const std::string &cand, const std::string &mid)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = conns_.find(peerId);
        if (it == conns_.end())
            return;
        Conn *c = it->second.get();
        if (!c->remoteSet)
        {
            c->iceQueue.push_back({ cand, mid });
            return;
        }
        try { c->pc->addRemoteCandidate(rtc::Candidate(cand, mid)); } catch (...) {}
    }

    void setAttached(const std::string &peerId, bool attached)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = conns_.find(peerId);
        if (it != conns_.end())
            it->second->attached = attached;
    }

    bool sendNet(const std::string &peerId, int channel, const uint8_t *data, size_t len)
    {
        if (channel < 0 || channel >= CH_MAX)
            return false;
        std::shared_ptr<rtc::DataChannel> dc;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = conns_.find(peerId);
            if (it == conns_.end())
                return false;
            dc = it->second->dc[channel];
        }
        if (!dc || !dc->isOpen())
            return false;
        try { return dc->send(reinterpret_cast<const std::byte *>(data), len); } catch (...) { return false; }
    }

    bool sendControl(const std::string &peerId, const std::string &msg)
    {
        std::shared_ptr<rtc::DataChannel> dc;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = conns_.find(peerId);
            if (it == conns_.end())
                return false;
            dc = it->second->dc[CH_REL];
        }
        if (!dc || !dc->isOpen())
            return false;
        try { return dc->send(msg); } catch (...) { return false; }
    }

    bool channelsReady(const std::string &peerId)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = conns_.find(peerId);
        return it != conns_.end() && allChannelsOpen(it->second.get());
    }

    std::vector<std::string> connectedPeers()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<std::string> v;
        for (auto &kv : conns_)
            if (allChannelsOpen(kv.second.get()))
                v.push_back(kv.first);
        return v;
    }

    void close(const std::string &peerId)
    {
        std::shared_ptr<Conn> c;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            seenOfferUfrags_.erase(peerId);
            auto it = conns_.find(peerId);
            if (it == conns_.end())
                return;
            c = it->second;
            conns_.erase(it);
        }
        teardown(c.get());
    }

    void closeAll()
    {
        std::vector<std::shared_ptr<Conn>> all;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto &kv : conns_)
                all.push_back(kv.second);
            conns_.clear();
        }
        for (auto &c : all)
            teardown(c.get());
    }

    // Reap connections that died (ICE timeout/failure/close) or never came up.
    // MUST be called OUTSIDE pc callbacks (the game-thread poll): destroying a
    // PeerConnection from inside its own callback deadlocks libdatachannel.
    // Nothing ever closed dead conns before -- every crashed tab and abandoned
    // join left an rtc::PeerConnection pinning one of the 16 pooled UDP ports
    // until the pool was exhausted and the host silently stopped answering
    // offers while still publishing announces (2026-08-15: "I can see the
    // match, just not join").
    int reapDead(std::vector<std::string> *reapedOut = nullptr)
    {
        std::vector<std::shared_ptr<Conn>> victims;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto &c : retired_) // conns replaced by a fresh re-offer
                victims.push_back(c);
            retired_.clear();
            for (auto &c : reapConns_)
            {
                // Erase from the map ONLY if the id still maps to THIS conn --
                // a re-offer may have installed a fresh replacement under it.
                auto it = conns_.find(c->peerId);
                if (it != conns_.end() && it->second == c)
                    conns_.erase(it);
                victims.push_back(c);
            }
            reapConns_.clear();
            // Zombie sweep: a conn whose channels never all opened within 75s
            // is a corpse -- an abandoned join, a gathering that could not
            // bind a pooled port (never reaches Failed), or a peer that
            // vanished mid-handshake. Established conns have readyFired set
            // and are never swept; their deaths arrive via onStateChange.
            auto const now = std::chrono::steady_clock::now();
            for (auto it = conns_.begin(); it != conns_.end();)
            {
                Conn *c = it->second.get();
                if (!c->readyFired && now - c->created > std::chrono::seconds(75))
                {
                    victims.push_back(it->second);
                    it = conns_.erase(it);
                }
                else
                    ++it;
            }
        }
        for (auto &c : victims)
        {
            if (reapedOut)
                reapedOut->push_back(c->peerId);
            teardown(c.get());
        }
        return (int)victims.size();
    }

private:
    struct Conn
    {
        std::string peerId;
        std::shared_ptr<rtc::PeerConnection> pc;
        std::shared_ptr<rtc::DataChannel> dc[CH_MAX];
        bool offered = false;
        bool answered = false;
        bool remoteSet = false;
        bool readyFired = false;
        std::atomic<bool> attached{ false };
        std::vector<std::pair<std::string, std::string>> iceQueue; // (candidate, mid) before remote set
        std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
        std::string offerUfrag; // ICE ufrag of the offer generation this conn answers
    };

    // First "a=ice-ufrag:" line of an SDP -- the per-RTCPeerConnection identity.
    static std::string sdpUfrag(const std::string &sdp)
    {
        auto p = sdp.find("a=ice-ufrag:");
        if (p == std::string::npos)
            return sdp.substr(0, 96); // malformed: fall back to a prefix identity
        p += 12;
        auto e = sdp.find_first_of("\r\n", p);
        return sdp.substr(p, e == std::string::npos ? std::string::npos : e - p);
    }

    static bool anyChannelOpen(Conn *c)
    {
        for (int i = 0; i < CH_MAX; i++)
            if (c->dc[i] && c->dc[i]->isOpen())
                return true;
        return false;
    }
    static bool allChannelsOpen(Conn *c)
    {
        for (int i = 0; i < CH_MAX; i++)
            if (!c->dc[i] || !c->dc[i]->isOpen())
                return false;
        return true;
    }

    Conn *ensure(const std::string &peerId) // caller holds mtx_
    {
        auto it = conns_.find(peerId);
        if (it != conns_.end())
            return it->second.get();
        auto c = std::make_shared<Conn>();
        c->peerId = peerId;
        try
        {
            c->pc = std::make_shared<rtc::PeerConnection>(config_);
        }
        catch (const std::exception &e)
        {
            printf("[nnet] peer pool: PeerConnection create FAILED for %.8s: %s\n", peerId.c_str(), e.what());
            return nullptr; // no map entry: a retry after the reaper runs can succeed
        }
        catch (...)
        {
            printf("[nnet] peer pool: PeerConnection create FAILED for %.8s\n", peerId.c_str());
            return nullptr;
        }
        conns_[peerId] = c;
        wirePc(c);
        return c.get();
    }

    // Every callback binds to ITS OWN Conn via weak_ptr (strong would cycle:
    // pc owns callback owns pc). NEVER key callbacks by peerId alone: after a
    // re-offer retirement the id belongs to a fresh replacement, and a stale
    // conn's late callbacks must not touch it (reap it, reset its channels).
    void wirePc(const std::shared_ptr<Conn> &c)
    {
        std::string peerId = c->peerId;
        PeerManager *self = this;
        rtc::PeerConnection *pc = c->pc.get();
        std::weak_ptr<Conn> wc = c;

        c->pc->onGatheringStateChange([self, peerId, pc, wc](rtc::PeerConnection::GatheringState state) {
            if (state != rtc::PeerConnection::GatheringState::Complete)
                return;
            auto desc = pc->localDescription();
            if (!desc || !self->sendSdp)
                return;
            bool isOffer = desc->typeString() == "offer";
            std::string gen;
            if (!isOffer)
            {
                if (auto sc = wc.lock())
                {
                    std::lock_guard<std::mutex> lk(self->mtx_);
                    sc->answered = true;
                    gen = sc->offerUfrag;
                }
            }
            self->sendSdp(peerId, std::string(desc->generateSdp()), isOffer, gen);
        });
        c->pc->onLocalCandidate([self, peerId](rtc::Candidate cand) {
            if (self->sendIce)
                self->sendIce(peerId, std::string(cand.candidate()), std::string(cand.mid()));
        });
        c->pc->onStateChange([self, peerId, wc](rtc::PeerConnection::State state) {
            bool connected = state == rtc::PeerConnection::State::Connected;
            if (state == rtc::PeerConnection::State::Failed || state == rtc::PeerConnection::State::Closed)
            {
                // Queue THIS conn for the game-thread reaper; never tear down
                // from here (destroying the pc inside its own callback
                // deadlocks libdatachannel).
                if (auto sc = wc.lock())
                {
                    std::lock_guard<std::mutex> lk(self->mtx_);
                    self->reapConns_.push_back(sc);
                }
            }
            if (self->onConnectionChange)
                self->onConnectionChange(peerId, connected);
        });
        // Answerer receives the offerer's channels here.
        c->pc->onDataChannel([self, wc](std::shared_ptr<rtc::DataChannel> dc) {
            if (auto sc = wc.lock())
            {
                std::lock_guard<std::mutex> lk(self->mtx_);
                self->setupDcLocked(sc, dc);
            }
        });
    }

    void createOffer(const std::shared_ptr<Conn> &c) // caller holds mtx_
    {
        // Offerer creates all three channels synchronously so they ride in one offer.
        for (int ch = 0; ch < CH_MAX; ch++)
        {
            rtc::DataChannelInit init;
            // duke-move is UNRELIABLE + UNORDERED: the tic-indexed move protocol
            // (oldnet.cpp) makes every packet self-contained with an ack-driven
            // resend window, so loss/reorder self-repairs and SCTP retransmits
            // of stale frames would only add head-of-line latency. duke-rel and
            // duke-bulk stay reliable+ordered (mirrors netconfig.ts DC_INIT).
            if (ch == CH_MOVE)
            {
                init.reliability.unordered = true;
                init.reliability.maxRetransmits = 0;
            }
            auto dc = c->pc->createDataChannel(dcLabel(ch), init);
            setupDcLocked(c, dc);
        }
        // gathering-complete -> onGatheringStateChange sends the self-contained offer
    }

    void setupDcLocked(const std::shared_ptr<Conn> &c, std::shared_ptr<rtc::DataChannel> dc) // caller holds mtx_
    {
        int ch = dcChannel(dc->label());
        if (ch < 0)
        {
            try { dc->close(); } catch (...) {}
            return;
        }
        c->dc[ch] = dc;
        std::string peerId = c->peerId;
        PeerManager *self = this;
        std::weak_ptr<Conn> wc = c;

        dc->onOpen([self, peerId, wc]() { self->onDcOpen(peerId, wc); });
        dc->onMessage([self, peerId, wc, ch](rtc::message_variant data) {
            if (std::holds_alternative<std::string>(data))
            {
                // Strings are ALWAYS control (netcode frames are binary), safe even
                // after attach - keeps rtt/kick/handshake working post-join.
                if (self->onControl)
                    self->onControl(peerId, std::get<std::string>(data));
                return;
            }
            const auto &bin = std::get<rtc::binary>(data);
            const uint8_t *p = reinterpret_cast<const uint8_t *>(bin.data());
            self->onDcBinary(peerId, wc, ch, p, bin.size());
        });
        dc->onClosed([self, wc, ch]() {
            if (auto sc = wc.lock())
            {
                std::lock_guard<std::mutex> lk(self->mtx_);
                sc->dc[ch].reset();
            }
        });
    }

    void onDcOpen(const std::string &peerId, const std::weak_ptr<Conn> &wc)
    {
        bool ready = false;
        if (auto sc = wc.lock())
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!sc->readyFired && allChannelsOpen(sc.get()))
            {
                sc->readyFired = true;
                ready = true;
            }
        }
        if (ready && onChannelsReady)
            onChannelsReady(peerId);
    }

    void onDcBinary(const std::string &peerId, const std::weak_ptr<Conn> &wc, int ch, const uint8_t *p, size_t len)
    {
        auto sc = wc.lock();
        if (!sc)
            return;
        bool const attached = sc->attached.load();
        if (attached)
        {
            if (onNetFrame)
                onNetFrame(peerId, ch, p, len);
        }
        else if (ch == CH_BULK && onBulkChunk)
            onBulkChunk(peerId, p, len);
    }

    void flushIce(Conn *c) // caller holds mtx_
    {
        for (auto &ic : c->iceQueue)
            try { c->pc->addRemoteCandidate(rtc::Candidate(ic.first, ic.second)); } catch (...) {}
        c->iceQueue.clear();
    }

    static void teardown(Conn *c)
    {
        for (int i = 0; i < CH_MAX; i++)
            if (c->dc[i])
                try { c->dc[i]->close(); } catch (...) {}
        if (c->pc)
            try { c->pc->close(); } catch (...) {}
    }

    std::string myId_;
    rtc::Configuration config_;
    std::mutex mtx_;
    std::map<std::string, std::shared_ptr<Conn>> conns_;
    std::vector<std::shared_ptr<Conn>> reapConns_; // conns whose pc hit Failed/Closed; drained by reapDead()
    std::vector<std::shared_ptr<Conn>> retired_;   // conns replaced by re-offers; torn down by reapDead()
    std::map<std::string, std::vector<std::string>> seenOfferUfrags_; // peerId -> answered offer generations
};

} // namespace nn

#endif // NN_PEER_HPP_
