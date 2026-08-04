// ─────────────────────────────────────────────────────────────────────────────
// NET CONFIG — Duke relays, Nostr kinds, STUN/TURN, data channels, lobby key.
//
// Forked from the scorchedearth-multi transport. Peers talk over WebRTC data
// channels; the SDP offer/answer + ICE are signaled through Nostr ephemeral events
// on public relays. There is NO game server. A "room" is a shared 32-byte AES key
// that both encrypts every event's content AND (via SHA-256) derives a secp256k1
// Nostr keypair, so everyone holding the key publishes/subscribes as the same
// identity.
//   - private match  -> a freshly generated room key, shared out-of-band (invite/QR)
//   - public  match  -> announced on the FIXED PUBLIC_LOBBY_KEY channel below
//
// Duke differences vs the source stack:
//   - THREE data channels per peer (move/rel/bulk), binary framing (see peer.ts).
//   - STAR topology (guests connect only to the host; see match.ts).
//   - A Duke-specific PUBLIC_LOBBY_KEY so the Duke lobby is its own channel.
// ─────────────────────────────────────────────────────────────────────────────

export const PROTOCOL_VERSION = 1;

/** Public Nostr relays used as the signaling rendezvous. Same proven set as the
 *  source stack: multiple relays so a single outage does not block discovery. */
export const NOSTR_RELAYS: readonly string[] = [
  // Loopback relay FIRST: `npm run relay` (or any NIP-01 relay) on the player's own
  // machine makes desktop<->browser signaling fully local — no third-party relay in
  // the path. Browsers exempt loopback from mixed-content blocking, so an https page
  // may open ws://127.0.0.1; when nothing listens it fails in milliseconds and the
  // public relays below take over.
  "ws://127.0.0.1:7500",
  "wss://relay.damus.io",
  "wss://nos.lol",
  "wss://relay.snort.social",
  "wss://relay.primal.net",
  "wss://nostr.mom",
  "wss://nostr.einundzwanzig.space",
  "wss://yabu.me",
  "wss://nostr.oxtr.dev",
  "wss://relay.mostr.pub",
  "wss://soloco.nl",
  "wss://nostr.data.haus",
  "wss://relay.nostr.net",
  "wss://relay.noswhere.com",
];

/** Public STUN (zero-config). Multiple servers + Cloudflare's so a single outage
 *  does not block gathering; STUN reflexive candidates traverse every NAT pair
 *  EXCEPT two symmetric NATs, which physically require a TURN relay. */
export const STUN_SERVERS: RTCIceServer[] = [
  {
    urls: [
      "stun:stun.l.google.com:19302",
      "stun:stun1.l.google.com:19302",
      "stun:stun2.l.google.com:19302",
      "stun:stun.cloudflare.com:3478",
    ],
  },
];

/** TURN is OPT-IN. A pair of *symmetric* NATs cannot be hole-punched with STUN
 *  alone; only a TURN relay bridges them. There is no reliable zero-signup public
 *  TURN. To make connectivity bulletproof, paste free-tier creds here (Metered
 *  ~50 GB/mo or Cloudflare ~1 TB/mo) and rebuild, OR inject them at runtime with
 *  `?turn=<base64 of a JSON RTCIceServer or RTCIceServer[]>` (no rebuild). Empty =>
 *  STUN-only, which already connects every NON-symmetric pair. */
export const TURN_SERVERS: RTCIceServer[] = [
  // Example — replace with YOUR free-tier TURN creds, then `npm run build:net`:
  // {
  //   urls: ["turn:relay.example.com:80", "turn:relay.example.com:80?transport=tcp",
  //          "turns:relay.example.com:443?transport=tcp"],
  //   username: "USERNAME", credential: "CREDENTIAL",
  // },
];

/** Runtime TURN override: ?turn=<base64(JSON)> where JSON is an RTCIceServer or an
 *  array of them. Lets a deployment (or a test harness) add TURN without rebuild. */
function _turnOverride(): RTCIceServer[] {
  try {
    const params = new URLSearchParams(typeof location !== "undefined" ? location.search : "");
    const raw = params.get("turn");
    if (!raw) return [];
    const parsed = JSON.parse(atob(raw)) as RTCIceServer | RTCIceServer[];
    return Array.isArray(parsed) ? parsed : [parsed];
  } catch {
    return []; // malformed override: fall back to STUN + any compiled-in TURN
  }
}

