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

inline int64_t nowMs() { return (int64_t)nowSec() * 1000; }

inline std::string buildSdpMsg(const std::string &from, const std::string &to, const std::string &sdp, bool isOffer)
{
    return "{\"type\":" + jsonStr(isOffer ? "offer" : "answer") + ",\"from\":" + jsonStr(from) + ",\"to\":" + jsonStr(to) +
           ",\"sdp\":" + jsonStr(sdp) + ",\"ts\":" + std::to_string(nowMs()) + "}";
}

inline std::string buildIceMsg(const std::string &from, const std::string &to, const std::string &cand, const std::string &mid)
{
    return "{\"type\":\"ice\",\"from\":" + jsonStr(from) + ",\"to\":" + jsonStr(to) + ",\"candidate\":" + jsonStr(cand) +
           ",\"sdpMid\":" + jsonStr(mid) + ",\"sdpMLineIndex\":null,\"ts\":" + std::to_string(nowMs()) + "}";
}

inline std::string buildPresenceMsg(const std::string &from, const std::string &name)
{
    return "{\"type\":\"presence\",\"from\":" + jsonStr(from) + ",\"name\":" + jsonStr(name) + ",\"ts\":" +
           std::to_string(nowMs()) + "}";
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
    return true;
}

} // namespace nn

#endif // NN_SIGNALING_HPP_
