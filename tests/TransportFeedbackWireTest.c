#include "TransportFeedbackWire.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void golden(void) {
    const uint8_t bytes[] = {0,1,0,56,0,1,0,0,1,2,3,4,5,6,7,8,
        0,0,0,0,0,0,0,9,0,0,0,0,0,0,0,11,0,0,0,0,1,0,0,2,
        255,255,255,255,255,255,255,252,0,4,0,0,0xe4,0,0,10};
    TF_PACKET_REPORT r;
    CHECK(TfDecodeReport(bytes, sizeof(bytes), &r));
    CHECK(r.connectionEpoch == 0x0102030405060708ull);
    CHECK(r.reportSequence == 9 && r.receiverClockEpoch == 11);
    CHECK(r.receiverSampleTimeUs == 0x01000002 && r.baseExtendedSequence == UINT64_MAX - 3);
    CHECK(r.packetCount == 4 && r.status[0] == TF_PENDING && r.status[1] == TF_RECEIVED);
    CHECK(r.status[2] == TF_MISSING && r.status[3] == TF_UNKNOWN);
    CHECK(r.firstArrivalTimeUs[0] == TF_NO_ARRIVAL && r.firstArrivalTimeUs[1] == 0x00fffff8);
    uint8_t out[TF_MAX_REPORT_BYTES];
    CHECK(TfEncodeReport(&r, out, sizeof(out)) == sizeof(bytes));
    CHECK(memcmp(out, bytes, sizeof(bytes)) == 0);
}

static void allSizes(void) {
    for (unsigned n = 1; n <= TF_MAX_PACKETS; ++n) {
        TF_PACKET_REPORT r;
        memset(&r, 0, sizeof(r));
        r.connectionEpoch = r.reportSequence = r.receiverClockEpoch = UINT64_MAX;
        r.receiverSampleTimeUs = INT64_MAX;
        r.baseExtendedSequence = UINT64_MAX - n + 1;
        r.packetCount = (uint16_t)n;
        for (unsigned i = 0; i < n; ++i) {
            r.status[i] = TF_RECEIVED;
            r.firstArrivalTimeUs[i] = INT64_MAX - (i % 2 ? 0 : TF_MAX_ARRIVAL_AGE_US);
        }
        uint8_t bytes[TF_MAX_REPORT_BYTES];
        const size_t size = TfEncodeReport(&r, bytes, sizeof(bytes));
        CHECK(size == TF_REPORT_HEADER_BYTES + (n + 3) / 4 + n * 3);
        TF_PACKET_REPORT decoded;
        CHECK(TfDecodeReport(bytes, size, &decoded));
        CHECK(decoded.baseExtendedSequence == r.baseExtendedSequence);
        for (unsigned i = 0; i < n; ++i) CHECK(decoded.firstArrivalTimeUs[i] == r.firstArrivalTimeUs[i]);
    }
}

static void atomicFailure(void) {
    TF_PACKET_REPORT r;
    memset(&r, 0xa5, sizeof(r));
    const TF_PACKET_REPORT original = r;
    uint8_t bytes[TF_MAX_REPORT_BYTES] = {0};
    for (size_t n = 0; n <= sizeof(bytes); ++n) {
        CHECK(!TfDecodeReport(bytes, n, &r));
        CHECK(memcmp(&r, &original, sizeof(r)) == 0);
    }
    memset(bytes, 0xab, sizeof(bytes));
    CHECK(TfEncodeReport(&original, bytes, sizeof(bytes)) == 0);
    for (size_t i = 0; i < sizeof(bytes); ++i) CHECK(bytes[i] == 0xab);
}

