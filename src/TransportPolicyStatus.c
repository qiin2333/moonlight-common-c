#include "TransportPolicyStatus.h"
#include <string.h>

static uint64_t readInteger(const uint8_t* p, size_t n) {
    uint64_t value = 0;
    for (size_t i = 0; i < n; ++i) value = (value << 8) | p[i];
    return value;
}
static void writeInteger(uint8_t* p, uint64_t value, size_t n) {
    for (size_t i = n; i != 0; --i) { p[i - 1] = (uint8_t)value; value >>= 8; }
}
static bool valid(const TPS_STATUS_NOTICE* n) {
    if (n == NULL || (n->flags & ~0x7fu) || n->controlSource > 3 || n->failure > 4 ||
        !n->sessionId || !n->connectionEpoch || !n->noticeSequence || !n->controlEpoch || !n->acceptedRevision) return false;
    if (n->flags & TPS_APPLIED_KNOWN) {
        if (!n->encoderAppliedRevision || n->encoderAppliedRevision > n->acceptedRevision) return false;
    }
    else if (n->encoderAppliedRevision) return false;
    if (n->flags & TPS_FIRST_SENT_KNOWN) {
        if (!(n->flags & TPS_APPLIED_KNOWN) || !n->firstSentRevision ||
            n->firstSentRevision > n->encoderAppliedRevision) return false;
    }
    else if (n->firstSentRevision || n->firstSentFrame) return false;
    if ((n->flags & TPS_PENDING) && (n->failure || n->encoderAppliedRevision == n->acceptedRevision)) return false;
    if ((n->flags & TPS_STOPPED) && (n->flags & (TPS_PENDING | TPS_ENCODER_READY | TPS_PACKET_CONTROL | TPS_VIDEO_PACER))) return false;
    return true;
}
size_t TpsEncodeStatus(const TPS_STATUS_NOTICE* n, uint8_t* output, size_t capacity) {
    if (!valid(n) || output == NULL || capacity < TPS_STATUS_BYTES) return 0;
    memset(output, 0, TPS_STATUS_BYTES);
    writeInteger(output, TPS_STATUS_VERSION, 2);
    writeInteger(output + 2, TPS_STATUS_BYTES, 2);
    writeInteger(output + 4, n->flags, 2);
    output[6] = n->controlSource; output[7] = n->failure;
    writeInteger(output + 8, n->sessionId, 4);
    writeInteger(output + 16, n->connectionEpoch, 8);
    writeInteger(output + 24, n->noticeSequence, 8);
    writeInteger(output + 32, n->controlEpoch, 8);
    writeInteger(output + 40, n->acceptedRevision, 8);
    writeInteger(output + 48, n->encoderAppliedRevision, 8);
    writeInteger(output + 56, n->firstSentRevision, 8);
    writeInteger(output + 64, n->firstSentFrame, 8);
    return TPS_STATUS_BYTES;
}
bool TpsDecodeStatus(const uint8_t* payload, size_t length, TPS_STATUS_NOTICE* notice) {
    if (payload == NULL || notice == NULL || length != TPS_STATUS_BYTES ||
        readInteger(payload, 2) != TPS_STATUS_VERSION || readInteger(payload + 2, 2) != TPS_STATUS_BYTES ||
        readInteger(payload + 12, 4) != 0) return false;
    TPS_STATUS_NOTICE next = {0};
    next.flags = (uint16_t)readInteger(payload + 4, 2);
    next.controlSource = payload[6]; next.failure = payload[7];
    next.sessionId = (uint32_t)readInteger(payload + 8, 4);
    next.connectionEpoch = readInteger(payload + 16, 8);
    next.noticeSequence = readInteger(payload + 24, 8);
    next.controlEpoch = readInteger(payload + 32, 8);
    next.acceptedRevision = readInteger(payload + 40, 8);
    next.encoderAppliedRevision = readInteger(payload + 48, 8);
    next.firstSentRevision = readInteger(payload + 56, 8);
    next.firstSentFrame = readInteger(payload + 64, 8);
    if (!valid(&next)) return false;
    *notice = next;
    return true;
}
void TpsInitializeReceiver(TPS_STATUS_RECEIVER* receiver, uint64_t epoch) {
    if (receiver == NULL) return;
    memset(receiver, 0, sizeof(*receiver));
    receiver->expectedConnectionEpoch = epoch;
}
bool TpsAcceptStatus(TPS_STATUS_RECEIVER* receiver, const TPS_STATUS_NOTICE* n) {
    if (receiver == NULL || !valid(n) || !receiver->expectedConnectionEpoch || n->connectionEpoch != receiver->expectedConnectionEpoch) return false;
    if (receiver->hasNotice) {
        const TPS_STATUS_NOTICE* old = &receiver->latest;
        if (n->sessionId != old->sessionId || n->noticeSequence <= old->noticeSequence ||
            n->acceptedRevision < old->acceptedRevision || n->controlEpoch < old->controlEpoch ||
            n->encoderAppliedRevision < old->encoderAppliedRevision || n->firstSentRevision < old->firstSentRevision ||
            (n->controlEpoch == old->controlEpoch && n->controlSource != old->controlSource) ||
            (old->flags & TPS_STOPPED) ||
            (n->firstSentRevision && n->firstSentRevision == old->firstSentRevision && n->firstSentFrame != old->firstSentFrame)) return false;
    }
    receiver->latest = *n;
    receiver->hasNotice = true;
    return true;
}
bool TpsCopyStatus(const TPS_STATUS_RECEIVER* receiver, TPS_STATUS_NOTICE* notice) {
    if (receiver == NULL || notice == NULL || !receiver->hasNotice) return false;
    *notice = receiver->latest;
    return true;
}
