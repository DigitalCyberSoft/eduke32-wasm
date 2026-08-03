# EDuke32-WASM multiplayer transport (the "wire")

Browser-side WebRTC + Nostr multiplayer **transport** for the EDuke32-WASM port. This is
the wire only — discovery, connection, GRP gating/transfer, lobby, ping, and the JS side
of the `net_transport.h` seam. The **netcode** (netduke32) lives in the C engine on the
`netcode-port` branch; the two meet at the frozen seam.

## Layout

```
platform/emscripten/
  net/
    netconfig.ts identity.ts nostr.ts signaling.ts   # reused/forked scorchedearth stack
    peer.ts        # Duke fork: binary framing, 3 data channels, star topology, phase gate
    match.ts       # fork: MatchInfo(+GRP fingerprint/name/ping), star, public discovery
    grptable.ts    # transcribed engine internalgrpfiles[]  (classification authority)
    grp.ts         # CRC-32 + SHA-256 fingerprint, gating, transfer (hash-before-use), selector
    sanitize.ts    # inbound-string trust boundary (printable ASCII, strip '^', ≤31)
    ping.ts lobby.ts idb.ts
    seam.ts        # runtime seam: token<->device map, inbound queue, send/broadcast
    duke-net.ts    # window.DukeNet facade + JS seam surface  (esbuild ENTRY POINT)
    seam_library.js# emcc --js-library: the C ABI of net_transport.h (NOT esbuild-bundled)
  test/            # vitest standalone unit tests + harness.html (browser 2-peer test)
  docs/            # INTEGRATION.md, INENGINE_MENU_SPEC.md, ci-pages.patch
  grp-manifest.json# bundleable free-GRP descriptors (shareware only, per research)
  eduke32-net.js   # BUILT bundle (committed), loaded by index.html beside eduke32.js
  index.html       # wasm boot page + a minimal net <script> and pending-GRP boot hook
  package.json esbuild.config.mjs tsconfig.json vitest.config.ts
```

## Build / test

```bash
npm ci
npm run build:net   # -> eduke32-net.js (self-contained: nostr-tools + qrcode + jsqr inlined)
npm test            # 52 standalone unit tests (Node)
npm run typecheck
```

## Ownership boundary
- **This branch owns** everything under `platform/emscripten/` above.
- It does **not** edit any `source/duke3d/src/*` (netcode agent owns `net_transport.h`,
  `net_transport_stub.cpp`, `menus.cpp`, `network.cpp`, `oldnet*`, `game.cpp`) nor the
  `NETDUKE32` flags in `Common.mak` / `GNUmakefile`. C-side changes are specified in
  `docs/INTEGRATION.md` (seam/Makefile) and `docs/INENGINE_MENU_SPEC.md` (menu hooks) for
  `main` to apply at reconciliation.

See `docs/INTEGRATION.md` for the frozen-seam wiring, the peerToken/connectindex +
star-listen-server semantics, and what is verified standalone vs. pending the merge.
