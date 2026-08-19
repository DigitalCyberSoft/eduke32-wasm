#ifndef NET_PHASE2_H
#define NET_PHASE2_H

#include <cstdint>

// Wire-stable constants for the additive PACKET_TYPE_NEW_GAME team vector.
// Valid Duke teams are 0..3; the vector is always one byte per engine seat.
enum { NET_TEAM_COUNT = 4, NET_TEAM_VECTOR_SIZE = 16 };

static inline int Net_ClampTeam(int team)
{
    return team < 0 ? 0 : (team >= NET_TEAM_COUNT ? NET_TEAM_COUNT - 1 : team);
}

// Deterministic least-populated choice, with the scan order providing the
// required lowest-team tie break.
static inline int Net_LeastPopulatedTeam(int const teamCount[NET_TEAM_COUNT])
{
    int team = 0;
    for (int t = 1; t < NET_TEAM_COUNT; t++)
        if (teamCount[t] < teamCount[team])
            team = t;
    return team;
}

// Decode only a complete vector. A legacy packet (or a truncated extension)
// returns false without touching the destination, preserving known local teams.
static inline bool Net_DecodeTeamVector(uint8_t const *bytes, int len,
                                        int teamOut[NET_TEAM_VECTOR_SIZE])
{
    if (bytes == nullptr || teamOut == nullptr || len < NET_TEAM_VECTOR_SIZE)
        return false;

    int decoded[NET_TEAM_VECTOR_SIZE];
    for (int k = 0; k < NET_TEAM_VECTOR_SIZE; k++)
        decoded[k] = Net_ClampTeam(bytes[k]);
    for (int k = 0; k < NET_TEAM_VECTOR_SIZE; k++)
        teamOut[k] = decoded[k];
    return true;
}

#endif
