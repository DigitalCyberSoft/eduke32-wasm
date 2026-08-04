//-------------------------------------------------------------------------
// nn_crypto.hpp - crypto primitives for the native transport, byte-compatible
// with the browser transport (platform/emscripten/net/nostr.ts + identity.ts).
//
// Header-only (inline) so the same code compiles into both eduke32
// (net_transport_native.cpp) and the standalone Stage-1 tests.
//
// Matches nostr.ts exactly:
//   * room key = base64 of 32 raw bytes (AES-256 key)
//   * content encryption = AES-256-GCM, 12-byte random IV, NO additional data;
//     wire = base64( iv[12] || ciphertext || tag[16] )   (WebCrypto layout)
//   * Nostr identity = secp256k1 secret key sha256(rawKey) -> BIP-340 x-only
//     pubkey; events signed with schnorr over the NIP-01 event id.
//
// Deps: OpenSSL libcrypto (SHA-256, AES-256-GCM, RAND) + libsecp256k1 (schnorr).
//-------------------------------------------------------------------------
#ifndef NN_CRYPTO_HPP_
#define NN_CRYPTO_HPP_

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <secp256k1_schnorrsig.h>

namespace nn {

using Bytes = std::vector<uint8_t>;

// ── hex ──────────────────────────────────────────────────────────────────────
inline std::string toHex(const uint8_t *p, size_t n)
{
    static const char *H = "0123456789abcdef";
    std::string s;
    s.resize(n * 2);
    for (size_t i = 0; i < n; i++)
    {
        s[i * 2]     = H[p[i] >> 4];
        s[i * 2 + 1] = H[p[i] & 0xF];
    }
    return s;
}
inline std::string toHex(const Bytes &b) { return toHex(b.data(), b.size()); }

inline bool fromHex(const std::string &s, Bytes &out)
{
    if (s.size() & 1)
        return false;
    out.clear();
    out.reserve(s.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2)
    {
        int hi = nib(s[i]), lo = nib(s[i + 1]);
        if (hi < 0 || lo < 0)
            return false;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

// ── base64 (standard alphabet, '=' padding, no line breaks; matches btoa) ─────
inline std::string base64Encode(const uint8_t *data, size_t len)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3)
    {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back(T[n & 63]);
    }
    if (len - i == 1)
    {
        uint32_t n = data[i] << 16;
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    }
    else if (len - i == 2)
    {
        uint32_t n = (data[i] << 16) | (data[i + 1] << 8);
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}
inline std::string base64Encode(const Bytes &b) { return base64Encode(b.data(), b.size()); }

inline bool base64Decode(const std::string &s, Bytes &out)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    uint32_t buf = 0;
    int bits = 0;
    for (char c : s)
    {
        if (c == '=' || c == '\n' || c == '\r')
            continue;
        int v = val(c);
        if (v < 0)
            return false;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back((uint8_t)((buf >> bits) & 0xFF));
        }
    }
    return true;
}

// ── SHA-256 ──────────────────────────────────────────────────────────────────
inline Bytes sha256(const uint8_t *data, size_t len)
{
    Bytes out(32);
    SHA256(data, len, out.data());
    return out;
}
inline Bytes sha256(const std::string &s) { return sha256((const uint8_t *)s.data(), s.size()); }
inline Bytes sha256(const Bytes &b) { return sha256(b.data(), b.size()); }

// ── randomness ───────────────────────────────────────────────────────────────
inline Bytes randomBytes(size_t n)
{
    Bytes b(n);
    if (RAND_bytes(b.data(), (int)n) != 1)
        throw std::runtime_error("RAND_bytes failed");
    return b;
}

// ── AES-256-GCM (WebCrypto-compatible: 12-byte IV, 16-byte tag, no AAD) ───────
//
// Encrypt returns base64( iv || ciphertext || tag ), exactly like nostr.ts
// encrypt(): `out = iv(12) || subtle.encrypt(...)` where subtle's output is
// ciphertext||tag. Decrypt reverses and verifies the tag (returns false on any
// mismatch, matching the TS `catch { return null }`).
inline std::string aesGcmEncryptB64(const Bytes &key32, const std::string &plaintext)
{
    Bytes iv = randomBytes(12);
    Bytes ct(plaintext.size());
    Bytes tag(16);

    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        throw std::runtime_error("EVP_CIPHER_CTX_new");
    int len = 0, ok = 1;
    ok &= EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    ok &= EVP_EncryptInit_ex(ctx, nullptr, nullptr, key32.data(), iv.data());
    ok &= EVP_EncryptUpdate(ctx, ct.data(), &len, (const uint8_t *)plaintext.data(), (int)plaintext.size());
    int ctLen = len;
    ok &= EVP_EncryptFinal_ex(ctx, ct.data() + ctLen, &len); // GCM: no extra bytes
    ctLen += len;
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data());
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1)
        throw std::runtime_error("aes-gcm encrypt failed");

    Bytes out;
    out.reserve(12 + ctLen + 16);
    out.insert(out.end(), iv.begin(), iv.end());
    out.insert(out.end(), ct.begin(), ct.begin() + ctLen);
    out.insert(out.end(), tag.begin(), tag.end());
    return base64Encode(out);
}

