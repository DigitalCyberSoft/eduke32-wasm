#ifndef OLDNET_H
#define OLDNET_H

#include "build.h"
#include "player.h"
#include "sync.h"

#ifdef OLDNET_CPP_
#define OLDNET_EXTERN
#else
#define OLDNET_EXTERN extern
#endif

#define INPUTFIFO_CURTICK (movefifoplc & (MOVEFIFOSIZ - 1))
#define INPUTFIFO_LASTTICK ((movefifoplc - 1) & (MOVEFIFOSIZ - 1))
#define INPUTFIFO_PREDICTTICK (predictfifoplc & (MOVEFIFOSIZ - 1))

struct votedata_t
{
    int32_t dmflags;
    int8_t starter = -1, level = -1, episode = -1;
    int8_t yes_votes;
    int8_t gametype, skill;
};
OLDNET_EXTERN votedata_t vote;

extern int quittimer;
extern int lastpackettime;
extern int mymaxlag, otherminlag, bufferjitter;

extern int movefifosendplc;
extern int movefifoplc;

OLDNET_EXTERN input_t netInput;

OLDNET_EXTERN bool oldnet_gotinitialsettings; // True if we got PACKET_TYPE_INIT_SETTINGS from the host.

//OLDNET_EXTERN PredictBackup_t predictBackup[MOVEFIFOSIZ];

enum DukePacket_t
{
    PACKET_TYPE_MASTER_TO_SLAVE,
    PACKET_TYPE_SLAVE_TO_MASTER,
    PACKET_TYPE_BROADCAST,
    SERVER_GENERATED_BROADCAST,
    PACKET_TYPE_VERSION,

    /* don't change anything above this line */

    PACKET_TYPE_MESSAGE,

    PACKET_TYPE_NEW_GAME,
    PACKET_TYPE_RTS,
    PACKET_TYPE_MENU_LEVEL_QUIT,
    PACKET_TYPE_WEAPON_CHOICE,
    PACKET_TYPE_PLAYER_OPTIONS,
    PACKET_TYPE_PLAYER_NAME,
    PACKET_TYPE_INIT_SETTINGS,

    PACKET_TYPE_USER_MAP,

    PACKET_TYPE_MAP_VOTE,
    PACKET_TYPE_MAP_VOTE_INITIATE,
    PACKET_TYPE_MAP_VOTE_CANCEL,

    PACKET_TYPE_LOAD_GAME,
    PACKET_TYPE_NULL_PACKET,
    PACKET_TYPE_PLAYER_READY,
    PACKET_TYPE_FRAGLIMIT_CHANGED,
    PACKET_TYPE_EOL,
    PACKET_TYPE_PING,
    PACKET_END, // Should remain last in list.
};

enum NetMode_t
{
    NETMODE_MASTERSLAVE,
    NETMODE_P2P, // UNSUPPORTED.
    NETMODE_OFFLINE = 255
};

void faketimerhandler(void);
void Net_HandleInput(void);
void Net_GetPackets(void);
void Net_ParsePackets(void);
void Net_SendQuit(void);
void Net_SendWeaponChoice(void);
void Net_SendVersion(void);
void Net_SendPlayerOptions(void);
void Net_SendFragLimit(void);
void Net_SendPlayerName(void);
void Net_SendUserMapName(void);
void Net_SendInitialSettings(void);
void Net_SendNewGame(uint32_t flags);
void Net_EndOfLevel(bool secret);
void Net_EnterMessage(void);

void Net_InitiateVote();
void Net_CancelVote();

void Net_ClearFIFO(void);
void Net_CheckPlayerQuit(int i);
void Net_Disconnect(bool showScores);
void Net_WaitForPlayers();

#endif
