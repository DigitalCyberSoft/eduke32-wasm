#pragma once
// GRP fingerprint for the cross-play join gate. Byte-matches the browser's
// grp.ts contract:
//   crc      = standard CRC-32 (zlib polynomial) of the file bytes
//   sha256   = lowercase hex of SHA-256(file bytes)
//   setDigest= sha256Hex( utf8( join("|", "<sha256>:<crc as unsigned decimal>")
//              over the ordered component list ) )   [grp.ts setFingerprint]
// The native build loads a single GRP, so the component list is one entry and
// setDigest = sha256Hex("<sha256>:<crc>"). Hosts compare setDigest on join and
// deny with {t:"join_deny",reason:"grpmismatch",hostGrp} exactly like
// duke-net.ts _hostHandleJoin.
#include <cstdint>
#include <cstdio>
#include <string>

#include "nn_crypto.hpp" // sha256, toHex, base64Encode/Decode

namespace nn {

struct GrpFp
{
    bool        valid = false;
    uint32_t    crc = 0;
    long        size = 0;
    std::string name;      // file label ("DUKE3D.GRP")
    std::string sha256hex; // lowercase hex
    std::string setDigest; // the join-gating key
    bool        officialPaid = false;
    bool        shareable = false;
};

inline uint32_t crc32Buf(const uint8_t *p, size_t n)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; i++)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// Known base-GRP identities (grptable.ts equivalents we care about).
inline void classifyGrp(GrpFp &fp)
{
    if (fp.crc == 0x983AD923u) // 1.3d shareware
        fp.shareable = true;
    else if (fp.crc == 0xFD3DCFF1u) // 1.5 Atomic
        fp.officialPaid = true;
}

inline const char *grpBasename(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

inline GrpFp grpFingerprintFile(const char *path)
{
    GrpFp fp;
    if (!path || !*path)
        return fp;
    FILE *f = fopen(path, "rb");
    if (!f)
        return fp;
    std::string bytes;
    char buf[1 << 16];
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0)
        bytes.append(buf, got);
    fclose(f);
    if (bytes.empty())
        return fp;

    fp.name      = grpBasename(path);
    fp.size      = (long)bytes.size();
    fp.crc       = crc32Buf((const uint8_t *)bytes.data(), bytes.size());
    fp.sha256hex = toHex(sha256((const uint8_t *)bytes.data(), bytes.size()));
    // Single-component set digest, per grp.ts setFingerprint.
    std::string parts = fp.sha256hex + ":" + std::to_string((unsigned long)fp.crc);
    fp.setDigest      = toHex(sha256(parts));
    classifyGrp(fp);
    fp.valid = true;
    return fp;
}

inline std::string jsonBool(bool b) { return b ? "true" : "false"; }

// Full browser-shaped GrpFingerprint JSON (duke-net.ts / match.ts wire form).
// jsonStrFn = the transport's string escaper (jsonStr).
template <typename JsonStrFn>
inline std::string grpFingerprintJson(const GrpFp &fp, JsonStrFn jsonStrFn)
{
    return std::string("{\"setDigest\":") + jsonStrFn(fp.setDigest) +
           ",\"mainGrp\":{\"crc\":" + std::to_string((unsigned long)fp.crc) +
           ",\"sha256\":" + jsonStrFn(fp.sha256hex) +
           ",\"size\":" + std::to_string(fp.size) + "}" +
           ",\"labels\":[" + jsonStrFn(fp.name.empty() ? std::string("GRP") : fp.name) + "]" +
           ",\"officialPaid\":" + jsonBool(fp.officialPaid) +
           ",\"shareable\":" + jsonBool(fp.shareable) + "}";
}

// base64url (browser invite alphabet) <-> base64
inline std::string b64ToUrl(std::string s)
{
    for (auto &c : s)
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    return s;
}
inline std::string b64FromUrl(std::string s)
{
    for (auto &c : s)
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    return s;
}

} // namespace nn
