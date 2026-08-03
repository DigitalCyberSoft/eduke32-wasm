# EDuke32-WASM multiplayer — transport integration spec

This document is the reconciliation guide for wiring the browser **transport** (this
branch, the "wire" side) to the **netcode** (`netcode-port`, netduke32) at merge time.
It is written for `main`. Nothing here edits a netcode-owned file; the C-side changes
are specified as diffs for `main`/the netcode agent to apply.

## 1. What this branch ships (all under `platform/emscripten/`)

Two independent JS artifacts:

| Artifact | Built by | Loaded / linked | Role |
|---|---|---|---|
| `eduke32-net.js` | esbuild (`npm run build:net`) | `index.html` `<script type=module>` at **runtime** | `window.DukeNet`: WebRTC+Nostr transport, discovery, GRP gating/transfer, lobby, ping. Bundles `nostr-tools` + `qrcode` + `jsqr`. **Never enters the emcc graph.** |
| `net/seam_library.js` | consumed by `emcc` (`--js-library`) at **link** | linked **into** the wasm | The C ABI of `net_transport.h` (`net_send`/`net_broadcast`/`net_poll`/`net_transport_init`/`net_transport_shutdown`). Tiny marshaller that forwards to `window.DukeNet`. **Replaces `net_transport_stub.cpp`.** |

Source (`net/*.ts`): `netconfig` `identity` `nostr` `signaling` (reused/forked from the
proven scorchedearth-multi stack) · `peer` (Duke fork: binary framing, 3 channels, star)
· `match` (fork: GRP fingerprint + name + ping in `MatchInfo`, star, discovery) · `grptable`
(transcribed `internalgrpfiles[]`) · `grp` (fingerprint/gating/transfer/selector) · `sanitize`
· `ping` · `lobby` · `idb` · `seam` · `duke-net` (the `window.DukeNet` facade + seam JS).

## 2. The frozen seam (implemented exactly as given)

`net_transport.h` is authoritative and was **not** modified. Outbound (C→JS) is
implemented by `seam_library.js`; inbound (JS→C) is the netcode's
`Net_ReceiveFrame(peerToken, channel, data, len)` / `Net_PeerEvent(peerToken, up|down)`.

- Channels map to data channels: `NET_CHAN_MOVE→duke-move {ordered:false,maxRetransmits:0}`,
  `NET_CHAN_REL→duke-rel {ordered:true}`, `NET_CHAN_BULK→duke-bulk {ordered:true}`.
- `net_send` copies bytes out of the wasm heap **synchronously** (`HEAPU8.slice`) before
  the async wire send, as the header requires.
- `net_poll` drains one FIFO of inbound frames **and** peer up/down events **in arrival
  order**, calling `Net_ReceiveFrame` / `Net_PeerEvent` for each — so a `NET_PEER_UP`
  always precedes that peer's first frame and `NET_PEER_DOWN` follows its last.

## 3. Makefile wiring — SPEC for `main` (do NOT let this branch edit the makefiles)

The netcode-port already gates the netcode with `NETDUKE32` (GNUmakefile ~L563). When
`NETDUKE32=1`, `net_transport_stub.cpp` is compiled in. To use the **real** browser
backend on the Emscripten build, `main` should:

**(a) GNUmakefile — drop the stub when building the browser backend.** Just after the
existing `ifneq (1,$(NETDUKE32)) … else … duke3d_excl += network.cpp … endif` block:

```make
# Browser transport backend replaces the no-op stub under NetDuke32 + Emscripten.
ifeq (1,$(NETDUKE32))
    ifeq ($(PLATFORM),EMSCRIPTEN)
        duke3d_excl += net_transport_stub.cpp
    endif
endif
```

**(b) Common.mak — link the js-library and export the entrypoints.** Inside the
existing `ifeq ($(PLATFORM),EMSCRIPTEN)` linker block (Common.mak ~L987, where
`-sEXPORTED_RUNTIME_METHODS=FS,callMain,ccall,cwrap` already lives):

