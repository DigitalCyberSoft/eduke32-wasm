// ─────────────────────────────────────────────────────────────────────────────
// SIGNALING — WebRTC offer/answer/ICE + room presence over Nostr ephemeral events.
//
// Forked from the scorchedearth-multi transport; Duke presence carries just a name
// (no tank icon). All payloads are addressed by device id and encrypted under the
// room key, so only room members see them. The Nostr layer carries ONLY the
// handshake; once a data channel opens, game traffic flows peer-to-peer over it.
// ─────────────────────────────────────────────────────────────────────────────
import { SIGNALING_KIND } from "./netconfig";
import { DEVICE_ID } from "./identity";
import { publishEphemeral, subscribeEphemeral } from "./nostr";

type SignalMsg =
  | { type: "offer"; from: string; to: string; sdp?: string; ts: number }
  | { type: "answer"; from: string; to: string; sdp?: string; gen?: string; ts: number }
  | { type: "ice"; from: string; to: string; candidate: string; sdpMid: string | null; sdpMLineIndex: number | null; ts: number }
  | { type: "presence"; from: string; name: string; ts: number };

export interface SignalingHandlers {
  onOffer?: (from: string, sdp: RTCSessionDescriptionInit) => void;
  onAnswer?: (from: string, sdp: RTCSessionDescriptionInit, gen?: string) => void;
  onIce?: (from: string, candidate: RTCIceCandidateInit) => void;
  onPresence?: (from: string, name: string, ts: number) => void;
}

export async function sendOffer(encKey: string, to: string, offer: RTCSessionDescriptionInit, relays: readonly string[]): Promise<void> {
  await publishEphemeral(SIGNALING_KIND, encKey, { type: "offer", from: DEVICE_ID, to, sdp: offer.sdp, ts: Date.now() }, relays);
}

// `gen`: ufrag of the OFFER this answer pairs with. Relays redeliver stale
// signaling for minutes; unpaired, an old session's answer can beat the real
// one to a reconnecting offerer's fresh pc and poison it (wrong DTLS
// fingerprint -> channels never open). Optional field: old peers ignore it.
export async function sendAnswer(encKey: string, to: string, answer: RTCSessionDescriptionInit, relays: readonly string[], gen?: string): Promise<void> {
  await publishEphemeral(SIGNALING_KIND, encKey, { type: "answer", from: DEVICE_ID, to, sdp: answer.sdp, gen, ts: Date.now() }, relays);
}

export async function sendIceCandidate(encKey: string, to: string, c: RTCIceCandidate, relays: readonly string[]): Promise<void> {
  await publishEphemeral(
    SIGNALING_KIND,
    encKey,
    { type: "ice", from: DEVICE_ID, to, candidate: c.candidate, sdpMid: c.sdpMid, sdpMLineIndex: c.sdpMLineIndex, ts: Date.now() },
    relays,
  );
}

export async function sendPresence(encKey: string, name: string, relays: readonly string[]): Promise<void> {
  await publishEphemeral(SIGNALING_KIND, encKey, { type: "presence", from: DEVICE_ID, name, ts: Date.now() }, relays);
}

export async function subscribeSignaling(encKey: string, handlers: SignalingHandlers, relays: readonly string[]): Promise<() => void> {
  return subscribeEphemeral<SignalMsg>(
    SIGNALING_KIND,
    encKey,
    (msg) => {
      if (msg.from === DEVICE_ID) return; // ignore our own echo
      switch (msg.type) {
        case "offer":
          if (msg.to === DEVICE_ID) handlers.onOffer?.(msg.from, { type: "offer", sdp: msg.sdp });
          break;
        case "answer":
          if (msg.to === DEVICE_ID) handlers.onAnswer?.(msg.from, { type: "answer", sdp: msg.sdp }, msg.gen);
          break;
        case "ice":
          if (msg.to === DEVICE_ID)
            handlers.onIce?.(msg.from, { candidate: msg.candidate, sdpMid: msg.sdpMid, sdpMLineIndex: msg.sdpMLineIndex });
          break;
        case "presence":
          handlers.onPresence?.(msg.from, msg.name, msg.ts);
          break;
      }
    },
    relays,
  );
}
