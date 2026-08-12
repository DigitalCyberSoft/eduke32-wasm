//-------------------------------------------------------------------------
// nn_ws.hpp - Minimal WebSocket (wss://) client over libcurl's WebSocket API
// (OpenSSL backend). This exists because libdatachannel's GnuTLS WebSocket
// backend stalls the client-side upgrade: it receives the relay's 101 response
// promptly but does not drain the already-decrypted TLS record until the *next*
// TCP segment arrives (a keepalive ping ~5s later, or the relay's FIN at its
// ~10s idle timeout). Public relays close first, so the WebSocket never opens
// and native signaling/lobby-announce silently fails ("TLS handshake failed").
// libcurl (OpenSSL) opens the same relays in <1s and stays stable.
//
// One CURL easy handle + one owned thread per connection (easy handles are not
// thread-safe, so every curl call for a connection happens on its own thread).
// Sends from other threads are queued and drained on the connection thread.
// Reconnects with capped backoff -- the relay layer above has none.
//
// Scope: the Nostr *relay* transport only. The actual peer-to-peer game data
// still runs over libdatachannel's PeerConnection/DataChannel (DTLS/SCTP over
// ICE/UDP), a separate code path unaffected by this WSS stall.
//-------------------------------------------------------------------------
#ifndef NN_WS_HPP_
#define NN_WS_HPP_

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#ifndef _WIN32
#include <poll.h>
#endif

namespace nn {

class WsClient
{
public:
    using OnOpen    = std::function<void()>;               // fired on the ws thread each (re)connect
    using OnMessage = std::function<void(const std::string &)>; // complete text message, ws thread
    using OnClose   = std::function<void()>;               // fired on the ws thread on each drop
    using OnError   = std::function<void(const std::string &)>;

    WsClient(std::string url, OnOpen onOpen, OnMessage onMsg, OnClose onClose, OnError onErr)
        : url_(std::move(url)), onOpen_(std::move(onOpen)), onMsg_(std::move(onMsg)),
          onClose_(std::move(onClose)), onErr_(std::move(onErr))
    {}

    ~WsClient() { stop(); }

    // Ensure libcurl's global state is initialized exactly once, before any
    // easy handle or worker thread exists. Callers spin up WsClients from a
    // single control thread, so this is race-free in practice; guarded anyway.
    static void globalInit()
    {
        static std::once_flag once;
        std::call_once(once, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
    }

    void start()
    {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true))
            return; // already started
        th_ = std::thread([this]() { run(); });
    }

    // Signal the worker to stop without waiting. Lets a caller managing many
    // clients unblock them all first, then join -- so a connect-in-progress
    // relay does not serialize the whole shutdown behind its connect timeout.
    void requestStop() { running_.store(false); }

    void stop()
    {
        running_.store(false);
        if (th_.joinable())
            th_.join();
    }

    // Thread-safe: queue a text message. Delivered once the socket is open;
    // messages queued while disconnected are sent on the next successful open.
    void send(const std::string &msg)
    {
        std::lock_guard<std::mutex> lk(sendMtx_);
        if (sendQ_.size() < kMaxQueue)
            sendQ_.push_back(msg);
    }

    bool isOpen() const { return open_.load(); }
    const std::string &url() const { return url_; }

private:
    static constexpr size_t kMaxQueue   = 512;
    static constexpr size_t kMaxMessage = 1u << 20; // 1 MiB reassembly cap

    void run()
    {
        int backoffMs = kBackoffMinMs;
        while (running_.load())
        {
            CURL *c = connectOnce();
            if (!c)
            {
                sleepInterruptible(backoffMs);
                backoffMs = std::min(backoffMs * 2, kBackoffMaxMs);
                continue;
            }
            backoffMs = kBackoffMinMs; // reset on a good connect
            open_.store(true);
            if (onOpen_)
                onOpen_(); // upper layer (re)issues REQ subscriptions -> queued below
            serve(c);
            open_.store(false);
            if (onClose_)
                onClose_();
            curl_easy_cleanup(c);
            if (running_.load())
                sleepInterruptible(backoffMs); // brief pause before reconnecting
        }
    }