```make
ifeq (1,$(NETDUKE32))
    # The seam's outbound C ABI (net_send/…): implemented in JS, linked in here.
    LINKERFLAGS += --js-library platform/emscripten/net/seam_library.js
    # Keep + export the netcode's inbound entrypoints and the allocator the seam
    # uses to hand frames to C. (Net_ReceiveFrame/Net_PeerEvent are also reachable
    # via EMSCRIPTEN_KEEPALIVE on the netcode side; listing them is belt-and-braces.)
    LINKERFLAGS += -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_Net_ReceiveFrame,_Net_PeerEvent
endif
```

Notes for `main`:
- If another `-sEXPORTED_FUNCTIONS` is already set, **merge** the symbol lists (a second
  flag overrides rather than appends).
- `seam_library.js` also declares `net_poll__deps: ['Net_ReceiveFrame','Net_PeerEvent','malloc','free']`,
  so the linker keeps them even without the explicit export list.
- No change is needed to `EXPORTED_RUNTIME_METHODS` (`ccall`/`cwrap` already present; the
  seam calls `_Net_ReceiveFrame` directly).

## 4. peerToken == connectindex, and the STAR listen-server model — READ THIS

`peerToken` is the Duke `connectindex`. The transport is a **star listen server**: guests
connect **only to the host**; the host connects to every guest; there are no guest↔guest
links. The host **also plays**. Consequences the netcode side must agree with:

- **Slot assignment is host-authoritative.** The host is `connectindex 0`. When a guest
  passes the GRP gate (its `MatchInfo.grp.setDigest` equals the host's), the host allocates
  the lowest free slot `k∈[1..maxPlayers)`, then fires `Net_PeerEvent(k, UP)` locally and
  tells the guest `{yourSlot:k, hostSlot:0}` over the reliable channel. The guest fires
  `Net_PeerEvent(0, UP)` for the host. Both sides now use the agreed slot as the peerToken
  for that connection.
- **A guest's netcode sees exactly ONE peer: the host (token 0).** In a star, `net_broadcast`
  from a guest reaches only the host; `net_send(token)` from a guest can only address the
  host. The host relays as needed. **This is the load-bearing assumption to confirm with the
  netcode agent.** If netduke32 needs to address other guests' connectindexes directly, it
  must route through the host (listen-server relay) at the netcode layer — the transport
  intentionally does not silently relay, because relaying would require tagging the otherwise
  raw netcode frames.
- **`myconnectindex` bootstrap.** The frozen header has no "set my index" call, so the guest
  learns its own slot from the join handshake (`DukeNet.getMyConnectIndex()` and the
  `onJoined` event). At reconciliation, decide one of: (i) netduke32's own hello over
  `NET_CHAN_REL` carries the index (no header change), or (ii) add a tiny
  `Net_SetLocalIndex(int)` export the seam calls. **This is a question flagged for main; the
  header was NOT changed here.**

## 5. Per-peer phase model (why the 3 channels stay byte-pure for the netcode)

Each connection is `attached` = false until the join handshake completes. Before attach the
connection carries the transport's own protocol — JSON **strings** on `duke-rel` (join,
name, RTT ping/pong, GRP-transfer control) and binary **GRP chunks** on `duke-bulk`. After
attach, all three channels carry **only raw netcode frames** (always binary), delivered to
`Net_ReceiveFrame`. Because a joiner who lacks the GRP **downloads → reloads → rejoins**, the
lobby phase and the game phase never overlap on a channel. The `attached` flag is per-peer,
so a host can serve a GRP to a new joiner (pre-attach) while playing with others (attached).

## 6. GRP gating, firewall, and transfer

- **Gating:** only identical-GRP-set players may join. `MatchInfo.grp` is a `setDigest`
  (SHA-256 over the ordered main-GRP + mods/CON/DEF component hashes) plus the main GRP's
  `{crc, sha256, size}`, labels, and `shareable`/`officialPaid` flags. The host rejects a
  join whose `setDigest` differs.
