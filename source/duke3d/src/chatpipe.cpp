//-------------------------------------------------------------------------
//
// ChatPipe stub for the WASM/NetDuke32 build.
//
// The upstream NetDuke32 ChatPipe bridges chat to an external process via OS
// named pipes, which is neither available nor meaningful in a browser. The
// netcode only needs these entry points to exist; chat still flows over the
// normal in-band PACKET_TYPE_MESSAGE path.
//
// Compiled only when NETDUKE32 is defined.
//-------------------------------------------------------------------------

#include "chatpipe.h"

void ChatPipe_Create(void)
{
    // no-op: no external chat pipe in the browser build
}

void ChatPipe_SendMessage(const char * /*message*/)
{
    // no-op
}

void ChatPipe_Poll(void)
{
    // no-op
}
