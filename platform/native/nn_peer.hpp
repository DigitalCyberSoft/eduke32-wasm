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
    std::function<void(const std::string &toDevice, const std::string &sdp, bool isOffer)> sendSdp;
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
        ensure(peerId);
        if (myId_ >= peerId)
            return; // the larger id waits for the offer
        Conn *c = conns_[peerId].get();
        if (c->offered || anyChannelOpen(c))
            return;
        c->offered = true;
        createOffer(peerId, c);
    }

    void handleOffer(const std::string &peerId, const std::string &sdp)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        Conn *c = ensure(peerId);
        // Duplicate offer after we already answered: re-send the answer.
        if (c->remoteSet && c->answered && c->pc && c->pc->localDescription())
        {
            if (sendSdp)
                sendSdp(peerId, std::string(c->pc->localDescription()->generateSdp()), false);
            return;
        }
        try
        {
            c->pc->setRemoteDescription(rtc::Description(sdp, "offer"));
            c->remoteSet = true;
            flushIce(c);
        }
        catch (...)
        {
        }
    }

    void handleAnswer(const std::string &peerId, const std::string &sdp)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = conns_.find(peerId);
        if (it == conns_.end())
            return;
        Conn *c = it->second.get();
        if (c->remoteSet)
            return; // already applied (duplicate answer resend)
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
    };

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
        c->pc = std::make_shared<rtc::PeerConnection>(config_);
        conns_[peerId] = c;
        wirePc(c.get());
        return c.get();
    }

    void wirePc(Conn *c)
    {
        std::string peerId = c->peerId;
        PeerManager *self = this;
        rtc::PeerConnection *pc = c->pc.get();

        c->pc->onGatheringStateChange([self, peerId, pc](rtc::PeerConnection::GatheringState state) {
            if (state != rtc::PeerConnection::GatheringState::Complete)
                return;
            auto desc = pc->localDescription();
            if (!desc || !self->sendSdp)
                return;
            bool isOffer = desc->typeString() == "offer";
            if (!isOffer)
                self->markAnswered(peerId);
            self->sendSdp(peerId, std::string(desc->generateSdp()), isOffer);
        });
        c->pc->onLocalCandidate([self, peerId](rtc::Candidate cand) {
            if (self->sendIce)
                self->sendIce(peerId, std::string(cand.candidate()), std::string(cand.mid()));
        });
        c->pc->onStateChange([self, peerId](rtc::PeerConnection::State state) {
            bool connected = state == rtc::PeerConnection::State::Connected;
            if (self->onConnectionChange)
                self->onConnectionChange(peerId, connected);
        });
        // Answerer receives the offerer's channels here.
        c->pc->onDataChannel([self, peerId](std::shared_ptr<rtc::DataChannel> dc) {
            self->setupDc(peerId, dc);
        });
    }

    void createOffer(const std::string &peerId, Conn *c) // caller holds mtx_
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

    void markAnswered(const std::string &peerId)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = conns_.find(peerId);
        if (it != conns_.end())
            it->second->answered = true;
    }

    void setupDc(const std::string &peerId, std::shared_ptr<rtc::DataChannel> dc)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = conns_.find(peerId);
        if (it == conns_.end())
            return;
        setupDcLocked(it->second.get(), dc);
    }

    void setupDcLocked(Conn *c, std::shared_ptr<rtc::DataChannel> dc) // caller holds mtx_
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

        dc->onOpen([self, peerId]() { self->onDcOpen(peerId); });
        dc->onMessage([self, peerId, ch](rtc::message_variant data) {
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
            self->onDcBinary(peerId, ch, p, bin.size());
        });
        dc->onClosed([self, peerId, ch]() { self->onDcClosed(peerId, ch); });
    }

    void onDcOpen(const std::string &peerId)
    {
        bool ready = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = conns_.find(peerId);
            if (it == conns_.end())
                return;
            Conn *c = it->second.get();
            if (!c->readyFired && allChannelsOpen(c))
            {
                c->readyFired = true;
                ready = true;
            }
        }
        if (ready && onChannelsReady)
            onChannelsReady(peerId);
    }

    void onDcBinary(const std::string &peerId, int ch, const uint8_t *p, size_t len)
    {
        bool attached = false;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = conns_.find(peerId);
            if (it == conns_.end())
                return;
            attached = it->second->attached.load();
        }
        if (attached)
        {
            if (onNetFrame)
                onNetFrame(peerId, ch, p, len);
        }
        else if (ch == CH_BULK && onBulkChunk)
            onBulkChunk(peerId, p, len);
    }

    void onDcClosed(const std::string &peerId, int ch)
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = conns_.find(peerId);
        if (it != conns_.end())
            it->second->dc[ch].reset();
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
};

} // namespace nn

#endif // NN_PEER_HPP_
