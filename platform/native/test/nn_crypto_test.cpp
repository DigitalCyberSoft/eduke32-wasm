// Stage-1 crypto test CLI. Internal vectors + a cross-check surface the Node
// harness (crypto_check.mjs) drives to prove byte-compatibility with nostr.ts.
//   selftest                      -> run internal known-answer + round-trip tests
//   derive  <b64key>              -> print x-only pubkey hex (== nostr getPublicKey)
//   encrypt <b64key> <plaintext>  -> print base64(iv||ct||tag)
//   decrypt <b64key> <b64blob>    -> print recovered plaintext
#include "nn_crypto.hpp"
#include <cstdio>
#include <string>

using namespace nn;

static int g_fail = 0;
static void check(bool ok, const char *name)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok)
        g_fail++;
}

static int selftest()
{
    // base64 RFC 4648 vectors
    check(base64Encode((const uint8_t *)"", 0) == "", "b64 empty");
    check(base64Encode((const uint8_t *)"f", 1) == "Zg==", "b64 f");
    check(base64Encode((const uint8_t *)"fo", 2) == "Zm8=", "b64 fo");
    check(base64Encode((const uint8_t *)"foo", 3) == "Zm9v", "b64 foo");
    check(base64Encode((const uint8_t *)"foobar", 6) == "Zm9vYmFy", "b64 foobar");
    Bytes rt;
    check(base64Decode("Zm9vYmFy", rt) && std::string(rt.begin(), rt.end()) == "foobar", "b64 decode foobar");

    // sha256 vectors
    check(toHex(sha256(std::string(""))) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256 empty");
    check(toHex(sha256(std::string("abc"))) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256 abc");

    // hex round-trip
    Bytes hb;
    check(fromHex("deadbeef", hb) && toHex(hb) == "deadbeef", "hex roundtrip");

    // AES-GCM round-trip (random IV) + tamper detection
    Bytes key = sha256(std::string("test-key-material")); // any 32 bytes
    std::string pt = "v=0\r\no=- 42 2 IN IP4 127.0.0.1\r\nquote:\"hi\" back\\slash";
    std::string blob = aesGcmEncryptB64(key, pt);
    std::string got;
    check(aesGcmDecryptB64(blob, key, got) && got == pt, "aes-gcm roundtrip (sdp-like)");
    std::string bad = blob;
    bad[bad.size() / 2] = (bad[bad.size() / 2] == 'A') ? 'B' : 'A';
    std::string ignore;
    check(!aesGcmDecryptB64(bad, key, ignore), "aes-gcm rejects tampered");
    Bytes wrongKey = sha256(std::string("other-key"));
    check(!aesGcmDecryptB64(blob, wrongKey, ignore), "aes-gcm rejects wrong key");

    // schnorr sign/verify round-trip
    Bytes rawKey = randomBytes(32);
    Bytes sk;
    std::string pub;
    check(Secp::instance().deriveFromRawKey(rawKey, sk, pub) && pub.size() == 64, "derive keypair");
    Bytes msg = sha256(std::string("event-id-material"));
    Bytes sig;
    check(Secp::instance().sign(sk, msg.data(), sig) && sig.size() == 64, "schnorr sign");
    Bytes pubBytes;
    fromHex(pub, pubBytes);
    check(Secp::instance().verify(sig, msg.data(), pubBytes), "schnorr verify");
    msg[0] ^= 1;
    check(!Secp::instance().verify(sig, msg.data(), pubBytes), "schnorr rejects wrong msg");

    printf(g_fail ? "\nSELFTEST FAIL (%d)\n" : "\nSELFTEST OK\n", g_fail);
    return g_fail ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s selftest|derive|encrypt|decrypt ...\n", argv[0]);
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "selftest")
        return selftest();

    if (cmd == "derive" && argc == 3)
    {
        Bytes raw;
        if (!base64Decode(argv[2], raw) || raw.size() != 32)
        {
            fprintf(stderr, "bad key\n");
            return 2;
        }
        Bytes sk;
        std::string pub;
        if (!Secp::instance().deriveFromRawKey(raw, sk, pub))
            return 1;
        printf("%s\n", pub.c_str());
        return 0;
    }
    if (cmd == "encrypt" && argc == 4)
    {
        Bytes raw;
        base64Decode(argv[2], raw);
        printf("%s\n", aesGcmEncryptB64(raw, argv[3]).c_str());
        return 0;
    }
    if (cmd == "decrypt" && argc == 4)
    {
        Bytes raw;
        base64Decode(argv[2], raw);
        std::string out;
        if (!aesGcmDecryptB64(argv[3], raw, out))
        {
            fprintf(stderr, "decrypt failed\n");
            return 1;
        }
        printf("%s\n", out.c_str());
        return 0;
    }
    fprintf(stderr, "bad args\n");
    return 2;
}
