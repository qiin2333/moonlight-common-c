// Conservative endpoint refresh tests over real loopback UDP.
#include "Limelight-internal.h"
#include "EndpointRefresh.h"
#include <stdio.h>

STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
struct sockaddr_storage RemoteAddr, LocalAddr;
SOCKADDR_LEN AddrLen = sizeof(struct sockaddr_in);
static uint64_t now = 10000;
static unsigned int socketCreates, socketCloses, clockReads;

uint64_t PltGetMillis(void) { clockReads++; return now; }
int PltCreateMutex(PLT_MUTEX* mutex) {
#ifdef LC_WINDOWS
    InitializeSRWLock(mutex);
    return 0;
#else
    return pthread_mutex_init(mutex, NULL);
#endif
}
void PltDeleteMutex(PLT_MUTEX* mutex) {
#ifdef LC_WINDOWS
    (void)mutex;
#else
    pthread_mutex_destroy(mutex);
#endif
}
void PltLockMutex(PLT_MUTEX* mutex) {
#ifdef LC_WINDOWS
    AcquireSRWLockExclusive(mutex);
#else
    pthread_mutex_lock(mutex);
#endif
}
void PltUnlockMutex(PLT_MUTEX* mutex) {
#ifdef LC_WINDOWS
    ReleaseSRWLockExclusive(mutex);
#else
    pthread_mutex_unlock(mutex);
#endif
}
void LiRecordStreamSocket(SOCKET s, int slot) { (void)s; (void)slot; }
void LiReplaceStreamSocket(SOCKET previous, SOCKET replacement, int slot) {
    (void)previous; (void)replacement; (void)slot;
}
int setNonFatalRecvTimeoutMs(SOCKET s, int timeout) { (void)s; (void)timeout; return 0; }
void closeSocket(SOCKET s) {
    socketCloses++;
#ifdef LC_WINDOWS
    closesocket(s);
#else
    close(s);
#endif
}
SOCKET bindUdpSocket(int family, struct sockaddr_storage* local, SOCKADDR_LEN length, int bufferSize, int qos) {
    (void)bufferSize;
    (void)qos;
    SOCKET socketValue = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (socketValue == INVALID_SOCKET) {
        return socketValue;
    }
    if (bind(socketValue, (struct sockaddr*)local, length) != 0) {
        closeSocket(socketValue);
        return INVALID_SOCKET;
    }
    socketCreates++;
    return socketValue;
}
int pollSockets(struct pollfd* fds, int count, int timeout) {
    (void)timeout;
    fd_set reads;
    struct timeval immediate = { 0, 0 };
    SOCKET highest = 0;
    FD_ZERO(&reads);
    for (int i = 0; i < count; i++) {
        fds[i].revents = 0;
        FD_SET(fds[i].fd, &reads);
        if (fds[i].fd > highest) {
            highest = fds[i].fd;
        }
    }
    int result = select((int)(highest + 1), &reads, NULL, NULL, &immediate);
    if (result > 0) {
        for (int i = 0; i < count; i++) {
            if (FD_ISSET(fds[i].fd, &reads)) {
                fds[i].revents = POLLIN;
            }
        }
    }
    return result;
}
int recvUdpSocket(SOCKET socketValue, char* buffer, int size, bool useSelect) {
    (void)useSelect;
    struct pollfd readable = { .fd = socketValue, .events = POLLIN };
    if (pollSockets(&readable, 1, 0) <= 0) {
        return 0;
    }
    return (int)recvfrom(socketValue, buffer, size, 0, NULL, NULL);
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); return 1; } } while (0)

