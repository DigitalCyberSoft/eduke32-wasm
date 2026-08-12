// Stage-1 relay end-to-end test CLI (drives the NostrClient over a real relay).
//   selfrt <ws-url> <b64key>            publish an event and receive it back
//   sub    <ws-url> <b64key> <count>    print "RECV <payload>" for each event; exit after count
//   pub    <ws-url> <b64key> <payload>  publish one ephemeral event, then exit
// Kind is SIGNALING_KIND=20079 (ephemeral). Bounded timeouts so it always exits.
// Provide the sjson implementation once, standalone (eduke32 links its own
// sjson.o built with Xmalloc; these tests use a plain allocator). The macro is
// SJSON_IMPLEMENT; #undef it before nn_relay.hpp re-includes sjson.h so the impl
// is emitted exactly once.
#include <cstdint>
#include <cstdlib>
#define SJSON_IMPLEMENT
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define ABORT_IF_F(cond, ...) \
    do { if (cond) abort(); } while (0)
#define sjson_malloc(user, size) (malloc(size))
#define sjson_free(user, ptr) (free(ptr))
#define sjson_realloc(user, ptr, size) (realloc(ptr, size))
#include <sjson.h>
#undef SJSON_IMPLEMENT
#include "nn_relay.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace nn;
using namespace std::chrono_literals;

static const int KIND = 20079;

static bool waitOpen(NostrClient &c, int ms)
{
    for (int t = 0; t < ms; t += 25)
    {
        if (c.openRelayCount() > 0)
            return true;
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

int main(int argc, char **argv)
{
    // The relay transport is now libcurl (nn_ws.hpp), not libdatachannel, so no
    // rtc logger/cleanup is needed here.
    int rc = 2;

    if (argc >= 4 && std::string(argv[1]) == "selfrt")
    {
        std::string url = argv[2], key = argv[3];
        std::string want = "{\"type\":\"presence\",\"from\":\"native\",\"name\":\"Duke\",\"ts\":1712345678000}";
        std::atomic<int> got{ 0 };
        NostrClient client({ url });
        client.subscribeEphemeral(KIND, key, [&](const std::string &p) {
            printf("RECV %s\n", p.c_str());
            fflush(stdout);
            if (p == want)
                got++;
        });
        client.start();
        if (!waitOpen(client, 3000))
            fprintf(stderr, "relay never opened\n");
        std::this_thread::sleep_for(300ms); // let the REQ register before publishing
        client.publishEphemeral(KIND, key, want);
        for (int t = 0; t < 4000 && got.load() == 0; t += 25)
            std::this_thread::sleep_for(25ms);
        rc = got.load() > 0 ? 0 : 1;
        printf(rc == 0 ? "SELFRT OK\n" : "SELFRT FAIL\n");
        client.stop();
    }
    else if (argc >= 5 && std::string(argv[1]) == "sub")
    {
        std::string url = argv[2], key = argv[3];
        int count = atoi(argv[4]);
        std::atomic<int> got{ 0 };
        NostrClient client({ url });
        client.subscribeEphemeral(KIND, key, [&](const std::string &p) {
            printf("RECV %s\n", p.c_str());
            fflush(stdout);
            got++;
        });
        client.start();
        waitOpen(client, 3000);
        printf("READY\n");
        fflush(stdout); // signal the orchestrator that the subscription is live
        for (int t = 0; t < 8000 && got.load() < count; t += 25)
            std::this_thread::sleep_for(25ms);
        rc = got.load() >= count ? 0 : 1;
        client.stop();
    }
    else if (argc >= 5 && std::string(argv[1]) == "pub")
    {
        std::string url = argv[2], key = argv[3], payload = argv[4];
        NostrClient client({ url });
        client.start();
        waitOpen(client, 3000);
        std::this_thread::sleep_for(200ms);
        client.publishEphemeral(KIND, key, payload);
        std::this_thread::sleep_for(600ms); // let the frame flush before closing
        rc = 0;
        client.stop();
    }
    else
    {
        fprintf(stderr, "usage: selfrt|sub|pub ...\n");
    }

    return rc;
}
