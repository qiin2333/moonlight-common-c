#include "TransportPolicyStatus.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); ++failures; } } while (0)

static const uint8_t golden[TPS_STATUS_BYTES] = {
    0,1,0,72,0,11,1,0, 0,0,0,9,0,0,0,0,
    128,0,0,0,0,0,0,1, 0,0,0,0,0,0,0,2,
    0,0,0,0,0,0,0,3, 0,0,0,0,0,0,0,7,
    0,0,0,0,0,0,0,6, 0,0,0,0,0,0,0,5,
    255,255,255,255,255,255,255,255
};

static TPS_STATUS_NOTICE decodeGolden(void) {
    TPS_STATUS_NOTICE n = {0};
    CHECK(TpsDecodeStatus(golden, sizeof(golden), &n));
    CHECK(n.flags == 11 && n.sessionId == 9 && n.controlSource == 1);
    CHECK(n.connectionEpoch == UINT64_C(0x8000000000000001));
    CHECK(n.noticeSequence == 2 && n.controlEpoch == 3 && n.acceptedRevision == 7);
    CHECK(n.encoderAppliedRevision == 6 && n.firstSentRevision == 5 && n.firstSentFrame == UINT64_MAX);
    uint8_t encoded[TPS_STATUS_BYTES];
    CHECK(TpsEncodeStatus(&n, encoded, sizeof(encoded)) == sizeof(golden));
    CHECK(memcmp(encoded, golden, sizeof(golden)) == 0);
    return n;
}

static void malformed(void) {
    TPS_STATUS_NOTICE sentinel, destination;
    memset(&sentinel, 0xa5, sizeof(sentinel));
    for (size_t length = 0; length < sizeof(golden); ++length) {
        destination = sentinel;
        CHECK(!TpsDecodeStatus(golden, length, &destination));
        CHECK(memcmp(&destination, &sentinel, sizeof(sentinel)) == 0);
    }
    uint8_t bad[TPS_STATUS_BYTES + 1];
    memcpy(bad, golden, sizeof(golden)); bad[TPS_STATUS_BYTES] = 0;
    destination = sentinel;
    CHECK(!TpsDecodeStatus(bad, sizeof(bad), &destination));
    CHECK(memcmp(&destination, &sentinel, sizeof(sentinel)) == 0);
    const size_t offsets[] = {1, 3, 5, 6, 7, 12};
    const uint8_t values[] = {2, 71, 128, 4, 5, 1};
    for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); ++i) {
        memcpy(bad, golden, sizeof(golden)); bad[offsets[i]] = values[i];
        destination = sentinel;
        CHECK(!TpsDecodeStatus(bad, sizeof(golden), &destination));
        CHECK(memcmp(&destination, &sentinel, sizeof(sentinel)) == 0);
    }
    TPS_STATUS_NOTICE invalid = decodeGolden(); invalid.acceptedRevision = 4;
    memset(bad, 0xa5, sizeof(bad));
    CHECK(TpsEncodeStatus(&invalid, bad, sizeof(bad)) == 0);
    for (size_t i = 0; i < sizeof(bad); ++i) CHECK(bad[i] == 0xa5);
}

static void lifecycle(void) {
    TPS_STATUS_NOTICE n = decodeGolden(), copy;
    TPS_STATUS_RECEIVER receiver;
    TpsInitializeReceiver(&receiver, 0);
    CHECK(!TpsAcceptStatus(&receiver, &n));
    TpsInitializeReceiver(&receiver, n.connectionEpoch);
    CHECK(!TpsCopyStatus(&receiver, &copy));
    CHECK(TpsAcceptStatus(&receiver, &n));
    CHECK(!TpsAcceptStatus(&receiver, &n));
    ++n.noticeSequence; n.flags &= ~TPS_ENCODER_READY;
    CHECK(TpsAcceptStatus(&receiver, &n)); // SDK rebuilding can lower readiness.
    ++n.noticeSequence; n.flags |= TPS_ENCODER_READY;
    CHECK(TpsAcceptStatus(&receiver, &n));
    const TPS_STATUS_RECEIVER before = receiver;
    ++n.noticeSequence; --n.acceptedRevision;
    CHECK(!TpsAcceptStatus(&receiver, &n));
    CHECK(memcmp(&receiver, &before, sizeof(before)) == 0);
    n = before.latest; ++n.noticeSequence; n.firstSentFrame = 0;
    CHECK(!TpsAcceptStatus(&receiver, &n)); // Same sent revision has immutable frame identity.
    n = before.latest; ++n.noticeSequence; n.controlSource = 2;
    CHECK(!TpsAcceptStatus(&receiver, &n));
    n = before.latest; ++n.noticeSequence; ++n.controlEpoch; n.controlSource = 2;
    CHECK(TpsAcceptStatus(&receiver, &n));
    ++n.noticeSequence; n.flags = TPS_APPLIED_KNOWN | TPS_FIRST_SENT_KNOWN | TPS_STOPPED;
    CHECK(TpsAcceptStatus(&receiver, &n));
    ++n.noticeSequence; n.flags = TPS_APPLIED_KNOWN | TPS_FIRST_SENT_KNOWN | TPS_ENCODER_READY;
    CHECK(!TpsAcceptStatus(&receiver, &n));
    TpsInitializeReceiver(&receiver, n.connectionEpoch + 1);
    CHECK(!TpsCopyStatus(&receiver, &copy));
    CHECK(!TpsAcceptStatus(&receiver, &n));
    TpsInitializeReceiver(&receiver, n.connectionEpoch);
    n.noticeSequence = UINT64_MAX;
    CHECK(TpsAcceptStatus(&receiver, &n));
    n.noticeSequence = 0;
    CHECK(!TpsAcceptStatus(&receiver, &n)); // Never wrap into another ordering epoch.
}

int main(void) {
    (void)decodeGolden(); malformed(); lifecycle();
    if (!failures) puts("Policy status golden wire, atomic rejection and lifetime checks passed");
    return failures ? 1 : 0;
}