    // Progress callback: curl invokes this ~once/second during connect+transfer
    // (it caps its internal poll wait when NOPROGRESS=0). Returning non-zero aborts
    // curl_easy_perform, so a connect in progress bails out within ~1s of stop()
    // instead of blocking the shutdown join for the full connect timeout. Without
    // this, a relay reconnecting at exit hangs the whole process in std::thread::join.
    static int progressCb(void *clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
    {
        return static_cast<WsClient *>(clientp)->running_.load() ? 0 : 1;
    }

    CURL *connectOnce()
    {
        CURL *c = curl_easy_init();
        if (!c)
            return nullptr;
        curl_easy_setopt(c, CURLOPT_URL, url_.c_str());
        curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L); // 2 = WebSocket
        // The relay is an untrusted, E2E-encrypted signaling channel (every event
        // is AES-256-GCM sealed + secp256k1-signed); its TLS certificate
        // authenticates nothing we depend on. Match the browser, which likewise
        // does not pin relay certs.
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L); // no SIGALRM/SIGPIPE in a threaded app
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, (long)(kConnectTimeoutMs / 1000));
        curl_easy_setopt(c, CURLOPT_USERAGENT, "eduke32-native/1.0");
        // Make curl_easy_perform interruptible so stop()/requestStop() can't be
        // blocked behind an in-flight connect (see progressCb).
        curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &WsClient::progressCb);
        curl_easy_setopt(c, CURLOPT_XFERINFODATA, this);

        CURLcode res = curl_easy_perform(c);
        if (res != CURLE_OK)
        {
            if (onErr_ && running_.load()) // don't spam errors while shutting down
                onErr_(curl_easy_strerror(res));
            curl_easy_cleanup(c);
            return nullptr;
        }
        return c;
    }

    // Receive/reassemble frames and flush the send queue until the socket drops
    // or stop() is requested.
    void serve(CURL *c)
    {
        curl_socket_t sock = CURL_SOCKET_BAD;
        curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &sock);

        std::string assembly;
        char buf[16384];

        while (running_.load())
        {
            // Wait for readability (bounded) so stop() stays responsive.
#ifndef _WIN32
            struct pollfd pfd;
            pfd.fd = (int)sock;
            pfd.events = POLLIN;
            pfd.revents = 0;
            poll(&pfd, 1, kPollMs);
#else
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
#endif
            // Drain everything libcurl has buffered (this is the whole point:
            // never leave a decrypted frame sitting unread).
            bool dropped = false;
            for (;;)
            {
                size_t rlen = 0;
                const struct curl_ws_frame *meta = nullptr;
                CURLcode r = curl_ws_recv(c, buf, sizeof(buf), &rlen, &meta);
                if (r == CURLE_AGAIN)
                    break;
                if (r != CURLE_OK)
                {
                    dropped = true;
                    break;
                }
                unsigned flags = meta ? (unsigned)meta->flags : 0u;
                if (flags & CURLWS_CLOSE)
                {
                    dropped = true;
                    break;
                }
                if (flags & (CURLWS_PING | CURLWS_PONG))
                    continue; // libcurl answers pings automatically
                // TEXT/BINARY payload: append, emit when the message is complete.
                if (assembly.size() + rlen <= kMaxMessage)
                    assembly.append(buf, rlen);
                bool complete = meta && meta->bytesleft == 0 && !(flags & CURLWS_CONT);
                if (complete)
                {
                    if (!assembly.empty() && onMsg_)
                        onMsg_(assembly);
                    assembly.clear();
                }
                else if (assembly.size() > kMaxMessage)
                    assembly.clear(); // runaway/oversized: drop and resync
            }
            if (dropped)
                break;

            flushSends(c, dropped);
            if (dropped)
                break;
        }
    }

    void flushSends(CURL *c, bool &dropped)
    {
        std::deque<std::string> q;
        {
            std::lock_guard<std::mutex> lk(sendMtx_);
            q.swap(sendQ_);
        }
        for (auto &m : q)
        {
            if (!wsSendAll(c, m))
            {
                dropped = true;
                // Re-queue the unsent tail so a reconnect can retry it.
                std::lock_guard<std::mutex> lk(sendMtx_);
                sendQ_.push_front(m);
                return;
            }
        }
    }

    // Send a whole text message; loops if libcurl accepts it in pieces. Relay
    // control/EVENT messages are small (a few KB), so the loop rarely iterates.
    static bool wsSendAll(CURL *c, const std::string &m)
    {
        size_t off = 0;
        int spins = 0;
        while (off < m.size())
        {
            size_t sent = 0;
            unsigned flags = (off == 0) ? CURLWS_TEXT : CURLWS_CONT;
            CURLcode r = curl_ws_send(c, m.data() + off, m.size() - off, &sent, 0, flags);
            if (r == CURLE_AGAIN)
            {
                if (++spins > 2000)
                    return false; // ~2s stuck: treat as dropped
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (r != CURLE_OK)
                return false;
            off += sent;
            spins = 0;
        }
        return true;
    }

    void sleepInterruptible(int ms)
    {
        int step = 50;
        for (int e = 0; e < ms && running_.load(); e += step)
            std::this_thread::sleep_for(std::chrono::milliseconds(std::min(step, ms - e)));
    }

    static constexpr int kPollMs           = 200;
    static constexpr int kConnectTimeoutMs = 12000;
    static constexpr int kBackoffMinMs     = 1000;
    static constexpr int kBackoffMaxMs     = 30000;

    std::string url_;
    OnOpen onOpen_;
    OnMessage onMsg_;
    OnClose onClose_;
    OnError onErr_;

    std::atomic<bool> running_{ false };
    std::atomic<bool> open_{ false };
    std::thread th_;
    std::mutex sendMtx_;
    std::deque<std::string> sendQ_;
};

} // namespace nn

#endif // NN_WS_HPP_
