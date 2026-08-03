# In-engine multiplayer menu — design + C-side hook spec

**Hard constraint:** the lobby / host / join / GRP-selector menus MUST match the Duke
in-engine menu (font, colors, layout) — they live in `source/duke3d/src/menus.cpp`,
which the **netcode agent owns and is actively editing**. This branch therefore does
**not** touch `menus.cpp`. This file specifies (a) the menu screens and (b) the exact
C↔JS hook surface so the C menu is a thin renderer over `window.DukeNet`. The JS side
(data + callbacks) is implemented and ready; the C rendering is built at reconciliation.

All peer-supplied strings (match names, player names, GRP labels) reach C **already
sanitized** by `net/sanitize.ts` (printable ASCII, no `^` palette escape, ≤31 chars), so
the menu draws them as plain `MINIFONT`/`BIGFONT` text with no extra escaping.

## 1. Data flow (thin C menu over JS)

```
   window.DukeNet  ──(push on change, ccall)──▶  C setters  ──▶  cached arrays the
   (lobby/roster/                                (NetMenu_*)      menu draws each frame
    status/progress)
        ▲
        └──(menu action handlers call)────────  DukeNet methods (host/join/filter/…)
```

- **JS → C (data push).** On each `DukeNet` event, JS calls a small exported C setter
  with a JSON string; C parses once and caches a struct array it redraws from (Duke
  menus redraw every frame, so do **not** re-parse per frame). JS wiring is ready as
  `DukeNet.bindInEngineMenu(Module)` (spec §4) and is a no-op until the C setters exist.
- **C → JS (actions).** Menu selections call `window.DukeNet` methods via
  `emscripten_run_script` / a `ccall` bridge (spec §3).

## 2. C setters the netcode menu should export (EMSCRIPTEN_KEEPALIVE)

| C function | Called by JS when | Payload (JSON) |
|---|---|---|
| `void NetMenu_SetStatus(const char* s)` | `onStatus` / `onError` (prefix `!` for error) | status line |
| `void NetMenu_SetLobby(const char* json)` | `onLobby` | `[{matchId,name,players,maxPlayers,ping,grpState}]` where `grpState ∈ {"have","download","paid"}`, `ping` is `-1` for unknown ("?") |
| `void NetMenu_SetRoster(const char* json)` | `onRoster` | `[{name,connected}]` |
| `void NetMenu_SetGrpList(const char* json)` | boot / selector open | `[{id,name,size,shareware,present,isDefault}]` |
| `void NetMenu_SetProgress(int pct, const char* label)` | `onGrpProgress` | 0–100 + label ("Downloading GRP"…) |
| `void NetMenu_OnJoined(int myConnectIndex)` | `onJoined` | close lobby, enter game roster |

These are the only new exports the menu needs; add them to `-sEXPORTED_FUNCTIONS`
(see INTEGRATION.md §3) as `_NetMenu_SetStatus,…`.

## 3. JS methods the menu action handlers call

All already implemented on `window.DukeNet`:

| Menu action | JS call |
|---|---|
| Host a public game | `DukeNet.host({name, isPublic:true, maxPlayers})` |
| Host a private game (invite) | `DukeNet.host({name, isPublic:false, maxPlayers})` → returns `{inviteCode, inviteUrl}` |
| Show invite QR (host, web overlay) | `DukeNet.inviteQr(inviteCode)` → data URL |
| Open the public list | `DukeNet.startLobby()` / `DukeNet.stopLobby()` |
| Join a listed row / invite code | `DukeNet.join(matchIdRow.raw)` or `DukeNet.join(code)` |
| Set the high-ping filter | `DukeNet.setPingFilter(DukeNet.pingPresets()[idx].maxMs)` |
| Toggle GRP sharing (host) | `DukeNet.setAllowGrpDownload(bool)` |
| Set player name | `DukeNet.setPlayerName(str)` |
| Leave | `DukeNet.leave()` |

