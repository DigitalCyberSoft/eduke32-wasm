// Stage-2 WebRTC peer test: a full native peer (NostrClient + PeerManager +
// signaling) that discovers a peer via presence, connects over WebRTC, and
// exchanges a control message. Two of these on the same relay+key connect to
// each other. Prints CONNECTED / CTRL / PEER OK.  Bounded timeouts.
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
#include "nn_signaling.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

using namespace nn;
using namespace std::chrono_literals;

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <ws-url> <b64key> <name>\n", argv[0]);
        return 2;
    }
    std::string url = argv[1], key = argv[2], name = argv[3];
    rtc::InitLogger(rtc::LogLevel::Error);

    // Random per-process device id (like identity.ts node path).
    std::string deviceId = "native-" + toHex(randomBytes(8));

    NostrClient client({ url });
    rtc::Configuration cfg; // localhost: host candidates suffice, no ICE servers
    PeerManager pm(deviceId, cfg);

    sjson_context *sigCtx = sjson_create_context(0, 0, nullptr);
    std::mutex sigMtx;
    std::atomic<bool> gotPeerCtrl{ false };

    pm.sendSdp = [&](const std::string &to, const std::string &sdp, bool isOffer) {
        client.publishEphemeral(SIGNALING_KIND, key, buildSdpMsg(deviceId, to, sdp, isOffer));
    };
    pm.sendIce = [&](const std::string &to, const std::string &cand, const std::string &mid) {
        client.publishEphemeral(SIGNALING_KIND, key, buildIceMsg(deviceId, to, cand, mid));
    };
    pm.onChannelsReady = [&](const std::string &peer) {
        printf("CONNECTED %s\n", peer.c_str());
        fflush(stdout);
        pm.sendControl(peer, "{\"hello\":\"" + deviceId + "\"}");
    };
    pm.onControl = [&](const std::string &peer, const std::string &ctrl) {
        printf("CTRL from %s: %s\n", peer.c_str(), ctrl.c_str());
        fflush(stdout);
        gotPeerCtrl = true;
    };

    client.subscribeEphemeral(SIGNALING_KIND, key, [&](const std::string &payload) {
        SignalMsg m;
        {
            std::lock_guard<std::mutex> lk(sigMtx);
            if (!parseSignal(sigCtx, payload, m))
                return;
        }
        if (m.from == deviceId)
            return; // own echo
        if (m.type == "presence")
            pm.connect(m.from);
        else if (m.to == deviceId)
        {
            if (m.type == "offer")
                pm.handleOffer(m.from, m.sdp);
            else if (m.type == "answer")
                pm.handleAnswer(m.from, m.sdp);
            else if (m.type == "ice")
                pm.addIce(m.from, m.candidate, m.sdpMid);
        }
    });

    client.start();
    for (int t = 0; t < 3000 && client.openRelayCount() == 0; t += 25)
        std::this_thread::sleep_for(25ms);

    // Presence-driven discovery: announce until connected (or timeout).
    for (int t = 0; t < 20000 && !gotPeerCtrl.load(); t += 250)
    {
        if (t % 800 == 0)
            client.publishEphemeral(SIGNALING_KIND, key, buildPresenceMsg(deviceId, name));
        std::this_thread::sleep_for(250ms);
    }

    int rc = gotPeerCtrl.load() ? 0 : 1;
    printf(rc == 0 ? "PEER OK (%s)\n" : "PEER FAIL (%s)\n", deviceId.c_str());
    fflush(stdout);

    pm.closeAll();
    client.stop();
    sjson_destroy_context(sigCtx);
    rtc::Cleanup().wait();
    return rc;
}
