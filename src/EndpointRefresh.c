#include "Limelight-internal.h"
#include "EndpointRefresh.h"
#include <stdatomic.h>

// These are local observation limits. They do not decide that a NAT mapping
// changed and they never cause the active UDP socket to be replaced.
enum {
    ENDPOINT_REFRESH_PROBE_INTERVAL_MS = 500,
    ENDPOINT_REFRESH_RESPONSE_INTERVAL_MS = 250,
    ENDPOINT_REFRESH_SUSPECT_TIMEOUT_MS = 2000,
    ENDPOINT_REFRESH_DEGRADED_TIMEOUT_MS = 8000,
    ENDPOINT_REFRESH_MAINTENANCE_INTERVAL_MS = 100,
    ENDPOINT_REFRESH_SEQUENCE_WINDOW = 8
};

bool EndpointRefreshRequested;
static atomic_bool enabled;

bool LiIsEndpointAutoRefreshEnabled(void) {
    return atomic_load_explicit(&enabled, memory_order_acquire);
}

void LiSetEndpointAutoRefreshEnabled(bool value) {
    atomic_store_explicit(&enabled, value, memory_order_release);
}

bool LiErClientInitialize(LI_ENDPOINT_REFRESH* state, SOCKET socket, uint8_t stream,
                          const char session[LI_ER_SESSION_SIZE], uint16_t remotePort) {
    memset(state, 0, sizeof(*state));
    if (!EndpointRefreshRequested) {
        return true;
    }
    if (PltCreateMutex(&state->mutex)) {
        return false;
    }
    state->active = socket;
    state->activeResponseAt = UINT64_MAX;
    state->stream = stream;
    memcpy(state->session, session, LI_ER_SESSION_SIZE);
    if (!LiErDeriveKey((const unsigned char*)StreamConfig.remoteInputAesKey, state->session, state->key)) {
        PltDeleteMutex(&state->mutex);
        return false;
    }
    memcpy(&state->remote, &RemoteAddr, sizeof(state->remote));
#ifdef AF_INET6
    SET_PORT((struct sockaddr_in6*)&state->remote, remotePort);
#else
    SET_PORT((struct sockaddr_in*)&state->remote, remotePort);
#endif
    state->initialized = true;
    return true;
}

void LiErClientDestroy(LI_ENDPOINT_REFRESH* state) {
    if (!state->initialized) {
        return;
    }
    PltDeleteMutex(&state->mutex);
    LiErWipe(state, sizeof(*state));
}

static void sendProbe(LI_ENDPOINT_REFRESH* state, uint64_t now) {
    unsigned char packet[LI_ER_PACKET_SIZE];
    if (now - state->lastProbeAt < ENDPOINT_REFRESH_PROBE_INTERVAL_MS) {
        return;
    }
    if (state->sequence != UINT64_MAX && LiErEncode(packet, LI_ER_PROBE, state->stream,
            state->session, state->key, ++state->sequence, NULL)) {
        state->lastProbeAt = now;
        sendto(state->active, (char*)packet, sizeof(packet), 0,
            (struct sockaddr*)&state->remote, AddrLen);
    }
}

bool LiErClientPing(LI_ENDPOINT_REFRESH* state) {
    if (!state->initialized) {
        return false;
    }
    if (!LiIsEndpointAutoRefreshEnabled()) {
        return true;
    }
    PltLockMutex(&state->mutex);
    sendProbe(state, PltGetMillis());
    PltUnlockMutex(&state->mutex);
    return true;
}

static void handle(LI_ENDPOINT_REFRESH* state, const void* packet, size_t size, uint64_t now) {
    LI_ER_MESSAGE message;
    unsigned char response[LI_ER_PACKET_SIZE];
    if (!LiErDecode(packet, size, state->stream, state->session, state->key, &message) ||
        message.sequence > state->sequence ||
        state->sequence - message.sequence > ENDPOINT_REFRESH_SEQUENCE_WINDOW) {
        return;
    }
    if (message.type == LI_ER_CHALLENGE && message.sequence > state->ackSequence) {
        if (state->activeResponseAt != UINT64_MAX &&
            now - state->activeResponseAt < ENDPOINT_REFRESH_RESPONSE_INTERVAL_MS) {
            return;
        }
        if (LiErEncode(response, LI_ER_RESPONSE, state->stream, state->session, state->key,
                message.sequence, message.nonce)) {
            state->activeResponseAt = now;
            state->lastAck = now;
            sendto(state->active, (char*)response, sizeof(response), 0,
                (struct sockaddr*)&state->remote, AddrLen);
        }
    }
    else if (message.type == LI_ER_ACK && message.sequence > state->ackSequence) {
        state->ackSequence = message.sequence;
        state->lastAck = now;
        state->suspect = false;
        state->degraded = false;
        state->degradedNotified = false;
    }
}

static void maintain(LI_ENDPOINT_REFRESH* state, uint64_t now) {
    if (now - state->lastPoll < ENDPOINT_REFRESH_MAINTENANCE_INTERVAL_MS) {
        return;
    }
    state->lastPoll = now;
    PltLockMutex(&state->mutex);
    uint64_t lastActivity = state->lastAck;
    if (state->lastMedia > lastActivity) {
        lastActivity = state->lastMedia;
    }
    uint64_t quiet = now - lastActivity;
    if (quiet >= ENDPOINT_REFRESH_SUSPECT_TIMEOUT_MS) {
        state->suspect = true;
    }
    else {
        state->suspect = false;
        state->degraded = false;
        state->degradedNotified = false;
    }
    if (state->suspect && quiet >= ENDPOINT_REFRESH_DEGRADED_TIMEOUT_MS) {
        state->degraded = true;
    }
    PltUnlockMutex(&state->mutex);
}

void LiErClientMedia(LI_ENDPOINT_REFRESH* state) {
    if (!state->initialized) {
        return;
    }
    PltLockMutex(&state->mutex);
    state->lastMedia = PltGetMillis();
    state->suspect = false;
    state->degraded = false;
    state->degradedNotified = false;
    PltUnlockMutex(&state->mutex);
}

int LiErClientRecv(LI_ENDPOINT_REFRESH* state, SOCKET* socket, char* packet, int size, bool useSelect) {
    if (!state->initialized || !LiIsEndpointAutoRefreshEnabled()) {
        return recvUdpSocket(*socket, packet, size, useSelect);
    }
    uint64_t receiveTime = PltGetMillis();
    if (!state->receiving) {
        PltLockMutex(&state->mutex);
        state->lastAck = state->lastMedia = state->lastPoll = receiveTime;
        state->receiveTime = receiveTime;
        state->receiving = true;
        PltUnlockMutex(&state->mutex);
    }
    maintain(state, receiveTime);
    int length = recvUdpSocket(state->active, packet, size, useSelect);
    receiveTime = PltGetMillis();
    PltLockMutex(&state->mutex);
    state->receiveTime = receiveTime;
    PltUnlockMutex(&state->mutex);
    if (length <= 0) {
        PltLockMutex(&state->mutex);
        bool notifyDegraded = state->degraded && !state->degradedNotified;
        if (notifyDegraded) {
            state->degradedNotified = true;
        }
        PltUnlockMutex(&state->mutex);
        if (notifyDegraded) {
            return LI_ER_RECV_DEGRADED;
        }
        return length;
    }
    if (length == LI_ER_PACKET_SIZE && !memcmp(packet, "EAPR", 4)) {
        PltLockMutex(&state->mutex);
        handle(state, packet, (size_t)length, receiveTime);
        PltUnlockMutex(&state->mutex);
        return LI_ER_RECV_HANDLED;
    }
    return length;
}