From C: `emscripten_run_script("window.DukeNet.setPingFilter(150)")`, etc. For a value
back (invite code), use `emscripten_run_script_string(...)` or a `ccall` to a tiny JS
export. `DukeNet.getMyConnectIndex()` returns the local slot after joining.

## 4. JS wiring (ready to enable at reconciliation)

Once the C setters above are exported, this single call (add to `duke-net.ts`, or run
from the boot glue) forwards every `DukeNet` event into the menu — guarded so it is a
no-op until the C side exists:

```ts
function rowForMenu(r) {
  return { matchId: r.matchId, name: r.name, players: r.players, maxPlayers: r.maxPlayers,
           ping: r.ping == null ? -1 : Math.round(r.ping),
           grpState: r.haveGrp ? "have" : r.needsPaidGrp ? "paid" : "download" };
}
DukeNet.on({
  onStatus:  s => Module.ccall("NetMenu_SetStatus","void",["string"],[s]),
  onError:   s => Module.ccall("NetMenu_SetStatus","void",["string"],["!"+s]),
  onLobby:   rows => Module.ccall("NetMenu_SetLobby","void",["string"],[JSON.stringify(rows.map(rowForMenu))]),
  onRoster:  ps => Module.ccall("NetMenu_SetRoster","void",["string"],[JSON.stringify(ps.map(p=>({name:p.name,connected:p.connected})))]),
  onGrpProgress: (f,l) => Module.ccall("NetMenu_SetProgress","void",["number","string"],[Math.round(f*100),l]),
  onJoined:  i => Module.ccall("NetMenu_OnJoined","void",["number"],[i.myConnectIndex]),
});
```

## 5. Screens (match the existing Duke menu style)

1. **MULTIPLAYER** (new root under the main menu): `HOST PUBLIC GAME` · `HOST PRIVATE GAME`
   · `BROWSE PUBLIC GAMES` · `JOIN BY CODE` · `BACK`. Standard vertical `BIGFONT` list.
2. **HOST CONFIG:** match NAME (text entry, `MINIFONT`), MAX PLAYERS (slider/stepper),
   toggle `DON'T ALLOW PEOPLE TO DOWNLOAD MY GRP FILES` (unchecked by default — sharing
   ON), the advertised GRP set shown read-only (from `DukeNet.getLocalGrp().labels`), and
   `START`. Private host then shows the invite code (and a QR on the web overlay).
3. **BROWSE PUBLIC GAMES:** a scrollable list; each row: match name · `players x/y` · ping
   (right-aligned, "?" when unknown) · a GRP tag (`✓` have / `↓` downloadable / `$` needs
   paid GRP, drawn in the menu's accent/disabled colors). A `MAX PING` selector cycles the
   presets. **Rows we HAVE the GRP for sort to the top** (JS already sorts; C draws in
   order). Selecting a `↓` row prompts the download→reload→rejoin flow; a `$` row is
   non-selectable with a "requires a paid GRP" note.
4. **JOIN BY CODE:** a text entry to paste the invite code (or URL) → `DukeNet.join(text)`.
5. **STARTUP GRP SELECTOR** (boot, `DukeNet`/`grp.ts buildSelector`): shown **only** when
   more than one launchable GRP exists (else zero-friction one-click shareware boot). List
   of GRPs: name · size · a `SHAREWARE`/`REGISTERED` tag; the shareware entry is the
   default/highlighted. Selecting one sets `localStorage['eduke32-net-gamegrp']` and boots
   `-gamegrp` on it. If rendered in-engine, use the same menu font/colors; if rendered on
   the web boot page, keep it visually consistent with the loader.

## 6. Notes / open items for reconciliation
- `myconnectindex` bootstrap and the star listen-server relay assumption: see
  INTEGRATION.md §4 — resolve with the netcode agent before wiring §2/§3 live.
- Chat rides `NET_CHAN_REL` in the netcode; sanitize inbound chat with `sanitize.ts`
  before it reaches the menu/HUD text path (same trust boundary as names).
```
