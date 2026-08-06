#ifndef SYNC_H
#define SYNC_H

#define MAX_SYNC_TYPES 24

#define DEFINE_SYNCFUNC(funcname) { #funcname, funcname }
typedef struct
{
    const char* name;
    char (*func)(void);
} SyncType_t;

extern char g_szfirstSyncMsg[MAX_SYNC_TYPES][60];
void Net_ResetSyncCheck(void); // auto-resync: clear the divergence verdict
int  Net_SyncErrorDetected(void); // any syncError[] set (display-independent)
void Net_DisplaySyncMsg(void);

extern int8_t syncData[MOVEFIFOSIZ][MAX_SYNC_TYPES];
extern bool syncError[MAX_SYNC_TYPES];
extern bool g_foundSyncError;
extern int  desynched_players[MAXPLAYERS];   // per-peer mismatch flags (host: heal targeting)
extern int32_t g_netCompareFloorTic;         // stamps below this tic are never compared

void initsynccrc(void);
void Net_GetSyncStat(void);
void Net_DisplaySyncMsg(void);
void Net_AddSyncInfoToPacket(int *j);
void Net_GetSyncInfoFromPacket(char *packbuf, int *j, int otherconnectindex);

#endif