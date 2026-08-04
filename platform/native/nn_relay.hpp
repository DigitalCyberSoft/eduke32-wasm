//-------------------------------------------------------------------------
// nn_relay.hpp - Nostr relay client over WebSockets (libdatachannel), mirroring
// nostr.ts SimplePool usage: publish/subscribe encrypted ephemeral (signaling)
// and replaceable (lobby) events keyed by the room's derived identity.
//
// Threading: rtc::WebSocket callbacks fire on libdatachannel worker threads
// (serialized per socket). The subscription table + dedup set are shared, so
// they are mutex-guarded. Each relay owns its own sjson context (sjson is not
// thread-safe, but per-socket callbacks are serialized).
//-------------------------------------------------------------------------
#ifndef NN_RELAY_HPP_
#define NN_RELAY_HPP_

#include "nn_nostr.hpp"

#include <rtc/rtc.hpp>
#include <sjson.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nn {

// Decrypted-payload callbacks. Invoked on a relay worker thread; the consumer
// must be thread-safe.
using EphemeralCb   = std::function<void(const std::string &payloadJson)>;
using ReplaceableCb = std::function<void(const std::string &dTag, const std::string &payloadJson, int64_t createdAt)>;

class NostrClient
{
public:
    explicit NostrClient(std::vector<std::string> relays) : relays_(std::move(relays)) {}
    ~NostrClient() { stop(); }

    void start()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (started_)
            return;
        started_ = true;
        for (const auto &url : relays_)
            connectRelay(url);
    }

    void stop()
    {
        std::vector<std::shared_ptr<Conn>> conns;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!started_)
                return;
            started_ = false;
            conns.swap(conns_);
            subs_.clear();
        }
        for (auto &c : conns) // close outside the lock (callbacks may re-enter)
        {
            if (c->ws)
                try { c->ws->close(); } catch (...) {}
            if (c->ctx)
                sjson_destroy_context(c->ctx);
        }
    }

    void publishEphemeral(int kind, const std::string &b64Key, const std::string &payloadJson)
    {
        std::string ev;
        if (buildEvent(kind, b64Key, {}, payloadJson, ev))
            broadcast("[\"EVENT\"," + ev + "]");
    }

    void publishReplaceable(int kind, const std::string &b64Key, const std::string &dTag, const std::string &payloadJson)
    {
        std::string ev;
        if (buildEvent(kind, b64Key, { { "d", dTag } }, payloadJson, ev))
            broadcast("[\"EVENT\"," + ev + "]");
    }

    // Subscribe to ephemeral events for the room key; returns a sub id for unsub.
    std::string subscribeEphemeral(int kind, const std::string &b64Key, EphemeralCb cb)
    {
        auto sub = makeSub(kind, b64Key, /*replaceable*/ false);
        if (!sub)
            return "";
        sub->onEphemeral = std::move(cb);
        registerAndReq(sub);
        return sub->id;
    }

    std::string subscribeReplaceable(int kind, const std::string &b64Key, ReplaceableCb cb)
    {
        auto sub = makeSub(kind, b64Key, /*replaceable*/ true);
        if (!sub)
            return "";
        sub->onReplaceable = std::move(cb);
        registerAndReq(sub);
        return sub->id;
    }

    void unsubscribe(const std::string &subId)
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            subs_.erase(subId);
        }
        broadcast("[\"CLOSE\"," + jsonStr(subId) + "]");
    }

    // Number of relays whose WebSocket is currently open (test/diagnostic).
    int openRelayCount()
    {
        std::lock_guard<std::mutex> lk(mtx_);
        int n = 0;
        for (auto &c : conns_)
            if (c->open.load())
                n++;
        return n;
    }

