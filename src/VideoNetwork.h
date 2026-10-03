#pragma once

#include <stddef.h>
#include "VideoNetworkSnapshot.h"

#define LI_VIDEO_NETWORK_HISTORY 8192

#ifdef __cplusplus
extern "C" {
#endif

typedef enum _VIDEO_NETWORK_PACKET_STATUS {
    VIDEO_NETWORK_PENDING = 0,
    VIDEO_NETWORK_RECEIVED = 1,
    VIDEO_NETWORK_MISSING = 2,
    VIDEO_NETWORK_UNKNOWN = 3
} VIDEO_NETWORK_PACKET_STATUS;

typedef struct _VIDEO_NETWORK_PACKET_RECORD {
    uint64_t extendedSequence;
    uint64_t firstArrivalUs;
    uint32_t udpBytes;
    VIDEO_NETWORK_PACKET_STATUS status;
} VIDEO_NETWORK_PACKET_RECORD;

typedef struct _VIDEO_NETWORK_SLOT {
    uint64_t arrivalUs;
    uint64_t observedUs;
    uint32_t udpBytes;
    uint8_t status;
    uint8_t feedbackDirty;
    uint8_t feedbackRepeats;
} VIDEO_NETWORK_SLOT;

// Single owner. The stream wrapper serializes readers and publication.
typedef struct _VIDEO_NETWORK_OBSERVER {
    LI_VIDEO_NETWORK_SNAPSHOT snapshot;
    VIDEO_NETWORK_SLOT slots[LI_VIDEO_NETWORK_HISTORY];
    uint64_t historyStart;
    uint64_t startUs;
    uint64_t lastArrivalUs;
    uint64_t reorderGraceUs;
    uint64_t maximumSilenceUs;
    uint64_t highestOriginalSequence;
    bool hasOriginalSequence;
} VIDEO_NETWORK_OBSERVER;

void VnInitialize(VIDEO_NETWORK_OBSERVER* observer, uint64_t connectionEpoch,
                  uint64_t nowUs, uint64_t reorderGraceUs, uint64_t maximumSilenceUs);
void VnObserveOriginal(VIDEO_NETWORK_OBSERVER* observer, uint16_t sequence,
                       uint32_t udpBytes, uint64_t arrivalUs);
// Caller supplies an already unambiguous server transport identity. Coverage
// contains candidates only, to be intersected with successful submissions.
void VnCoverSubmittedThrough(VIDEO_NETWORK_OBSERVER* observer, uint64_t throughExclusive, uint64_t nowUs);
void VnObserveExtended(VIDEO_NETWORK_OBSERVER* observer, uint64_t sequence, uint32_t udpBytes, uint64_t arrivalUs);
bool VnParseVideoPayload(const uint8_t* payload, size_t length, bool multiFecCapable,
                        uint16_t* rtpSequence, uint32_t* transportSequence24);
// transportSequence24 is UINT32_MAX for a parity shard: its streamPacketIndex
// contains RS symbols. Its authenticated full identity and RTP sequence still
// identify the transmitted packet; the sentinel can never be a 24-bit value.
// Plaintext RTP datagram, after successful authentication (when encrypted),
// still in wire byte order. Parses only the existing video format.
bool VnObserveVideoPayload(VIDEO_NETWORK_OBSERVER* observer, const uint8_t* payload,
                           size_t length, uint32_t udpBytes, uint64_t arrivalUs,
                           bool multiFecCapable);
void VnAdvance(VIDEO_NETWORK_OBSERVER* observer, uint64_t nowUs);
void VnGetSnapshot(const VIDEO_NETWORK_OBSERVER* observer, uint64_t nowUs,
                   LI_VIDEO_NETWORK_SNAPSHOT* snapshot);
// A range is valid only in its sequence epoch. Evicted or unobserved entries
// return UNKNOWN, including the unsent/unknown tail beyond the highest packet.
VIDEO_NETWORK_PACKET_RECORD VnGetPacket(const VIDEO_NETWORK_OBSERVER* observer,
                                        uint64_t sequenceEpoch, uint64_t extendedSequence);

#ifdef __cplusplus
}
#endif
