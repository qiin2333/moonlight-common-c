#include "VideoNetwork.h"

#include <stdio.h>
#include <string.h>

static int failures;
static VIDEO_NETWORK_OBSERVER observer;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void initialize(void) {
    VnInitialize(&observer, 7, 1000, 30000, 250000);
}

static void testNoTrafficOrUnseenTail(void) {
    initialize();
    VnAdvance(&observer, 1000000);
    CHECK(!observer.snapshot.hasSequence);
    CHECK(observer.snapshot.missingCandidates == 0);
    VnObserveOriginal(&observer, 100, 1400, 1000001);
    VnAdvance(&observer, 2000000);
    CHECK(observer.snapshot.missingCandidates == 0);
    CHECK(observer.snapshot.settledReceivedPackets == 1);
    CHECK(VnGetPacket(&observer, 1, 101).status == VIDEO_NETWORK_UNKNOWN);
}

static void testReorderAndDuplicate(void) {
    initialize();
    VnObserveOriginal(&observer, 100, 1400, 1001);
    VnObserveOriginal(&observer, 102, 1400, 2001);
    CHECK(VnGetPacket(&observer, 1, 101).status == VIDEO_NETWORK_PENDING);
    VnObserveOriginal(&observer, 101, 1380, 3001);
    VnObserveOriginal(&observer, 101, 1380, 4001);
    VnAdvance(&observer, 100000);
    CHECK(observer.snapshot.uniquePackets == 3);
    CHECK(observer.snapshot.uniqueUdpBytes == 4180);
    CHECK(observer.snapshot.duplicatePackets == 1);
    CHECK(observer.snapshot.reorderedPackets == 1);
    CHECK(observer.snapshot.missingCandidates == 0);
    CHECK(observer.snapshot.settledReceivedPackets == 3);
    CHECK(VnGetPacket(&observer, 1, 101).firstArrivalUs == 3001);
}

static void testMissingThenLateCorrection(void) {
    initialize();
    VnObserveOriginal(&observer, 100, 1400, 1001);
    VnObserveOriginal(&observer, 102, 1400, 2001);
    VnAdvance(&observer, 32000);
    CHECK(VnGetPacket(&observer, 1, 101).status == VIDEO_NETWORK_PENDING);
    VnAdvance(&observer, 32001);
    CHECK(observer.snapshot.missingCandidates == 1);
    CHECK(observer.snapshot.settledThroughExclusive == 103);
    CHECK(VnGetPacket(&observer, 1, 101).status == VIDEO_NETWORK_MISSING);
    VnObserveOriginal(&observer, 101, 1400, 40001);
    VnObserveOriginal(&observer, 101, 1400, 41001);
    CHECK(observer.snapshot.missingCandidates == 1);
    CHECK(observer.snapshot.latePackets == 1);
    CHECK(observer.snapshot.settledReceivedPackets == 3);
    CHECK(observer.snapshot.duplicatePackets == 1);
    CHECK(VnGetPacket(&observer, 1, 101).firstArrivalUs == 40001);
}

static void testSequenceWrap(void) {
    initialize();
    VnObserveOriginal(&observer, 65534, 1400, 1001);
    VnObserveOriginal(&observer, 0, 1400, 2001);
    VnObserveOriginal(&observer, 65535, 1400, 3001);
    VnObserveOriginal(&observer, 1, 1400, 4001);
    VnAdvance(&observer, 100000);
    CHECK(observer.snapshot.highestExtendedSequence == 65537);
    CHECK(observer.snapshot.missingCandidates == 0);
    CHECK(observer.snapshot.sequenceEpoch == 1);
    CHECK(observer.snapshot.uniquePackets == 4);
    CHECK(VnGetPacket(&observer, 1, 65535).firstArrivalUs == 3001);
}

