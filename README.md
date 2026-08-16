# EDuke32-WASM

Duke Nukem 3D in the browser — a fork of [EDuke32](https://www.eduke32.com/)
that compiles the engine to WebAssembly and adds serverless peer-to-peer
multiplayer, with cross-play between browser players and a matching native
Linux build.

## What it does

- **Runs in the browser.** The full EDuke32 engine built with Emscripten:
  classic Build renderer, sound, music, savegames and settings persisted
  locally (IndexedDB / localStorage). No plugins, no install.
- **Multiplayer without servers.** Matches connect peer-to-peer over WebRTC
  data channels; public Nostr relays carry only the signaling (offers,
  answers, match discovery). There is no game server to run or pay for.
- **Co-op and deathmatch** on a host-authoritative netcode: the host streams
  authoritative world state continuously while guests run a responsive local
  simulation (client-side prediction, free-run input, position reports). Built
  on the NetDuke32 lockstep lineage, evolved into an OpenArena-style
  state-streaming model.
- **Cross-play.** A native Linux build speaks the same WebRTC transport and
  wire protocol, so browser guests can join a natively-hosted match and vice
  versa — including mid-game joins into a running match (late joiners receive
  a state snapshot, catch up, and are seated deterministically).
- **Lobby system.** Host public or private matches, share invite codes or
  `?join=` links, browse the public match list, transfer game data
  peer-to-peer where licensing permits (shareware).
- **Game data stays yours.** GRP files are fingerprinted (CRC-32 + SHA-256),
  stored locally, and never uploaded anywhere except direct peer-to-peer
  transfer between players of the same match.
- **Browser quality-of-life.** Fullscreen with Keyboard Lock (Ctrl+W while
  crouching no longer closes the tab), accidental-close confirmation during
  live matches, a lobby-style wait screen while all players load each level,
  a web settings panel (resolution, display mode, game data management), and
  crash forensics for bug reports.

## Game data

No copyrighted game assets are included. You need a `DUKE3D.GRP`:

- **Shareware**: the freely distributable 1.3d shareware episode works out of
  the box (see `assets/shareware/LICENSE.TXT`).
- **Retail / Atomic Edition**: use the GRP from a copy you own (GOG, Steam,
  original CD). The web build imports it once via the settings panel and keeps
  it in browser storage.

## Building

### Native (Linux)

```sh
make NETNATIVE=1 NETDUKE32=1 USE_LIBVPX=0 RENDERTYPE=SDL SDL_TARGET=2 obj=obj-native -j8
```

Produces `eduke32` with the WebRTC/Nostr transport built in. Run with
`-j /path/to/game/data`.

### WebAssembly

```sh
source /path/to/emsdk/emsdk_env.sh
make PLATFORM=EMSCRIPTEN EM_SINGLE_FILE=0 NETDUKE32=1 obj=obj-em -j8
```

Produces `eduke32.js` / `eduke32.wasm` / `eduke32.data`.

### Web networking bundle

```sh
cd platform/emscripten
npm install
npm run build:net       # bundles net/ into eduke32-net.js
```

Serve `platform/emscripten/index.html` together with the engine artifacts
from any static web server (a secure context — HTTPS or localhost — is
required for WebRTC and the Keyboard Lock API).

## Headless / testing knobs (native)

The native build can host and join matches without a display, driven by
environment variables — useful for automated testing and dedicated-ish hosts:

| Variable | Meaning |
| --- | --- |
| `NN_ROLE` | `host` or `guest` (auto-start without the menu) |
| `NN_KEY` | shared match key |
| `NN_HOSTID` | host identity for a guest to join |
| `NN_PLAYERS` | seats to wait for before launching (default 2) |
| `NN_MAXPLAYERS` | seat cap, decoupled from the launch threshold |
| `NN_PUBLIC` | `1` = announce in the public match list |
| `NN_NAME` | match / player name |
| `NN_GAMETYPE` | `0` deathmatch (default), `1` co-op |
| `NN_LOCALBOT` | `1` = this seat plays itself (test bot) |

## More documentation

- `platform/emscripten/README-netplay.md` — the browser transport (WebRTC
  channels, Nostr signaling, GRP gating/transfer, the JS↔C seam).
- `platform/emscripten/docs/` — integration notes and menu specs.

## Credits & licenses

This project stands on:

- **[EDuke32](https://www.eduke32.com/)** — GPL-2.0 (`source/duke3d/gpl-2.0.txt`)
  plus the BUILD engine license (`source/build/buildlic.txt`).
- **NetDuke32** — the lockstep netcode lineage this fork's netplay grew from.
- **Ken Silverman's BUILD engine** and 3D Realms' Duke Nukem 3D.

Duke Nukem 3D is a trademark of its respective owners. Game assets are not
included and remain the property of their copyright holders.
