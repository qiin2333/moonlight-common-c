#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Independently negotiated, authenticated, reliable latest-state notice.
// Complete policy values and operation identities are reconciled by paired HTTPS.
#define TPS_STATUS_PACKET_TYPE 0x550f
#define TPS_STATUS_VERSION 1
#define TPS_STATUS_BYTES 72
#define TPS_APPLIED_KNOWN 0x01
#define TPS_FIRST_SENT_KNOWN 0x02
#define TPS_PENDING 0x04
#define TPS_ENCODER_READY 0x08
#define TPS_STOPPED 0x10
#define TPS_PACKET_CONTROL 0x20
#define TPS_VIDEO_PACER 0x40

typedef struct _TPS_STATUS_NOTICE {
    uint16_t flags;
    uint8_t controlSource; // 0 legacy, 1 manual, 2 GoogCC, 3 local
    uint8_t failure; // accepted revision: 0 none, 1 unsupported, 2 backend, 3 superseded, 4 stopped
    uint32_t sessionId;
    uint64_t connectionEpoch;
    uint64_t noticeSequence;
    uint64_t controlEpoch;
    uint64_t acceptedRevision;
    uint64_t encoderAppliedRevision;
    uint64_t firstSentRevision;
    uint64_t firstSentFrame;
} TPS_STATUS_NOTICE;

typedef struct _TPS_STATUS_RECEIVER {
    uint64_t expectedConnectionEpoch;
    bool hasNotice;
    TPS_STATUS_NOTICE latest;
} TPS_STATUS_RECEIVER;

// Exact-size network byte order, zero reserved fields, no heap allocation.
// Rejection never changes the destination or receiver state.
size_t TpsEncodeStatus(const TPS_STATUS_NOTICE* notice, uint8_t* output, size_t capacity);
bool TpsDecodeStatus(const uint8_t* payload, size_t length, TPS_STATUS_NOTICE* notice);
void TpsInitializeReceiver(TPS_STATUS_RECEIVER* receiver, uint64_t expectedConnectionEpoch);
bool TpsAcceptStatus(TPS_STATUS_RECEIVER* receiver, const TPS_STATUS_NOTICE* notice);
bool TpsCopyStatus(const TPS_STATUS_RECEIVER* receiver, TPS_STATUS_NOTICE* notice);

#ifdef __cplusplus
}
#endif
