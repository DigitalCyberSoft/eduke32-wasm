// Stage-1 Nostr event test CLI.
//   buildevent <b64key> <kind> <plaintext> [dtag] [created_at]
//     -> derive identity from key, AES-GCM-encrypt the plaintext into content,
//        build + schnorr-sign the event, print the full event JSON.
// The node verifier (nostr_check.mjs) asserts nostr-tools verifyEvent() accepts
// it and that the content decrypts back to the plaintext.
#include "nn_nostr.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace nn;

int main(int argc, char **argv)
{
    if (argc >= 5 && std::string(argv[1]) == "buildevent")
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
        {
            fprintf(stderr, "derive failed\n");
            return 1;
        }
        int kind = atoi(argv[3]);
        std::string content = aesGcmEncryptB64(raw, argv[4]);
        Tags tags;
        if (argc >= 6 && argv[5][0])
            tags.push_back({ "d", argv[5] });
        int64_t created = (argc >= 7) ? atoll(argv[6]) : nowSec();
        NostrEvent ev;
        if (!buildSignedEvent(sk, pub, kind, created, tags, content, ev))
        {
            fprintf(stderr, "sign failed\n");
            return 1;
        }
        printf("%s\n", eventToJson(ev).c_str());
        return 0;
    }
    fprintf(stderr, "usage: %s buildevent <b64key> <kind> <plaintext> [dtag] [created_at]\n", argv[0]);
    return 2;
}