static void testOldFrameFecTails(void) {
    // Decoder has already advanced to frame 11 when frame 10's parity tail
    // arrives. It is still a real original datagram and closes the sequence gap.
    initialize();
    VnObserveOriginal(&observer, 100, 1400, 1001);
    VnObserveOriginal(&observer, 104, 1400, 2001);
    VnObserveOriginal(&observer, 101, 1400, 3001);
    VnObserveOriginal(&observer, 102, 1400, 4001);
    VnObserveOriginal(&observer, 103, 1400, 5001);
    VnAdvance(&observer, 100000);
    CHECK(observer.snapshot.missingCandidates == 0);
    CHECK(observer.snapshot.uniquePackets == 5);
    CHECK(observer.snapshot.reorderedPackets == 3);
}

static void testSyntheticRecoveryExcluded(void) {
    initialize();
    VnObserveOriginal(&observer, 100, 1400, 1001);
    VnObserveOriginal(&observer, 102, 1400, 2001);
    // RS produces source shard 101 inside RtpVideoQueue.c. The observer is
    // attached only to the authenticated UDP receive path, not that queue.
    VnAdvance(&observer, 100000);
    CHECK(observer.snapshot.uniquePackets == 2);
    CHECK(observer.snapshot.missingCandidates == 1);
}

static void testOverflowIsUnknown(void) {
    initialize();
    VnObserveOriginal(&observer, 1, 1400, 1001);
    VnObserveOriginal(&observer, 9000, 1400, 2001);
    CHECK(observer.snapshot.sequenceEpoch == 2);
    CHECK(observer.snapshot.coverageResets == 1);
    CHECK(observer.snapshot.unknownCandidates == 8998);
    CHECK(observer.snapshot.missingCandidates == 0);
    CHECK(observer.snapshot.uniquePackets == 2);
    CHECK(VnGetPacket(&observer, 1, 1).status == VIDEO_NETWORK_UNKNOWN);
    CHECK(VnGetPacket(&observer, 2, 9000).status == VIDEO_NETWORK_RECEIVED);
}

static void testHistoryPressureDoesNotInventLoss(void) {
    initialize();
    VnObserveOriginal(&observer, 1, 1400, 1001);
    VnObserveOriginal(&observer, 3, 1400, 1002);
    for (unsigned s = 4; s < LI_VIDEO_NETWORK_HISTORY + 10; s++) {
        VnObserveOriginal(&observer, (uint16_t)s, 1400, 1002 + s);
    }
    CHECK(observer.snapshot.missingCandidates == 0);
    CHECK(observer.snapshot.unknownCandidates == 1);
    CHECK(VnGetPacket(&observer, 1, 2).status == VIDEO_NETWORK_UNKNOWN);
    VnAdvance(&observer, 100000);
    CHECK(observer.snapshot.settledReceivedPackets == observer.snapshot.uniquePackets);
}

static void testLongGapAndClockReset(void) {
    initialize();
    VnObserveOriginal(&observer, 100, 1400, 1001);
    VnObserveOriginal(&observer, 102, 1400, 2001);
    VnObserveOriginal(&observer, 150, 1400, 1000000);
    CHECK(observer.snapshot.sequenceEpoch == 2);
    CHECK(observer.snapshot.missingCandidates == 0);
    CHECK(observer.snapshot.unknownCandidates == 1);
    VnObserveOriginal(&observer, 151, 1400, 999000);
    CHECK(observer.snapshot.sequenceEpoch == 3);
    CHECK(observer.snapshot.coverageResets == 2);
    CHECK(observer.snapshot.uniquePackets == 4);
}

static void testHalfCycleIsAmbiguous(void) {
    initialize();
    VnObserveOriginal(&observer, 100, 1400, 1001);
    VnObserveOriginal(&observer, 32868, 1400, 1002);
    CHECK(observer.snapshot.sequenceEpoch == 2);
    CHECK(observer.snapshot.missingCandidates == 0);
    CHECK(observer.snapshot.unknownCandidates == 0);
}

