// Red characterizations for Phase-1 native seat ownership/capacity defects.
// Links the real net_transport_native.cpp with a test-only, default-off accessor.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#define SJSON_IMPLEMENT
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define ABORT_IF_F(cond, ...) \
    do { if (cond) abort(); } while (0)
#define sjson_malloc(user, size) (malloc(size))
#define sjson_free(user, ptr) (free(ptr))
#define sjson_realloc(user, ptr, size) (realloc(ptr, size))
#include <sjson.h>
#undef SJSON_IMPLEMENT

extern "C" int NN_TestNativeNextFreeSlot(const int *slots, int count);

// Standalone transport link stubs; no engine or network is initialized by this test.
extern "C" {
void Net_ReceiveFrame(int, int, const unsigned char *, int) {}
void Net_PeerEvent(int, int) {}
void Net_SetLocalIndex(int) {}
void Net_SnapshotReady(int, int, int) {}
void NetMenu_SetStatus(const char *) {}
void NetMenu_SetRoster(const char *) {}
void NetMenu_OnJoined(int) {}
const char *Net_NativeGrpPath(void) { return ""; }
uint32_t Bcrc32(const void *, int, uint32_t crc) { return crc; }
}

static int expectEq(const char *id, int actual, int expected)
{
    if (actual == expected)
    {
        std::printf("ok %s actual=%d\n", id, actual);
        return 0;
    }
    std::fprintf(stderr, "RED %s expected=%d actual=%d\n", id, expected, actual);
    return 1;
}

int main()
{
    int failures = 0;
    // H-03 capacity alias: a full guest roster must report no seat. Baseline's
    // nextFreeSlot() returns seat 1, colliding with the first guest.
    int full[15];
    for (int i = 0; i < 15; i++) full[i] = i + 1;
    failures += expectEq("H-03 native full roster returns no seat", NN_TestNativeNextFreeSlot(full, 15), -1);

    // H-02 CPU-seat collision: the transport sees no human mapping for seat 1,
    // but that seat is engine-owned by a CPU. A correct allocator consults the
    // bot mask on the game thread and skips to 2. Baseline aliases seat 1.
    failures += expectEq("H-02 native allocator skips CPU-reserved seat 1", NN_TestNativeNextFreeSlot(nullptr, 0), 2);

    return failures ? 1 : 0;
}