private:
    struct Conn
    {
        std::string url;
        std::shared_ptr<rtc::WebSocket> ws;
        sjson_context *ctx = nullptr;
        std::atomic<bool> open{ false };
        std::mutex sendMtx;
        std::deque<std::string> backlog;   // messages queued before the socket opened
    };

    struct Sub
    {
        std::string id;
        Bytes key;
        std::string pubkey;
        int kind = 0;
        bool replaceable = false;
        EphemeralCb onEphemeral;
        ReplaceableCb onReplaceable;
    };

    bool buildEvent(int kind, const std::string &b64Key, const Tags &tags, const std::string &payloadJson, std::string &out)
    {
        Bytes raw;
        if (!base64Decode(b64Key, raw) || raw.size() != 32)
            return false;
        Bytes sk;
        std::string pub;
        if (!Secp::instance().deriveFromRawKey(raw, sk, pub))
            return false;
        std::string content = aesGcmEncryptB64(raw, payloadJson);
        NostrEvent ev;
        if (!buildSignedEvent(sk, pub, kind, nowSec(), tags, content, ev))
            return false;
        out = eventToJson(ev);
        return true;
    }

    std::shared_ptr<Sub> makeSub(int kind, const std::string &b64Key, bool replaceable)
    {
        Bytes raw;
        if (!base64Decode(b64Key, raw) || raw.size() != 32)
            return nullptr;
        Bytes sk;
        std::string pub;
        if (!Secp::instance().deriveFromRawKey(raw, sk, pub))
            return nullptr;
        auto s = std::make_shared<Sub>();
        s->id = "s" + std::to_string(nextSubId_++);
        s->key = raw;
        s->pubkey = pub;
        s->kind = kind;
        s->replaceable = replaceable;
        return s;
    }

    std::string filterFor(const Sub &s)
    {
        std::string f = "{\"kinds\":[" + std::to_string(s.kind) + "],\"authors\":[" + jsonStr(s.pubkey) + "]";
        if (!s.replaceable)   // ephemeral (signaling): only new events, since now
            f += ",\"since\":" + std::to_string(nowSec());
        f += "}";
        return f;
    }

    void registerAndReq(const std::shared_ptr<Sub> &sub)
    {
        std::string req = "[\"REQ\"," + jsonStr(sub->id) + "," + filterFor(*sub) + "]";
        {
            std::lock_guard<std::mutex> lk(mtx_);
            subs_[sub->id] = sub;
        }
        broadcast(req);
    }

    void connectRelay(const std::string &url)   // caller holds mtx_
    {
        auto c = std::make_shared<Conn>();
        c->url = url;
        c->ctx = sjson_create_context(0, 0, nullptr);
        auto ws = std::make_shared<rtc::WebSocket>();
        c->ws = ws;
        conns_.push_back(c);

        std::weak_ptr<Conn> wc = c;
        NostrClient *self = this;
        ws->onOpen([self, wc]() {
            if (auto c = wc.lock())
            {
                printf("[nnet] relay up %s\n", c->url.c_str());
                fflush(stdout);
                self->onOpen(c.get());
            }
        });
        ws->onMessage([self, wc](rtc::message_variant data) {
            if (!std::holds_alternative<std::string>(data))
                return;
            if (auto c = wc.lock())
                self->onMessage(c.get(), std::get<std::string>(data));
        });
        ws->onClosed([wc]() {
            if (auto c = wc.lock())
            {
                if (c->open.load())
                    printf("[nnet] relay down %s\n", c->url.c_str());
                c->open.store(false);
            }
        });
        // Never swallow relay errors silently again: three dead wss relays once looked
        // like an empty room and cost a full debugging session.
        ws->onError([wc](std::string e) {
            if (auto c = wc.lock())
            {
                printf("[nnet] relay error %s: %s\n", c->url.c_str(), e.c_str());
                fflush(stdout);
            }
        });
        try { ws->open(url); }
        catch (const std::exception &e)
        {
            printf("[nnet] relay open failed %s: %s\n", url.c_str(), e.what());
            fflush(stdout);
        }
    }

    void onOpen(Conn *c)
    {
        c->open.store(true);
        // (Re)issue every active subscription to this relay.
        std::vector<std::string> reqs;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto &kv : subs_)
                reqs.push_back("[\"REQ\"," + jsonStr(kv.second->id) + "," + filterFor(*kv.second) + "]");
        }
        for (auto &r : reqs)
            sendRaw(c, r);
        // Flush anything queued before the socket opened.
        std::deque<std::string> backlog;
        {
            std::lock_guard<std::mutex> lk(c->sendMtx);
            backlog.swap(c->backlog);
        }
        for (auto &m : backlog)
            sendRaw(c, m);
    }

    void sendRaw(Conn *c, const std::string &msg)
    {
        if (!c->open.load())
        {
            std::lock_guard<std::mutex> lk(c->sendMtx);
            if (!c->open.load())
            {
                c->backlog.push_back(msg);
                return;
            }
        }
        try { c->ws->send(msg); } catch (...) {}
    }

    void broadcast(const std::string &msg)
    {
        std::vector<std::shared_ptr<Conn>> conns;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            conns = conns_;
        }
        for (auto &c : conns)
            sendRaw(c.get(), msg);
    }

    // Parse ["EVENT",subid,ev] etc. on a relay thread; verify + decrypt + dispatch.
    void onMessage(Conn *c, const std::string &msg)
    {
        sjson_reset_context(c->ctx);
        sjson_node *root = sjson_decode(c->ctx, msg.c_str());
        if (!root || root->tag != SJSON_ARRAY)
            return;
        sjson_node *typeNode = sjson_first_child(root);
        if (!typeNode || typeNode->tag != SJSON_STRING)
            return;
        if (std::string(typeNode->string_) != "EVENT")
            return;   // EOSE / OK / NOTICE: ignored (signaling doesn't need them)
        sjson_node *subNode = typeNode->next;
        sjson_node *evNode = subNode ? subNode->next : nullptr;
        if (!subNode || subNode->tag != SJSON_STRING || !evNode || evNode->tag != SJSON_OBJECT)
            return;
        std::string subId = subNode->string_;

        const char *id = sjson_get_string(evNode, "id", nullptr);
        const char *pubkey = sjson_get_string(evNode, "pubkey", nullptr);
        const char *content = sjson_get_string(evNode, "content", nullptr);
        const char *sig = sjson_get_string(evNode, "sig", nullptr);
        if (!id || !pubkey || !content || !sig)
            return;
        int kind = sjson_get_int(evNode, "kind", -1);
        int64_t created_at = (int64_t)sjson_get_double(evNode, "created_at", 0);

        // dedup across relays
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (seen_.count(id))
                return;
            if (seen_.size() > 4096)
                seen_.clear();
            seen_.insert(id);
        }

        Tags tags;
        std::string dTag;
        if (sjson_node *tagsNode = sjson_find_member(evNode, "tags"))
        {
            if (tagsNode->tag == SJSON_ARRAY)
            {
                sjson_node *tagArr;
                sjson_foreach(tagArr, tagsNode)
                {
                    std::vector<std::string> t;
                    sjson_node *el;
                    sjson_foreach(el, tagArr)
                        if (el->tag == SJSON_STRING)
                            t.push_back(el->string_);
                    if (t.size() >= 2 && t[0] == "d")
                        dTag = t[1];
                    tags.push_back(std::move(t));
                }
            }
        }

        // Verify id + schnorr signature (matches nostr-tools verifyEvent).
        std::string recomputed = toHex(sha256(serializeForId(pubkey, created_at, kind, tags, content)));
        if (recomputed != id)
            return;
        Bytes sigB, idB, pubB;
        if (!fromHex(sig, sigB) || sigB.size() != 64 || !fromHex(id, idB) || idB.size() != 32 ||
            !fromHex(pubkey, pubB) || pubB.size() != 32)
            return;
        if (!Secp::instance().verify(sigB, idB.data(), pubB))
            return;

        // Look up the subscription and decrypt with its room key.
        std::shared_ptr<Sub> sub;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            auto it = subs_.find(subId);
            if (it != subs_.end())
                sub = it->second;
        }
        if (!sub)
            return;
        std::string payload;
        if (!aesGcmDecryptB64(content, sub->key, payload))
            return;   // wrong key / tampered: ignore (matches TS `decrypt -> null`)
        if (sub->replaceable)
        {
            if (sub->onReplaceable)
                sub->onReplaceable(dTag, payload, created_at);
        }
        else if (sub->onEphemeral)
            sub->onEphemeral(payload);
    }

    std::vector<std::string> relays_;
    std::mutex mtx_;
    bool started_ = false;
    std::vector<std::shared_ptr<Conn>> conns_;
    std::unordered_map<std::string, std::shared_ptr<Sub>> subs_;
    std::unordered_set<std::string> seen_;
    std::atomic<int> nextSubId_{ 1 };
};

} // namespace nn

#endif // NN_RELAY_HPP_
