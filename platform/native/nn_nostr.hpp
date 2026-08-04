//-------------------------------------------------------------------------
// nn_nostr.hpp - NIP-01 events + relay client for the native transport,
// byte-compatible with nostr.ts / nostr-tools.
//
//   * event id  = sha256( JSON [0,pubkey,created_at,kind,tags,content] )
//   * signature = BIP-340 schnorr over the id (nostr-tools finalizeEvent)
//   * relay wire = ["EVENT",ev] / ["REQ",subid,filter] / ["EVENT",subid,ev] /
//                  ["EOSE",subid] / ["OK",id,ok,msg]  over a WebSocket
//
// Event BUILD is dependency-light (hand-rolled exact-format JSON, since the
// signed fields are all ASCII: hex pubkey, base64 content, ASCII tags). Event
// PARSE uses the repo-bundled sjson. Transport uses rtc::WebSocket for relays.
//-------------------------------------------------------------------------
#ifndef NN_NOSTR_HPP_
#define NN_NOSTR_HPP_

#include "nn_crypto.hpp"

#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace nn {

// ── JSON string escaping, matching ECMAScript JSON.stringify ─────────────────
// Escapes " \ and control chars (U+0000..U+001F) as \b \t \n \f \r or \u00XX;
// bytes >= 0x20 (incl. '/' and raw UTF-8) pass through. This is exactly what
// nostr-tools' serializeEvent relies on, so the id matches byte-for-byte.
inline std::string jsonEscape(const std::string &s)
{
    static const char *H = "0123456789abcdef";
    std::string o;
    o.reserve(s.size() + 2);
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\t': o += "\\t";  break;
            case '\n': o += "\\n";  break;
            case '\f': o += "\\f";  break;
            case '\r': o += "\\r";  break;
            default:
                if (c < 0x20)
                {
                    o += "\\u00";
                    o += H[c >> 4];
                    o += H[c & 0xF];
                }
                else
                    o += (char)c;
        }
    }
    return o;
}
inline std::string jsonStr(const std::string &s) { return "\"" + jsonEscape(s) + "\""; }

// ── Nostr event ──────────────────────────────────────────────────────────────
using Tags = std::vector<std::vector<std::string>>;

struct NostrEvent
{
    std::string id;         // hex sha256 of the serialization
    std::string pubkey;     // hex x-only
    int64_t created_at = 0;
    int kind = 0;
    Tags tags;
    std::string content;    // opaque (base64 ciphertext for our use)
    std::string sig;        // hex schnorr
};

inline std::string tagsToJson(const Tags &tags)
{
    std::string o = "[";
    for (size_t i = 0; i < tags.size(); i++)
    {
        if (i) o += ",";
        o += "[";
        for (size_t j = 0; j < tags[i].size(); j++)
        {
            if (j) o += ",";
            o += jsonStr(tags[i][j]);
        }
        o += "]";
    }
    o += "]";
    return o;
}

// The exact preimage nostr-tools hashes: [0,pubkey,created_at,kind,tags,content].
inline std::string serializeForId(const std::string &pubkey, int64_t created_at, int kind,
                                  const Tags &tags, const std::string &content)
{
    return "[0," + jsonStr(pubkey) + "," + std::to_string(created_at) + "," + std::to_string(kind) + "," +
           tagsToJson(tags) + "," + jsonStr(content) + "]";
}

// Build + sign a full event (mirrors nostr-tools finalizeEvent).
inline bool buildSignedEvent(const Bytes &sk, const std::string &pubHex, int kind, int64_t created_at,
                             const Tags &tags, const std::string &content, NostrEvent &out)
{
    std::string ser = serializeForId(pubHex, created_at, kind, tags, content);
    Bytes id = sha256(ser);
    Bytes sig;
    if (!Secp::instance().sign(sk, id.data(), sig))
        return false;
    out.id = toHex(id);
    out.pubkey = pubHex;
    out.created_at = created_at;
    out.kind = kind;
    out.tags = tags;
    out.content = content;
    out.sig = toHex(sig);
    return true;
}

// Full event object as JSON (key order irrelevant to relays; id is already fixed).
inline std::string eventToJson(const NostrEvent &ev)
{
    return "{\"id\":" + jsonStr(ev.id) + ",\"pubkey\":" + jsonStr(ev.pubkey) +
           ",\"created_at\":" + std::to_string(ev.created_at) + ",\"kind\":" + std::to_string(ev.kind) +
           ",\"tags\":" + tagsToJson(ev.tags) + ",\"content\":" + jsonStr(ev.content) +
           ",\"sig\":" + jsonStr(ev.sig) + "}";
}

inline int64_t nowSec() { return (int64_t)time(nullptr); }

} // namespace nn

#endif // NN_NOSTR_HPP_
