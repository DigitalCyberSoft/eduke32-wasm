#ifndef SYNC_H
#define SYNC_H

#define MAX_SYNC_TYPES 16

#define DEFINE_SYNCFUNC(funcname) { #funcname, funcname }
typedef struct
{
    const char* name;
    char (*func)(void);
} SyncType_t;

extern char g_szfirstSyncMsg[MAX_SYNC_TYPES][60];

extern int8_t syncData[MOVEFIFOSIZ][MAX_SYNC_TYPES];
extern bool syncError[MAX_SYNC_TYPES];
extern bool g_foundSyncError;

void initsynccrc(void);
void Net_GetSyncStat(void);
void Net_DisplaySyncMsg(void);
void Net_AddSyncInfoToPacket(int *j);
void Net_GetSyncInfoFromPacket(char *packbuf, int *j, int otherconnectindex);

#endif