// Returns true + plaintext on success; false on wrong key / tampered / malformed.
inline bool aesGcmDecryptB64(const std::string &b64, const Bytes &key32, std::string &plaintext)
{
    Bytes all;
    if (!base64Decode(b64, all) || all.size() < 12 + 16)
        return false;
    const uint8_t *iv  = all.data();
    const uint8_t *ct  = all.data() + 12;
    size_t ctLen       = all.size() - 12 - 16;
    const uint8_t *tag = all.data() + all.size() - 16;

    Bytes pt(ctLen);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return false;
    int len = 0, ok = 1;
    ok &= EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
    ok &= EVP_DecryptInit_ex(ctx, nullptr, nullptr, key32.data(), iv);
    ok &= EVP_DecryptUpdate(ctx, pt.data(), &len, ct, (int)ctLen);
    int ptLen = len;
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void *)tag);
    int fin = EVP_DecryptFinal_ex(ctx, pt.data() + ptLen, &len); // verifies tag
    EVP_CIPHER_CTX_free(ctx);
    if (ok != 1 || fin != 1)
        return false;
    ptLen += len;
    plaintext.assign((const char *)pt.data(), ptLen);
    return true;
}

// ── secp256k1 / BIP-340 schnorr (Nostr identity + signatures) ─────────────────
//
// One process-wide context (thread-safe for sign/verify once created). The room
// key -> identity derivation and signing match nostr-tools finalizeEvent().
class Secp
{
public:
    static Secp &instance()
    {
        static Secp s;
        return s;
    }

    // sk = sha256(rawKey); returns lowercase-hex x-only pubkey (the Nostr pubkey).
    // `secOut` receives the 32-byte secret key for later signing.
    bool deriveFromRawKey(const Bytes &rawKey32, Bytes &secOut, std::string &pubHex)
    {
        Bytes sk = sha256(rawKey32);
        if (secp256k1_ec_seckey_verify(ctx_, sk.data()) != 1)
            return false;
        secp256k1_keypair kp;
        if (secp256k1_keypair_create(ctx_, &kp, sk.data()) != 1)
            return false;
        secp256k1_xonly_pubkey xonly;
        if (secp256k1_keypair_xonly_pub(ctx_, &xonly, nullptr, &kp) != 1)
            return false;
        uint8_t pub[32];
        if (secp256k1_xonly_pubkey_serialize(ctx_, pub, &xonly) != 1)
            return false;
        secOut = sk;
        pubHex = toHex(pub, 32);
        return true;
    }

    // Schnorr-sign a 32-byte message (the event id) with sk -> 64-byte sig.
    bool sign(const Bytes &sk32, const uint8_t msg32[32], Bytes &sig64)
    {
        secp256k1_keypair kp;
        if (secp256k1_keypair_create(ctx_, &kp, sk32.data()) != 1)
            return false;
        Bytes aux = randomBytes(32); // BIP-340 aux randomness; any value yields a valid sig
        sig64.resize(64);
        return secp256k1_schnorrsig_sign32(ctx_, sig64.data(), msg32, &kp, aux.data()) == 1;
    }

    // Verify a 64-byte schnorr sig of msg32 under a 32-byte x-only pubkey.
    bool verify(const Bytes &sig64, const uint8_t msg32[32], const Bytes &pub32)
    {
        secp256k1_xonly_pubkey xonly;
        if (secp256k1_xonly_pubkey_parse(ctx_, &xonly, pub32.data()) != 1)
            return false;
        return secp256k1_schnorrsig_verify(ctx_, sig64.data(), msg32, 32, &xonly) == 1;
    }

private:
    Secp() : ctx_(secp256k1_context_create(SECP256K1_CONTEXT_NONE))
    {
        // Randomize against side-channels (best-effort; non-fatal if it fails).
        Bytes seed = randomBytes(32);
        if (secp256k1_context_randomize(ctx_, seed.data()) != 1)
        {
            /* hardening only; signing/verification still correct without it */
        }
    }
    ~Secp() { secp256k1_context_destroy(ctx_); }
    Secp(const Secp &) = delete;
    Secp &operator=(const Secp &) = delete;
    secp256k1_context *ctx_;
};

} // namespace nn

#endif // NN_CRYPTO_HPP_
