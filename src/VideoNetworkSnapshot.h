#pragma once

#include <stdbool.h>
#include <stdint.h>

#define LI_VIDEO_NETWORK_SNAPSHOT_VERSION 1

// Original, valid UDP packet observations. Holes are only missing candidates:
// intersect them with the sender's successful submissions to calculate loss.
// FEC-generated packets never enter the original-receive counters. Negotiated
// authenticated watermarks can cover an unseen tail as candidates only.
// Cumulative counters are scoped to connectionEpoch; sequenceEpoch changes
// invalidate range comparisons while retaining those cumulative counters.
typedef struct _LI_VIDEO_NETWORK_SNAPSHOT {
    uint32_t version;
    uint32_t size;
    uint64_t connectionEpoch;
    uint64_t sequenceEpoch;
    uint64_t sampleTimeUs;
    uint64_t observationDurationUs;
    uint64_t firstExtendedSequence;
    uint64_t highestExtendedSequence;
    uint64_t settledThroughExclusive;
    uint64_t uniquePackets;
    uint64_t uniqueUdpBytes;
    uint64_t duplicatePackets;
    uint64_t reorderedPackets;
    uint64_t missingCandidates;
    uint64_t latePackets;
    uint64_t settledReceivedPackets;
    uint64_t unknownCandidates;
    uint64_t coverageResets;
    uint64_t untrackedPackets;
    uint64_t authenticationFailures;
    uint64_t invalidPackets;
    uint64_t completedBlocks;
    uint64_t failedObservedBlocks;
    uint64_t recoveredDataPackets;
    uint64_t completedFrames;
    bool enabled;
    bool hasSequence;
} LI_VIDEO_NETWORK_SNAPSHOT;
