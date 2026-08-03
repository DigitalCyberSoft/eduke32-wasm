//-------------------------------------------------------------------------
//
// No-op transport backend for the NetDuke32 netcode seam.
//
// This lets the netcode compile, link, and boot GREEN with NO live transport.
// The real transport (browser WebRTC via EM_JS) is a separate track that
// replaces this file's functions; the netcode above the seam does not change.
//
// Compiled only when NETDUKE32 is defined (see GNUmakefile duke3d_excl).
//-------------------------------------------------------------------------

#include "net_transport.h"

extern "C" {

void net_send(int /*peerToken*/, int /*channel*/, int /*reliable*/, const void * /*data*/, int /*len*/)
{
    // no-op: no wire attached
}

void net_broadcast(int /*channel*/, int /*reliable*/, const void * /*data*/, int /*len*/)
{
    // no-op: no wire attached
}

void net_poll(void)
{
    // no-op: no inbound frames to deliver
}

void net_transport_init(void)
{
    // no-op
}

void net_transport_shutdown(void)
{
    // no-op
}

}  // extern "C"
