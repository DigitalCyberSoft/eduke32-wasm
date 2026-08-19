// Deterministic Phase-2 gate: TDM least-populated assignment and the additive
// NEW_GAME pteam vector. This uses the production header directly and needs no
// game data, transport, external dependency, or RNG state.
#include "net_phase2.h"

#include <cstdio>

static int fail(char const *message)
{
    std::fprintf(stderr, "PHASE2 MODE TEST FAIL: %s\n", message);
    return 1;
}

int main()
{
    int count[NET_TEAM_COUNT] = { 1, 0, 0, 0 };
    if (Net_LeastPopulatedTeam(count) != 1)
        return fail("lone team-0 host did not place first CPU on team 1");
    count[1]++;
    if (Net_LeastPopulatedTeam(count) != 2)
        return fail("updated roster did not place second CPU on team 2");
    count[2]++;
    if (Net_LeastPopulatedTeam(count) != 3)
        return fail("updated roster did not place third CPU on team 3");
    int tied[NET_TEAM_COUNT] = { 2, 2, 2, 2 };
    if (Net_LeastPopulatedTeam(tied) != 0)
        return fail("team tie did not choose the lowest team");

    uint8_t wire[NET_TEAM_VECTOR_SIZE];
    for (int k = 0; k < NET_TEAM_VECTOR_SIZE; k++)
        wire[k] = (uint8_t)k;
    int team[NET_TEAM_VECTOR_SIZE];
    for (int &value : team)
        value = 99;

    if (Net_DecodeTeamVector(wire, 0, team) || team[0] != 99)
        return fail("legacy packet changed existing teams");
    if (Net_DecodeTeamVector(wire, NET_TEAM_VECTOR_SIZE - 1, team) || team[0] != 99)
        return fail("partial vector was not rejected atomically");
    if (!Net_DecodeTeamVector(wire, NET_TEAM_VECTOR_SIZE, team))
        return fail("complete vector was rejected");
    for (int k = 0; k < NET_TEAM_VECTOR_SIZE; k++)
        if (team[k] != (k < NET_TEAM_COUNT ? k : NET_TEAM_COUNT - 1))
            return fail("wire team was not clamped to 0..3");

    std::puts("PHASE2 MODE TEST OK");
    return 0;
}
