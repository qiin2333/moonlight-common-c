#include "VideoPacketFeedback.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #condition); failures++; } } while (0)
static VIDEO_NETWORK_OBSERVER observer;
static VIDEO_PACKET_FEEDBACK feedback;

static TF_READY ready(uint64_t through, uint64_t time) {
    TF_READY r = {42, through, time, 50, 256, 125000};
    return r;
}
static void initialize(void) {
    VnInitialize(&observer, 42, 1000, 30000, 0);
    VfInitialize(&feedback, 42);
}
static TF_PACKET_REPORT prepare(uint64_t now) {
    TF_PACKET_REPORT r;
    memset(&r, 0, sizeof(r));
    CHECK(VfPrepareReport(&feedback, &observer, now, &r));
    uint8_t bytes[TF_MAX_REPORT_BYTES];
    const size_t size = TfEncodeReport(&r, bytes, sizeof(bytes));
    CHECK(size != 0);
    TF_PACKET_REPORT decoded;
    CHECK(TfDecodeReport(bytes, size, &decoded));
    CHECK(decoded.reportSequence == r.reportSequence);
    return r;
}

static void negotiation(void) {
    initialize();
    TF_PACKET_REPORT r;
    memset(&r, 0xa5, sizeof(r));
    const TF_PACKET_REPORT before = r;
    CHECK(!VfPrepareReport(&feedback, &observer, 1000, &r));
    CHECK(memcmp(&r, &before, sizeof(r)) == 0);
    TF_READY offer = ready(1, 1000);
    offer.connectionEpoch = 43;
    CHECK(!VfAcceptReady(&feedback, &observer, &offer, 1000));
    offer.connectionEpoch = 42;
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1000));
    CHECK(!VfAcceptReady(&feedback, &observer, &offer, 1001));
    offer.senderSampleTimeUs++;
    CHECK(!VfAcceptReady(&feedback, &observer, &offer, 999));
    offer.reportIntervalMs++;
    CHECK(!VfAcceptReady(&feedback, &observer, &offer, 1001));
    CHECK(!VfObserveAuthenticated(&feedback, &observer, 43, 0, 1200, 1001));
    CHECK(!VfObserveAuthenticated(&feedback, &observer, 42, UINT64_MAX, 1200, 1001));
    CHECK(observer.snapshot.uniquePackets == 0);
}

static void wholeLostFrameAndTail(void) {
    initialize();
    const TF_READY offer = ready(40, 1000);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1000));
    TF_PACKET_REPORT r = prepare(30999);
    CHECK(r.packetCount == 40 && r.status[39] == TF_PENDING);
    VfCommitQueued(&feedback, &observer, &r);
    r = prepare(31000);
    for (unsigned i = 0; i < 40; ++i) CHECK(r.status[i] == TF_MISSING);
    CHECK(observer.snapshot.uniquePackets == 0 && observer.snapshot.missingCandidates == 40);
}

static void lateCommitRace(void) {
    initialize();
    const TF_READY offer = ready(1, 1000);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1000));
    TF_PACKET_REPORT old = prepare(31000);
    CHECK(old.status[0] == TF_MISSING);
    CHECK(VfObserveAuthenticated(&feedback, &observer, 42, 0, 1200, 32000));
    VfCommitQueued(&feedback, &observer, &old);
    CHECK(observer.slots[0].feedbackDirty && observer.slots[0].feedbackRepeats == 2);
    TF_PACKET_REPORT corrected = prepare(32000);
    CHECK(corrected.status[0] == TF_RECEIVED && corrected.firstArrivalTimeUs[0] == 32000);
    VfCommitQueued(&feedback, &observer, &corrected);
    CHECK(observer.slots[0].feedbackRepeats == 1);
    VfCommitQueued(&feedback, &observer, &corrected);
    CHECK(observer.slots[0].feedbackRepeats == 1);
    CHECK(observer.snapshot.latePackets == 1);
}

static void lostFeedbackAndImmutableArrival(void) {
    initialize();
    CHECK(VfObserveAuthenticated(&feedback, &observer, 42, 0, 1200, 1000));
    const TF_READY offer = ready(1, 1001);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1001));
    TF_PACKET_REPORT first = prepare(2000);
    // Queue success doesn't claim network delivery. One best-effort repeat remains.
    VfCommitQueued(&feedback, &observer, &first);
    CHECK(VfObserveAuthenticated(&feedback, &observer, 42, 0, 1200, 2001));
    TF_PACKET_REPORT second = prepare(2002);
    CHECK(first.firstArrivalTimeUs[0] == 1000 && second.firstArrivalTimeUs[0] == 1000);
    CHECK(second.reportSequence > first.reportSequence);
    VfCommitQueued(&feedback, &observer, &second);
    CHECK(!VfPrepareReport(&feedback, &observer, 2003, &second));
    CHECK(observer.snapshot.uniquePackets == 1 && observer.snapshot.duplicatePackets == 1);
}

