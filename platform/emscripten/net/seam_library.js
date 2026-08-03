// ─────────────────────────────────────────────────────────────────────────────
// seam_library.js — the JS implementation of net_transport.h, linked INTO the wasm.
//
// This is an Emscripten --js-library. It is consumed by `emcc` at LINK time (it
// enters the emcc graph on purpose) and defines the C symbols net_send /
// net_broadcast / net_poll / net_transport_init / net_transport_shutdown. It
// REPLACES net_transport_stub.cpp under the NETDUKE32 flag.
//
// It is deliberately TINY and dependency-free: every real decision lives in the
// bundled transport (window.DukeNet, built from net/*.ts by esbuild into
// eduke32-net.js). This file only marshals bytes across the C<->JS boundary and
// pumps the inbound queue into the netcode.
//
// DO NOT bundle this with esbuild — it is not an ES module; `mergeInto` and the
// bare `_Net_ReceiveFrame` / `_malloc` references are Emscripten link-time magic.
//
// Wiring (spec'd for main to apply in the Makefile — see docs/INTEGRATION.md):
//   * under NETDUKE32+EMSCRIPTEN, drop net_transport_stub.cpp and add
//       -Wl,--js-library=platform/emscripten/net/seam_library.js
//   * export the netcode entrypoints + malloc/free:
//       -sEXPORTED_FUNCTIONS=...,_Net_ReceiveFrame,_Net_PeerEvent,_malloc,_free
// ─────────────────────────────────────────────────────────────────────────────

mergeInto(LibraryManager.library, {
  // C: void net_send(int peerToken, int channel, int reliable, const void *data, int len)
  net_send: function (peerToken, channel, reliable, dataPtr, len) {
    var DN = typeof window !== "undefined" ? window.DukeNet : undefined;
    if (!DN || !DN._seamSend) return; // no wire attached (bundle not loaded): drop
    // Copy synchronously out of the wasm heap (it may move under memory growth).
    var bytes = HEAPU8.slice(dataPtr, dataPtr + len);
    DN._seamSend(peerToken | 0, channel | 0, reliable | 0, bytes);
  },

  // C: void net_broadcast(int channel, int reliable, const void *data, int len)
  net_broadcast: function (channel, reliable, dataPtr, len) {
    var DN = typeof window !== "undefined" ? window.DukeNet : undefined;
    if (!DN || !DN._seamBroadcast) return;
    var bytes = HEAPU8.slice(dataPtr, dataPtr + len);
    DN._seamBroadcast(channel | 0, reliable | 0, bytes);
  },

  // C: void net_poll(void)  — deliver every queued inbound frame + peer event NOW.
  net_poll__deps: ["Net_ReceiveFrame", "Net_PeerEvent", "malloc", "free"],
  net_poll: function () {
    var DN = typeof window !== "undefined" ? window.DukeNet : undefined;
    if (!DN || !DN._seamDrain) return;
    var items = DN._seamDrain();
    for (var i = 0; i < items.length; i++) {
      var it = items[i];
      if (it.kind === 1) {
        // peer event: { peer, event }  (event: 0 down, 1 up)
        _Net_PeerEvent(it.peer | 0, it.event | 0);
      } else {
        // frame: { peer, channel, data:Uint8Array }
        var data = it.data;
        var len = data.length | 0;
        var ptr = _malloc(len || 1);
        if (len) HEAPU8.set(data, ptr);
        _Net_ReceiveFrame(it.peer | 0, it.channel | 0, ptr, len);
        _free(ptr);
      }
    }
  },

  // C: void net_transport_init(void)
  net_transport_init: function () {
    var DN = typeof window !== "undefined" ? window.DukeNet : undefined;
    if (DN && DN._seamInit) DN._seamInit();
  },

  // C: void net_transport_shutdown(void)
  net_transport_shutdown: function () {
    var DN = typeof window !== "undefined" ? window.DukeNet : undefined;
    if (DN && DN._seamShutdown) DN._seamShutdown();
  },
});