/** The live ICE config: public STUN + any compiled-in TURN + any ?turn= override.
 *  Read at each RTCPeerConnection so a runtime override applies. */
export function rtcConfig(): RTCConfiguration {
  return { iceServers: [...STUN_SERVERS, ...TURN_SERVERS, ..._turnOverride()] };
}

// Nostr event kinds.
export const SIGNALING_KIND = 20079; // ephemeral: WebRTC signaling + room presence
export const LOBBY_KIND = 30078; // replaceable (NIP-78): public match announcements

// ── WebRTC data channels (Duke: three logical netcode channels) ──────────────
//
// These map 1:1 onto the frozen net_transport.h channel enum:
//   NET_CHAN_MOVE (0) -> "duke-move"  {ordered:false, maxRetransmits:0}  (per-tic input)
//   NET_CHAN_REL  (1) -> "duke-rel"   {ordered:true}                     (control/vote/chat)
//   NET_CHAN_BULK (2) -> "duke-bulk"  {ordered:true}                     (GRP / large xfer)
//
// The transport also drives its OWN pre-game protocol (join handshake, name, RTT
// ping, GRP transfer) over these SAME channels while a connection is NOT yet
// "attached" to the netcode — strings (JSON) on duke-rel, binary chunks on
// duke-bulk. Once a peer attaches (join complete), duke-move/rel/bulk carry ONLY
// raw netcode frames. See peer.ts for the phase model.
export const DC_LABELS = ["duke-move", "duke-rel", "duke-bulk"] as const;
export type DcLabel = (typeof DC_LABELS)[number];

/** channel index (net_channel_t) -> data channel label. */
export const CHANNEL_TO_LABEL: readonly DcLabel[] = ["duke-move", "duke-rel", "duke-bulk"];

/** RTCDataChannelInit per label. duke-move is fully unreliable/unordered so a lost
 *  input tic is never retransmitted (stale by the time it arrives); duke-rel and
 *  duke-bulk are reliable+ordered. duke-bulk is a SEPARATE channel so a large GRP
 *  transfer cannot head-of-line-block per-tic input. */
export const DC_INIT: Record<DcLabel, RTCDataChannelInit> = {
  "duke-move": { ordered: false, maxRetransmits: 0 },
  "duke-rel": { ordered: true },
  "duke-bulk": { ordered: true },
};

// Cadences (ms).
export const PRESENCE_INTERVAL_MS = 5_000; // Nostr room heartbeat (who is here)
export const ANNOUNCE_INTERVAL_MS = 15_000; // public-match announcement refresh
export const ANNOUNCE_TTL_MS = 60_000; // a public match silent longer than this is stale

/** FIXED shared key for the PUBLIC Duke lobby channel. Anyone running the app
 *  derives the same Nostr identity from it -> a single public room everyone can
 *  read/write. (Generated once with crypto.randomBytes(32); rotating it partitions
 *  the public lobby, so treat it as a protocol constant.) Distinct from any other
 *  app's lobby key so the Duke list is its own channel. */
export const PUBLIC_LOBBY_KEY = "MqBAIxP3Lwawq+18BL1KSjAdlTxfVtoERfmgszaEKnc=";

// Relay override: ?relays=ws://a,ws://b points the app at a private/local relay
// instead of the public set. activeRelays() is read at every publish/subscribe.
let _relayOverride: readonly string[] | null = null;
export function setRelayOverride(urls: readonly string[] | null): void {
  _relayOverride = urls && urls.length > 0 ? urls : null;
}
function _relayParam(): readonly string[] | null {
  try {
    const params = new URLSearchParams(typeof location !== "undefined" ? location.search : "");
    const raw = params.get("relays");
    if (!raw) return null;
    const urls = raw.split(",").map((s) => s.trim()).filter(Boolean);
    return urls.length ? urls : null;
  } catch {
    return null;
  }
}
export function activeRelays(): readonly string[] {
  return _relayOverride ?? _relayParam() ?? NOSTR_RELAYS;
}