static void fullIdentityAcrossManyWraps(void) {
    initialize();
    const uint64_t current = (1ull << 48) + (1ull << 24) * 3 + 7;
    CHECK(VfObserveAuthenticated(&feedback, &observer, 42, current, 1200, 1000));
    CHECK(VfObserveAuthenticated(&feedback, &observer, 42, current - (1ull << 24) * 2, 1200, 1001));
    CHECK(observer.snapshot.uniquePackets == 1 && observer.snapshot.untrackedPackets == 1);
    CHECK(observer.snapshot.duplicatePackets == 0 && observer.snapshot.reorderedPackets == 0);
    CHECK(VnGetPacket(&observer, observer.snapshot.sequenceEpoch, current).firstArrivalUs == 1000);
    const TF_READY offer = ready(current + 1, 1001);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1001));
    CHECK(observer.snapshot.highestExtendedSequence == current);
}

static void historyPressureIsUnknown(void) {
    initialize();
    TF_READY offer = ready(100, 1000);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1000));
    offer = ready(30000, 1001);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1001));
    CHECK(observer.snapshot.coverageResets == 1 && observer.snapshot.sequenceEpoch == 2);
    CHECK(observer.snapshot.missingCandidates == 0 && observer.snapshot.unknownCandidates == 30000 - LI_VIDEO_NETWORK_HISTORY);
    CHECK(observer.historyStart == 30000 - LI_VIDEO_NETWORK_HISTORY);
    CHECK(VnGetPacket(&observer, 1, 0).status == VIDEO_NETWORK_UNKNOWN);
}

static void timeoutAndExhaustion(void) {
    initialize();
    TF_READY offer = ready(1, 1000);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1000));
    TF_PACKET_REPORT r;
    CHECK(!VfPrepareReport(&feedback, &observer, 1001000, &r));
    offer = ready(1, 1002);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1001001));
    CHECK(VfPrepareReport(&feedback, &observer, 1001001, &r));
    feedback.nextReportSequence = UINT64_MAX;
    CHECK(!VfPrepareReport(&feedback, &observer, 1001002, &r));
    CHECK(feedback.exhausted);
}

static void oldArrivalAndMaximumSequence(void) {
    initialize();
    const uint64_t last = UINT64_MAX - 1;
    CHECK(VfObserveAuthenticated(&feedback, &observer, 42, last, 1200, 1000));
    const TF_READY offer = ready(UINT64_MAX, 20000000);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 20000000));
    TF_PACKET_REPORT r;
    bool reached = false;
    for (unsigned cycle = 0; cycle < 6 && !reached; ++cycle) {
        for (unsigned n = 0; n < 8 && VfPrepareReport(&feedback, &observer, 20000000 + cycle * 50000, &r); ++n) {
            if (r.baseExtendedSequence + r.packetCount - 1 == last) {
                CHECK(r.status[r.packetCount - 1] == TF_UNKNOWN && r.firstArrivalTimeUs[r.packetCount - 1] == TF_NO_ARRIVAL);
                reached = true;
            }
            VfCommitQueued(&feedback, &observer, &r);
        }
    }
    CHECK(reached);
}

static void feedbackBudgetAndPeriod(void) {
    initialize();
    const TF_READY offer = ready(LI_VIDEO_NETWORK_HISTORY, 1000);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1000));
    uint64_t charged = 0;
    unsigned reports = 0;
    TF_PACKET_REPORT r;
    for (; VfPrepareReport(&feedback, &observer, 1000, &r); ++reports) {
        uint8_t bytes[TF_MAX_REPORT_BYTES];
        charged += TfEncodeReport(&r, bytes, sizeof(bytes)) + VF_WIRE_OVERHEAD_BOUND;
        VfCommitQueued(&feedback, &observer, &r);
    }
    CHECK(reports == 8 && charged <= VF_FEEDBACK_BURST_BYTES);
    CHECK(!VfPrepareReport(&feedback, &observer, 50999, &r));
    CHECK(VfPrepareReport(&feedback, &observer, 51000, &r));
    // A failed queue attempt retains transitions but still reserves identity/tokens.
    const uint64_t sequence = r.reportSequence;
    CHECK(VfPrepareReport(&feedback, &observer, 51000, &r));
    CHECK(r.reportSequence == sequence + 1);
}

static void watermarkDoesNotInventReordering(void) {
    initialize();
    const TF_READY offer = ready(100, 1000);
    CHECK(VfAcceptReady(&feedback, &observer, &offer, 1000));
    CHECK(VfObserveAuthenticated(&feedback, &observer, 42, 50, 1200, 1001));
    CHECK(VfObserveAuthenticated(&feedback, &observer, 42, 51, 1200, 1002));
    CHECK(observer.snapshot.reorderedPackets == 0);
    CHECK(VfObserveAuthenticated(&feedback, &observer, 42, 49, 1200, 1003));
    CHECK(observer.snapshot.reorderedPackets == 1);
}

int main(void) {
    negotiation(); wholeLostFrameAndTail(); lateCommitRace(); lostFeedbackAndImmutableArrival();
    fullIdentityAcrossManyWraps(); historyPressureIsUnknown(); timeoutAndExhaustion();
    oldArrivalAndMaximumSequence(); feedbackBudgetAndPeriod(); watermarkDoesNotInventReordering();
    printf("Video packet feedback: 10 scenarios, %d failures\n", failures);
    return failures != 0;
}
