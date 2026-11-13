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
    atomic_init(&state->probeAttempts, 0);
    atomic_init(&state->probeSendFailures, 0);
    atomic_init(&state->probeConfirmed, 0);
    atomic_init(&state->challengeReceived, 0);
    atomic_init(&state->ackReceived, 0);
    atomic_init(&state->statsSuspect, false);
    atomic_init(&state->statsDegraded, false);
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
    // Send the first probe immediately. The first call can occur shortly
    // after process start, when the monotonic clock is still below the
    // regular probe interval.
    if (state->probeSent && now - state->lastProbeAt < ENDPOINT_REFRESH_PROBE_INTERVAL_MS) {
        return;
    }
    if (state->sequence != UINT64_MAX && LiErEncode(packet, LI_ER_PROBE, state->stream,
            state->session, state->key, ++state->sequence, NULL)) {
        state->lastProbeAt = now;
        state->probeSent = true;
        atomic_fetch_add_explicit(&state->probeAttempts, 1, memory_order_relaxed);
        SOCK_RET result = sendto(state->active, (char*)packet, sizeof(packet), 0,
            (struct sockaddr*)&state->remote, AddrLen);
        if (result != (SOCK_RET)sizeof(packet)) {
            atomic_fetch_add_explicit(&state->probeSendFailures, 1, memory_order_relaxed);
        }
    }
}

bool LiErClientPing(LI_ENDPOINT_REFRESH* state) {
    if (!state->initialized) {
        return false;
    }
    if (!LiIsEndpointAutoRefreshEnabled()) {
        // EARP is enabled only after the encrypted RTSP negotiation has been
        // confirmed. Until then, callers must keep sending the legacy ping
        // used to start audio/video transport on older hosts.
        return false;
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
        if (message.sequence > state->lastConfirmedSequence) {
            atomic_fetch_add_explicit(&state->probeConfirmed, 1, memory_order_relaxed);
            state->lastConfirmedSequence = message.sequence;
        }
        atomic_fetch_add_explicit(&state->challengeReceived, 1, memory_order_relaxed);
        // A valid challenge proves that the original socket can still reach
        // the host. Restore the observation state even if response rate
        // limiting suppresses this particular response.
        state->lastAck = now;
        state->suspect = false;
        state->degraded = false;
        state->degradedNotified = false;
        atomic_store_explicit(&state->statsSuspect, false, memory_order_relaxed);
        atomic_store_explicit(&state->statsDegraded, false, memory_order_relaxed);
        if (state->activeResponseAt != UINT64_MAX &&
            now - state->activeResponseAt < ENDPOINT_REFRESH_RESPONSE_INTERVAL_MS) {
            return;
        }
        if (LiErEncode(response, LI_ER_RESPONSE, state->stream, state->session, state->key,
                message.sequence, message.nonce)) {
            state->activeResponseAt = now;
            sendto(state->active, (char*)response, sizeof(response), 0,
                (struct sockaddr*)&state->remote, AddrLen);
        }
    }
    else if (message.type == LI_ER_ACK && message.sequence > state->ackSequence) {
        if (message.sequence > state->lastConfirmedSequence) {
            atomic_fetch_add_explicit(&state->probeConfirmed, 1, memory_order_relaxed);
            state->lastConfirmedSequence = message.sequence;
        }
        atomic_fetch_add_explicit(&state->ackReceived, 1, memory_order_relaxed);
        state->ackSequence = message.sequence;
        state->lastAck = now;
        state->suspect = false;
        state->degraded = false;
        state->degradedNotified = false;
        atomic_store_explicit(&state->statsSuspect, false, memory_order_relaxed);
        atomic_store_explicit(&state->statsDegraded, false, memory_order_relaxed);
    }
}

static void maintain(LI_ENDPOINT_REFRESH* state) {
    PltLockMutex(&state->mutex);
    uint64_t now = PltGetMillis();
    if (now - state->lastPoll < ENDPOINT_REFRESH_MAINTENANCE_INTERVAL_MS) {
        PltUnlockMutex(&state->mutex);
        return;
    }
    state->lastPoll = now;
    uint64_t lastActivity = state->lastAck;
    if (state->lastMedia > lastActivity) {
        lastActivity = state->lastMedia;
    }
    uint64_t quiet = now > lastActivity ? now - lastActivity : 0;
    if (quiet >= ENDPOINT_REFRESH_SUSPECT_TIMEOUT_MS) {
        state->suspect = true;
        atomic_store_explicit(&state->statsSuspect, true, memory_order_relaxed);
    }
    else {
        state->suspect = false;
        state->degraded = false;
        state->degradedNotified = false;
        atomic_store_explicit(&state->statsSuspect, false, memory_order_relaxed);
        atomic_store_explicit(&state->statsDegraded, false, memory_order_relaxed);
    }
    if (state->suspect && quiet >= ENDPOINT_REFRESH_DEGRADED_TIMEOUT_MS) {
        state->degraded = true;
        atomic_store_explicit(&state->statsDegraded, true, memory_order_relaxed);
    }
    PltUnlockMutex(&state->mutex);
}

void LiErClientMedia(LI_ENDPOINT_REFRESH* state) {
    if (!state->initialized) {
        return;
    }
    PltLockMutex(&state->mutex);
    uint64_t now = PltGetMillis();
    state->lastMedia = now;
    state->suspect = false;
    state->degraded = false;
    state->degradedNotified = false;
    atomic_store_explicit(&state->statsSuspect, false, memory_order_relaxed);
    atomic_store_explicit(&state->statsDegraded, false, memory_order_relaxed);
    PltUnlockMutex(&state->mutex);
}

bool LiErClientGetStats(LI_ENDPOINT_REFRESH* state, LI_ENDPOINT_REFRESH_STATS* stats) {
    if (state == NULL || stats == NULL || !state->initialized ||
        !LiIsEndpointAutoRefreshEnabled()) {
        return false;
    }
    stats->probeAttempts = atomic_load_explicit(&state->probeAttempts, memory_order_relaxed);
    stats->probeSendFailures = atomic_load_explicit(&state->probeSendFailures, memory_order_relaxed);
    stats->probeConfirmed = atomic_load_explicit(&state->probeConfirmed, memory_order_relaxed);
    stats->challengeReceived = atomic_load_explicit(&state->challengeReceived, memory_order_relaxed);
    stats->ackReceived = atomic_load_explicit(&state->ackReceived, memory_order_relaxed);
    stats->suspect = atomic_load_explicit(&state->statsSuspect, memory_order_relaxed);
    stats->degraded = atomic_load_explicit(&state->statsDegraded, memory_order_relaxed);
    return true;
}

int LiErClientRecv(LI_ENDPOINT_REFRESH* state, SOCKET* socket, char* packet, int size, bool useSelect) {
    if (!state->initialized || !LiIsEndpointAutoRefreshEnabled()) {
        return recvUdpSocket(*socket, packet, size, useSelect);
    }
    uint64_t receiveTime = PltGetMillis();
    if (!state->receiving) {
        PltLockMutex(&state->mutex);
        state->lastAck = state->lastMedia = state->lastPoll = receiveTime;
        state->receiving = true;
        PltUnlockMutex(&state->mutex);
    }
    maintain(state);
    int length = recvUdpSocket(state->active, packet, size, useSelect);
    receiveTime = PltGetMillis();
    if (length <= 0) {
        if (length < 0) {
            return length;
        }
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
