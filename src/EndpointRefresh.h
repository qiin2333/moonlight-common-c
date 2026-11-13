#pragma once
#include "PlatformSockets.h"
#include "PlatformThreads.h"
#include "EndpointRefreshProtocol.h"

typedef struct _LI_ENDPOINT_REFRESH {
    bool initialized;
    bool receiving;
    bool suspect;
    bool degraded;
    bool degradedNotified;
    bool probeSent;
    PLT_MUTEX mutex;
    SOCKET active;
    struct sockaddr_storage remote;
    uint8_t stream;
    unsigned char session[LI_ER_SESSION_SIZE], key[LI_ER_KEY_SIZE];
    uint64_t sequence, ackSequence;
    uint64_t lastAck, lastMedia, lastPoll;
    uint64_t lastProbeAt, activeResponseAt;
} LI_ENDPOINT_REFRESH;

bool LiErClientInitialize(LI_ENDPOINT_REFRESH* state, SOCKET socket, uint8_t stream,
                          const char session[LI_ER_SESSION_SIZE], uint16_t remotePort);
void LiErClientDestroy(LI_ENDPOINT_REFRESH* state);
bool LiErClientPing(LI_ENDPOINT_REFRESH* state);
// Distinguish protocol traffic from a real receive timeout/socket error.
#define LI_ER_RECV_HANDLED -2
// The stream has been quiet for the observation window. The caller decides
// whether a single media stream can degrade independently of the session.
#define LI_ER_RECV_DEGRADED -4
int LiErClientRecv(LI_ENDPOINT_REFRESH* state, SOCKET* socket, char* packet, int size, bool useSelect);
void LiErClientMedia(LI_ENDPOINT_REFRESH* state);

extern bool EndpointRefreshRequested;
