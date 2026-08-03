//-------------------------------------------------------------------------
//
// NetDuke32 pluggable transport seam.
//
// This header defines the ONLY boundary between the NetDuke32 netcode
// (oldnet.cpp / net_predict.cpp / sync.cpp) and the underlying wire.
//
//   * The netcode NEVER calls enet/UDP/sockets directly. It calls the
//     net_send / net_broadcast / net_poll functions below.
//   * A concrete transport (the browser WebRTC/EM_JS track, or the no-op
//     stub in net_transport_stub.cpp) implements those functions.
//   * Inbound frames and peer up/down events are delivered back into the
//     netcode by the transport calling Net_ReceiveFrame / Net_PeerEvent.
//
// peerToken == the Duke connectindex (mmulti's "other"), so no separate
// identity table is needed.
//
// This file is compiled only when NETDUKE32 is defined; the stock build is
// unaffected.
//-------------------------------------------------------------------------

#ifndef net_transport_h_
#define net_transport_h_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Logical channels. The transport maps these onto 3 WebRTC data channels:
//   NET_CHAN_MOVE -> duke-move  {ordered:false, maxRetransmits:0}
//   NET_CHAN_REL  -> duke-rel   {ordered:true}
//   NET_CHAN_BULK -> duke-bulk  {ordered:true}   (isolated so a large GRP
//                                                  transfer can't head-of-line
//                                                  block per-tic input)
enum net_channel_t
{
    NET_CHAN_MOVE = 0,  // per-tic input sync   (unreliable / flag 0)
    NET_CHAN_REL  = 1,  // control, vote, name, chat, RTS (reliable)
    NET_CHAN_BULK = 2,  // GRP / large transfers (reliable, isolated)
    NET_CHAN_MAX
};

// Peer event types delivered to Net_PeerEvent().
enum net_peerevent_t
{
    NET_PEER_DOWN = 0,
    NET_PEER_UP   = 1
};

//========================================================================
// Outbound: netcode -> transport   (implemented by the transport / stub)
//========================================================================

// Send `len` bytes to a single peer. `reliable` is 0/1. The transport MUST
// copy the bytes out of `data` synchronously (the wasm heap can move under
// memory growth). peerToken == connectindex.
void net_send(int peerToken, int channel, int reliable, const void *data, int len);

// Send `len` bytes to every connected peer (never loops back to self).
void net_broadcast(int channel, int reliable, const void *data, int len);

// Pump: called by the netcode at its Net_GetPackets drain site. The transport
// should deliver every queued inbound frame now, by invoking Net_ReceiveFrame
// (and Net_PeerEvent for any pending connect/disconnect) before returning.
void net_poll(void);

// Lifecycle hooks (open/close the transport). No-ops for the stub.
void net_transport_init(void);
void net_transport_shutdown(void);

//========================================================================
// Inbound: transport -> netcode   (implemented by the netcode, in oldnet.cpp)
//========================================================================
// These are the C entry points the JS/WebRTC transport calls (mark them
// EMSCRIPTEN_KEEPALIVE on the netcode side).

// Deliver one inbound frame (already reassembled) from `peerToken` on
// `channel`. The netcode feeds it into its packet-parse path.
void Net_ReceiveFrame(int peerToken, int channel, const uint8_t *data, int len);

// Deliver a peer connect (NET_PEER_UP) / disconnect (NET_PEER_DOWN).
void Net_PeerEvent(int peerToken, int eventType);

// [additive] Tell the netcode its own slot (== connectindex) after the join
// handshake. The transport is the sole slot authority (peerToken==connectindex);
// the netcode never assigns connectindex itself. The host is slot 0; each guest
// calls this once with the host-assigned slot. netduke32's oldnet does not
// negotiate connectindex in its own protocol, so this is where it is set.
void Net_SetLocalIndex(int slot);

#ifdef __cplusplus
}
#endif

#endif  // net_transport_h_