static void testBeforeFirstAndEvictedPackets(void) {
    initialize();
    VnObserveOriginal(&observer, 5, 1400, 1001);
    VnObserveOriginal(&observer, 4, 1400, 1002);
    VnObserveOriginal(&observer, 65535, 1400, 1003);
    CHECK(observer.snapshot.uniquePackets == 1);
    CHECK(observer.snapshot.untrackedPackets == 2);
    CHECK(observer.snapshot.missingCandidates == 0);
}

static void testMultipleWrapsAnd64BitBytes(void) {
    initialize();
    const uint64_t count = 200000;
    for (uint64_t s = 0; s < count; s++) {
        VnObserveOriginal(&observer, (uint16_t)s, 65507, 1001 + s);
        VnObserveOriginal(&observer, (uint16_t)s, 65507, 1001 + s);
    }
    VnAdvance(&observer, 1000000);
    CHECK(observer.snapshot.uniquePackets == count);
    CHECK(observer.snapshot.uniqueUdpBytes == count * 65507);
    CHECK(observer.snapshot.duplicatePackets == count);
    CHECK(observer.snapshot.highestExtendedSequence == count - 1);
    CHECK(observer.snapshot.missingCandidates == 0);
    CHECK(observer.snapshot.sequenceEpoch == 1);
    CHECK(observer.snapshot.settledReceivedPackets == count);
}

static void testSnapshotAndZeroGrace(void) {
    LI_VIDEO_NETWORK_SNAPSHOT snapshot;
    VnInitialize(&observer, 9, 1000, 0, 250000);
    VnObserveOriginal(&observer, 50, 1400, 1001);
    VnAdvance(&observer, 1001);
    VnGetSnapshot(&observer, 1500, &snapshot);
    CHECK(snapshot.version == LI_VIDEO_NETWORK_SNAPSHOT_VERSION);
    CHECK(snapshot.size == sizeof(snapshot));
    CHECK(snapshot.connectionEpoch == 9);
    CHECK(snapshot.sampleTimeUs == 1500);
    CHECK(snapshot.observationDurationUs == 500);
    CHECK(snapshot.missingCandidates == 0);
    CHECK(snapshot.settledReceivedPackets == 1);
}

static void makePayload(uint8_t* payload, uint16_t sequence, uint32_t frame, uint32_t index) {
    memset(payload, 0, 32);
    payload[0] = 0x90;
    payload[2] = (uint8_t)(sequence >> 8);
    payload[3] = (uint8_t)sequence;
    for (unsigned i = 0; i < 4; i++) {
        payload[20 + i] = (uint8_t)(frame >> (i * 8));
    }
    const uint32_t fecInfo = (4U << 22) | (index << 12) | (25U << 4);
    for (unsigned i = 0; i < 4; i++) {
        payload[28 + i] = (uint8_t)(fecInfo >> (i * 8));
    }
}

static void testDatagramHeaderAndOldFrameTail(void) {
    uint8_t payload[32];
    initialize();
    makePayload(payload, 100, 10, 0);
    CHECK(VnObserveVideoPayload(&observer, payload, sizeof(payload), 1432, 1001, true));
    makePayload(payload, 105, 11, 0);
    CHECK(VnObserveVideoPayload(&observer, payload, sizeof(payload), 1432, 2001, true));
    for (unsigned i = 1; i < 5; i++) {
        makePayload(payload, (uint16_t)(100 + i), 10, i);
        CHECK(VnObserveVideoPayload(&observer, payload, sizeof(payload), 1432, 2001 + i, true));
    }
    VnAdvance(&observer, 100000);
    CHECK(observer.snapshot.uniquePackets == 6);
    CHECK(observer.snapshot.uniqueUdpBytes == 6 * 1432);
    CHECK(observer.snapshot.missingCandidates == 0);
    CHECK(observer.snapshot.invalidPackets == 0);
    CHECK(VnGetPacket(&observer, 1, 104).firstArrivalUs == 2005);
}