- **Firewall (sender-side, `grptable.ts`):** only `GAMEFLAG_SHAREWARE` CRCs are ever
  transferable. Every retail base, every `GAMEFLAG_ADDON`, the 0.99 beta, and any unknown
  CRC are blocked — **keyed on CRC, not size** (Atomic `0xFD3DCFF1` and Atomic-WT
  `0x982AFE4A` are both 44356548 bytes and both blocked). The host serves **only its single
  in-play GRP**, only if shareable **and** the host has not toggled
  "Don't allow people to download my GRP files" (default: sharing ON). The paid firewall
  overrides the toggle.
- **Transfer:** chunked over `duke-bulk` (16 KB chunks, backpressured). **HASH-BEFORE-USE**:
  the receiver reassembles, then the bytes must match the advertised `crc` **and** `sha256`
  before anything is written. On success → persist to IndexedDB (keyed by CRC) → set the
  persistent boot choice `localStorage['eduke32-net-gamegrp']={crc,filename}` → reload. On
  boot, `index.html`'s `applyNetGrpChoice()` writes the stored GRP into `Module.FS` and
  appends `-gamegrp <file>` (strict no-op when no choice is set). The transient
  `sessionStorage['eduke32-net-rejoin']` holds the match to auto-rejoin this session
  (`DukeNet.consumePendingJoin()`), to be wired to the in-engine menu at reconciliation.

## 7. Build & CI

```bash
cd platform/emscripten
npm ci
npm run build:net        # -> platform/emscripten/eduke32-net.js  (committed)
npm test                 # 52 standalone unit tests (Node/vitest)
npm run typecheck        # tsc --noEmit
```

The Pages workflow must (1) run `npm ci && npm run build:net` and (2) copy
`eduke32-net.js` **and** `grp-manifest.json` into `_site` beside `eduke32.js`. Because
`.github/workflows/pages.yml` is shared, the change is delivered as
`docs/ci-pages.patch` (see below) for `main` to apply, not committed to the shared file
from this branch.

## 8. Verified standalone vs. pending the netcode merge

**Verified now (no C engine, `npm test`, 52 tests):**
- CRC-32 equals the engine's `Bcrc32` (canonical `123456789` → `0xCBF43926`; incremental == whole).
- GRP classification: shareware = shareable; Atomic + Atomic-WT (same size) both blocked by CRC; add-ons/beta/unknown blocked; the blocklist+shareware set partition the whole table.
- GRP fingerprint: chunked FS hash == one-shot; set digest order/content-sensitive; labels sanitized.
- Transfer: firewall blocks paid/opt-out/allows shareware; receiver yields bytes **only** on crc+sha match; tampered/oversized/duplicate chunks fail closed.
- Sanitizer: `^`, control bytes, non-ASCII, overlong all stripped; idempotent.
- Seam: token↔device mapping, ordered drain, frame-byte copy semantics, send/broadcast routing, self never targeted.
- Ping filter: unknown always included; presets; relay-RTT estimate; EWMA tracker. Lobby: have-GRP to top, paid/downloadable marking, filter excludes slow but keeps "?".

**Verified in a browser (manual, `test/harness.html`), NOT in Node CI:**
- 2-peer connect over the default public Nostr relays: Nostr signaling → 3 WebRTC data
  channels → GRP-gated join handshake → seam frame round-trip (`net_broadcast`→`net_poll`).
  (WebRTC/Web-Crypto/IndexedDB are browser APIs; Node has no `RTCPeerConnection`.)

**Pending the merge (cannot be verified until netduke32 is flag-on and linked):**
- End-to-end C↔JS: `emcc` linking `seam_library.js`, `net_send`→wire→`Net_ReceiveFrame`,
  peer up/down at real connectindexes, and the listen-server relay/`myconnectindex`
  question in §4. The JS is written to the frozen header; full integration is a merge task.
```
