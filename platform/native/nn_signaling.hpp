//-------------------------------------------------------------------------
// nn_signaling.hpp - signaling message build/parse, matching signaling.ts.
//
// Payloads (encrypted into the SIGNALING_KIND event content):
//   offer/answer : {type,from,to,sdp,ts}
//   ice          : {type:"ice",from,to,candidate,sdpMid,sdpMLineIndex,ts}
//   presence     : {type:"presence",from,name,ts}
//
// Build is dependency-light; parse uses sjson (the caller supplies a context,
// since sjson is not thread-safe).
//-------------------------------------------------------------------------
#ifndef NN_SIGNALING_HPP_
#define NN_SIGNALING_HPP_

#include "nn_peer.hpp"

#include <sjson.h>

#include <string>

namespace nn {

// Nostr kinds (netconfig.ts)
static const int SIGNALING_KIND = 20079; // ephemeral
static const int LOBBY_KIND = 30078;     // replaceable

// Fixed PUBLIC Duke lobby channel key -- MUST byte-match netconfig.ts:169. Every
// client derives the SAME Nostr identity from it, so public hosts all announce
// under one author (keyed per-match by the d-tag = matchId); the browser lists
// matches by subscribing to LOBBY_KIND under that author. Content stays AES-GCM
// encrypted under this key, so only Duke clients can read the lobby.
static const char *PUBLIC_LOBBY_KEY = "MqBAIxP3Lwawq+18BL1KSjAdlTxfVtoERfmgszaEKnc=";

inline int64_t nowMs() { return (int64_t)nowSec() * 1000; }

// `gen` (answers only, optional): the ICE ufrag of the OFFER this answer pairs
// with. Relays redeliver old signaling for minutes; without pairing, a
// redelivered stale answer can beat the real one to a reconnecting offerer's
// fresh pc and poison it (wrong ufrag/DTLS fingerprint -> channels never open).
// Optional JSON field: old receivers ignore it, old senders omit it.
inline std::string buildSdpMsg(const std::string &from, const std::string &to, const std::string &sdp, bool isOffer,
                               const std::string &gen = std::string())
{
    return "{\"type\":" + jsonStr(isOffer ? "offer" : "answer") + ",\"from\":" + jsonStr(from) + ",\"to\":" + jsonStr(to) +
           ",\"sdp\":" + jsonStr(sdp) + (gen.empty() ? std::string() : ",\"gen\":" + jsonStr(gen)) +
           ",\"ts\":" + std::to_string(nowMs()) + "}";
}

inline std::string buildIceMsg(const std::string &from, const std::string &to, const std::string &cand, const std::string &mid)
{
    return "{\"type\":\"ice\",\"from\":" + jsonStr(from) + ",\"to\":" + jsonStr(to) + ",\"candidate\":" + jsonStr(cand) +
           ",\"sdpMid\":" + jsonStr(mid) + ",\"sdpMLineIndex\":null,\"ts\":" + std::to_string(nowMs()) + "}";
}

// `isHost` lets a guest that joined with only the room key discover which peer is
// the star center (so the invite code need not carry the host device id).
inline std::string buildPresenceMsg(const std::string &from, const std::string &name, bool isHost = false)
{
    return "{\"type\":\"presence\",\"from\":" + jsonStr(from) + ",\"name\":" + jsonStr(name) +
           ",\"host\":" + (isHost ? "true" : "false") + ",\"ts\":" + std::to_string(nowMs()) + "}";
}

struct SignalMsg
{
    std::string type;   // offer | answer | ice | presence
    std::string from;
    std::string to;
    std::string sdp;
    std::string candidate;
    std::string sdpMid;
    std::string name;
    std::string gen;    // answers: ufrag of the offer generation being answered
    bool host = false;  // presence: is the sender the match host?
};

// Parse a decrypted signaling payload. Returns false on malformed input.
inline bool parseSignal(sjson_context *ctx, const std::string &json, SignalMsg &out)
{
    sjson_reset_context(ctx);
    sjson_node *root = sjson_decode(ctx, json.c_str());
    if (!root || root->tag != SJSON_OBJECT)
        return false;
    const char *type = sjson_get_string(root, "type", nullptr);
    if (!type)
        return false;
    out.type = type;
    auto get = [&](const char *k) { const char *v = sjson_get_string(root, k, nullptr); return v ? std::string(v) : std::string(); };
    out.from = get("from");
    out.to = get("to");
    out.sdp = get("sdp");
    out.candidate = get("candidate");
    out.sdpMid = get("sdpMid");
    out.name = get("name");
    out.gen = get("gen");
    out.host = sjson_get_bool(root, "host", false);
    return true;
}

} // namespace nn

#endif // NN_SIGNALING_HPP_