static void readyAndWrap(void) {
    TF_READY r = {UINT64_MAX, UINT64_MAX, INT64_MAX, 50, 256, 125000};
    uint8_t bytes[TF_READY_BYTES];
    CHECK(TfEncodeReady(&r, bytes, sizeof(bytes)) == sizeof(bytes));
    TF_READY decoded;
    CHECK(TfDecodeReady(bytes, sizeof(bytes), &decoded));
    CHECK(decoded.connectionEpoch == UINT64_MAX && decoded.submittedThroughExclusive == UINT64_MAX);
    CHECK(decoded.senderSampleTimeUs == INT64_MAX && decoded.maxFeedbackWireBytesPerSecond == 125000);
    uint64_t sequence = 123;
    CHECK(TfUnwrapSequence24(2, 0xfffffe, &sequence) && sequence == 0x1000002);
    CHECK(TfUnwrapSequence24(0xfffffe, 0x1000002, &sequence) && sequence == 0xfffffe);
    CHECK(!TfUnwrapSequence24(0x800000, 0, &sequence) && sequence == 0xfffffe);
    CHECK(!TfUnwrapSequence24(0, UINT64_MAX, &sequence));
    CHECK(!TfUnwrapSequence24(0xffffff, 0, &sequence));
}

static void mutations(void) {
    TF_PACKET_REPORT r;
    memset(&r, 0, sizeof(r));
    r.connectionEpoch = 42; r.reportSequence = r.receiverClockEpoch = 1;
    r.receiverSampleTimeUs = 100000; r.packetCount = 256;
    for (unsigned i = 0; i < 256; ++i) {
        r.status[i] = (uint8_t)(i % 4);
        r.firstArrivalTimeUs[i] = r.status[i] == TF_RECEIVED ? 99999 - i : TF_NO_ARRIVAL;
    }
    uint8_t valid[TF_MAX_REPORT_BYTES];
    const size_t size = TfEncodeReport(&r, valid, sizeof(valid));
    uint32_t random = 0x10203040;
    for (unsigned n = 0; n < 32768; ++n) {
        uint8_t bytes[TF_MAX_REPORT_BYTES];
        memcpy(bytes, valid, size);
        random = random * 1664525u + 1013904223u;
        bytes[random % size] ^= (uint8_t)(1u << ((random >> 24) & 7));
        TF_PACKET_REPORT decoded;
        if (TfDecodeReport(bytes, size, &decoded)) {
            uint8_t canonical[TF_MAX_REPORT_BYTES];
            CHECK(TfEncodeReport(&decoded, canonical, sizeof(canonical)) == size);
            CHECK(memcmp(bytes, canonical, size) == 0);
        }
    }
}

static void videoIdentityAndEpoch(void) {
    uint8_t bytes[TF_VIDEO_IDENTITY_BYTES];
    const uint8_t golden[16] = {1,2,3,4,5,6,7,8, 0,0,0,3,0,0,0,7};
    CHECK(TfEncodeVideoIdentity(0x0102030405060708ull, 0x300000007ull, bytes, sizeof(bytes)) == 16);
    CHECK(memcmp(bytes, golden, 16) == 0);
    uint64_t epoch = 0, sequence = 0;
    CHECK(TfDecodeVideoIdentity(bytes, sizeof(bytes), &epoch, &sequence));
    CHECK(epoch == 0x0102030405060708ull && sequence == 0x300000007ull);
    CHECK(!TfDecodeVideoIdentity(bytes, 15, &epoch, &sequence));
    CHECK(epoch == 0x0102030405060708ull && sequence == 0x300000007ull);
    CHECK(!TfEncodeVideoIdentity(0, 0, bytes, sizeof(bytes)));
    CHECK(!TfEncodeVideoIdentity(42, UINT64_MAX, bytes, sizeof(bytes)));
    CHECK(TfEncodeVideoIdentity(UINT64_MAX, UINT64_MAX - 1, bytes, sizeof(bytes)) == 16);
    CHECK(TfDecodeVideoIdentity(bytes, sizeof(bytes), &epoch, &sequence));
    CHECK(epoch == UINT64_MAX && sequence == UINT64_MAX - 1);
    CHECK(TfParseEpoch("18446744073709551615", &epoch) && epoch == UINT64_MAX);
    const char* invalid[] = {"", "0", "01", "-1", "+1", " 1", "1 ", "1x", "18446744073709551616", "111111111111111111111"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        CHECK(!TfParseEpoch(invalid[i], &epoch));
        CHECK(epoch == UINT64_MAX);
    }
}

int main(void) {
    golden(); allSizes(); atomicFailure(); readyAndWrap(); mutations(); videoIdentityAndEpoch();
    printf("Transport feedback wire: 6 scenarios, %d failures\n", failures);
    return failures != 0;
}