static void testMalformedPayloadDoesNotChangePacketRange(void) {
    uint8_t payload[32];
    initialize();
    makePayload(payload, 100, 10, 0);
    CHECK(!VnObserveVideoPayload(&observer, NULL, 32, 1400, 1001, true));
    CHECK(!VnObserveVideoPayload(&observer, payload, 31, 1400, 1001, true));
    payload[0] = 0x91;
    CHECK(!VnObserveVideoPayload(&observer, payload, 32, 1400, 1001, true));
    makePayload(payload, 100, 10, 5);
    CHECK(!VnObserveVideoPayload(&observer, payload, 32, 1400, 1001, true));
    makePayload(payload, 100, 10, 0);
    payload[27] = 0x10; // block 1 of 1 is invalid
    CHECK(!VnObserveVideoPayload(&observer, payload, 32, 1400, 1001, true));
    CHECK(observer.snapshot.invalidPackets == 5);
    CHECK(!observer.snapshot.hasSequence);
    CHECK(observer.snapshot.uniquePackets == 0);
}

static void testParityRetainsRsHeaderSymbols(void) {
    uint8_t payload[32];
    uint16_t rtp;
    uint32_t sequence24;
    makePayload(payload, 100, 1, 0);
    payload[16] = 0; payload[17] = 100;
    CHECK(VnParseVideoPayload(payload, sizeof(payload), true, &rtp, &sequence24));
    CHECK(rtp == 100 && sequence24 == 100);
    makePayload(payload, 104, 1, 4);
    memset(payload + 16, 0xa5, 4); // Actual parity symbols need not match RTP.
    CHECK(VnParseVideoPayload(payload, sizeof(payload), true, &rtp, &sequence24));
    CHECK(rtp == 104 && sequence24 == UINT32_MAX);
}

static void testDeterministicDropReplay(void) {
    // The injection ledger is independent of observer state. Sequence zero
    // and the last sequence are always delivered, so every injected gap has
    // an observed upper bound. Byte counts include received originals once.
    const unsigned count = 150000;
    unsigned dropped = 0;
    unsigned duplicated = 0;
    initialize();
    for (unsigned s = 0; s < count; s++) {
        if (s % 97 == 5 || (s % 7919 >= 100 && s % 7919 < 120)) {
            dropped++;
            continue;
        }
        VnObserveOriginal(&observer, (uint16_t)s, 1400, 1001 + (uint64_t)s * 100);
        if (s % 64 == 0) {
            VnObserveOriginal(&observer, (uint16_t)s, 1400, 1001 + (uint64_t)s * 100);
            duplicated++;
        }
    }
    VnAdvance(&observer, 1001 + (uint64_t)count * 100 + 30000);
    CHECK(observer.snapshot.uniquePackets == count - dropped);
    CHECK(observer.snapshot.uniqueUdpBytes == (uint64_t)(count - dropped) * 1400);
    CHECK(observer.snapshot.missingCandidates == dropped);
    CHECK(observer.snapshot.duplicatePackets == duplicated);
    CHECK(observer.snapshot.highestExtendedSequence == count - 1);
    CHECK(observer.snapshot.unknownCandidates == 0);
    CHECK(observer.snapshot.coverageResets == 0);
}

int main(void) {
    testNoTrafficOrUnseenTail();
    testReorderAndDuplicate();
    testMissingThenLateCorrection();
    testSequenceWrap();
    testOldFrameFecTails();
    testSyntheticRecoveryExcluded();
    testOverflowIsUnknown();
    testHistoryPressureDoesNotInventLoss();
    testLongGapAndClockReset();
    testHalfCycleIsAmbiguous();
    testBeforeFirstAndEvictedPackets();
    testMultipleWrapsAnd64BitBytes();
    testSnapshotAndZeroGrace();
    testDatagramHeaderAndOldFrameTail();
    testMalformedPayloadDoesNotChangePacketRange();
    testParityRetainsRsHeaderSymbols();
    testDeterministicDropReplay();
    printf("Video network observer: 17 scenarios, %d failures\n", failures);
    return failures != 0;
}