static int receiveEapr(SOCKET host, LI_ENDPOINT_REFRESH* state, LI_ER_MESSAGE* message, struct sockaddr_storage* peer) {
    unsigned char packet[LI_ER_PACKET_SIZE];
    SOCKADDR_LEN peerLength = sizeof(*peer);
    struct pollfd readable = { .fd = host, .events = POLLIN };
    if (pollSockets(&readable, 1, 0) <= 0) {
        return 0;
    }
    int length = recvfrom(host, (char*)packet, sizeof(packet), 0,
        (struct sockaddr*)peer, &peerLength);
    return length == LI_ER_PACKET_SIZE && LiErDecode(packet, length, LI_ER_VIDEO,
        state->session, state->key, message);
}

static int sendToPeer(SOCKET host, const struct sockaddr_storage* peer,
                      const unsigned char* packet, int size) {
    return (int)sendto(host, (const char*)packet, size, 0,
        (const struct sockaddr*)peer, AddrLen);
}

int main(void) {
#ifdef LC_WINDOWS
    WSADATA wsa;
    CHECK(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    memset(&StreamConfig, 0, sizeof(StreamConfig));
    struct sockaddr_in* local = (struct sockaddr_in*)&LocalAddr;
    local->sin_family = AF_INET;
    local->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    SOCKET host = bindUdpSocket(AF_INET, &LocalAddr, AddrLen, 0, 0);
    CHECK(host != INVALID_SOCKET);
    SOCKADDR_LEN length = sizeof(RemoteAddr);
    CHECK(getsockname(host, (struct sockaddr*)&RemoteAddr, &length) == 0);

    SOCKET active = bindUdpSocket(AF_INET, &LocalAddr, AddrLen, 0, 0);
    CHECK(active != INVALID_SOCKET);
    struct sockaddr_storage activePeer;
    length = sizeof(activePeer);
    CHECK(getsockname(active, (struct sockaddr*)&activePeer, &length) == 0);

    LI_ENDPOINT_REFRESH state;
    EndpointRefreshRequested = true;
    LiSetEndpointAutoRefreshEnabled(false);
    CHECK(LiErClientInitialize(&state, active, LI_ER_VIDEO,
        "0123456789abcdef", ntohs(((struct sockaddr_in*)&RemoteAddr)->sin_port)));
    // Before ANNOUNCE confirms EARP, the caller must fall back to the legacy
    // ping so RTSP/SETUP startup compatibility is preserved.
    CHECK(!LiErClientPing(&state));
    LiSetEndpointAutoRefreshEnabled(true);
    now += 1;
    CHECK(LiErClientPing(&state));
    LI_ER_MESSAGE probe;
    struct sockaddr_storage probePeer;
    CHECK(receiveEapr(host, &state, &probe, &probePeer));
    CHECK(probe.type == LI_ER_PROBE);
    LI_ENDPOINT_REFRESH_STATS stats;
    CHECK(LiErClientGetStats(&state, &stats));
    CHECK(stats.probeAttempts == 1 && stats.probeConfirmed == 0);
    CHECK(((struct sockaddr_in*)&probePeer)->sin_port ==
        ((struct sockaddr_in*)&activePeer)->sin_port);

    unsigned char ack[LI_ER_PACKET_SIZE];
    CHECK(LiErEncode(ack, LI_ER_ACK, LI_ER_VIDEO, state.session, state.key,
        probe.sequence, NULL));
    CHECK(sendToPeer(host, &activePeer, ack, sizeof(ack)) == sizeof(ack));
    unsigned char buffer[128];
    CHECK(LiErClientRecv(&state, &state.active, (char*)buffer, sizeof(buffer), false) ==
        LI_ER_RECV_HANDLED);
    CHECK(!state.suspect && !state.degraded);
    CHECK(LiErClientGetStats(&state, &stats));
    CHECK(stats.probeConfirmed == 1 && stats.ackReceived == 1);

    // Distinguish a response that reaches the socket from one that passes
    // the authenticated endpoint-refresh checks.
    unsigned char invalidAck[LI_ER_PACKET_SIZE];
    memcpy(invalidAck, ack, sizeof(invalidAck));
    invalidAck[LI_ER_PACKET_SIZE - 1] ^= 1;
    CHECK(sendToPeer(host, &activePeer, invalidAck, sizeof(invalidAck)) == sizeof(invalidAck));
    CHECK(LiErClientRecv(&state, &state.active, (char*)buffer, sizeof(buffer), false) ==
        LI_ER_RECV_HANDLED);
    CHECK(LiErClientGetStats(&state, &stats));
    CHECK(stats.responsePacketsReceived == 2 && stats.invalidResponses == 1);

    // Simulate high-bitrate queueing: no return traffic for 2 seconds. The
    // original socket remains the only socket and the stream is not failed.
    now += 2100;
    CHECK(LiErClientRecv(&state, &state.active, (char*)buffer, sizeof(buffer), false) == 0);
    CHECK(state.suspect && !state.degraded && socketCreates == 2);
    CHECK(LiErClientGetStats(&state, &stats));
    CHECK(stats.suspect && !stats.degraded);
    now += 6000;
    CHECK(LiErClientRecv(&state, &state.active, (char*)buffer, sizeof(buffer), false) ==
        LI_ER_RECV_DEGRADED);
    CHECK(state.degraded && socketCreates == 2 && socketCloses == 0);
    CHECK(LiErClientGetStats(&state, &stats));
    CHECK(stats.suspect && stats.degraded);

    // A valid challenge also proves that the original path is reachable. It
    // must clear the observation state and produce a response without any
    // socket replacement.
    unsigned char nonce[LI_ER_NONCE_SIZE] = { 1 };
    now += 500;
    CHECK(LiErClientPing(&state));
    CHECK(receiveEapr(host, &state, &probe, &probePeer));
    CHECK(probe.type == LI_ER_PROBE);
    CHECK(LiErEncode(ack, LI_ER_CHALLENGE, LI_ER_VIDEO, state.session, state.key,
        probe.sequence, nonce));
    CHECK(sendToPeer(host, &activePeer, ack, sizeof(ack)) == sizeof(ack));
    CHECK(LiErClientRecv(&state, &state.active, (char*)buffer, sizeof(buffer), false) ==
        LI_ER_RECV_HANDLED);
    CHECK(!state.suspect && !state.degraded);
    LI_ER_MESSAGE response;
    CHECK(receiveEapr(host, &state, &response, &probePeer));
    CHECK(response.type == LI_ER_RESPONSE && response.sequence == probe.sequence);
    CHECK(!memcmp(response.nonce, nonce, sizeof(nonce)));

    // Long probe stress: one original socket, no candidate bind and no fail.
    for (int i = 0; i < 2000; i++) {
        now += 500;
        CHECK(LiErClientPing(&state));
        CHECK(receiveEapr(host, &state, &probe, &probePeer));
        CHECK(probe.type == LI_ER_PROBE);
        CHECK(((struct sockaddr_in*)&probePeer)->sin_port ==
            ((struct sockaddr_in*)&activePeer)->sin_port);
    }
    CHECK(socketCreates == 2 && socketCloses == 0 && state.active == active);

    // A late ACK on the original socket restores health without migration.
    CHECK(LiErEncode(ack, LI_ER_ACK, LI_ER_VIDEO, state.session, state.key,
        probe.sequence, NULL));
    CHECK(sendToPeer(host, &activePeer, ack, sizeof(ack)) == sizeof(ack));
    CHECK(LiErClientRecv(&state, &state.active, (char*)buffer, sizeof(buffer), false) ==
        LI_ER_RECV_HANDLED);
    CHECK(!state.suspect && !state.degraded);

    LiErClientDestroy(&state);
    closeSocket(active);
    closeSocket(host);
    CHECK(socketCreates == socketCloses);
#ifdef LC_WINDOWS
    WSACleanup();
#endif
    puts("Endpoint refresh conservative client: loopback probe, ACK recovery, 2000-cycle stress and no candidate passed");
    return 0;
}
