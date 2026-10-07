#include "TransportFeedbackWire.h"

#include <limits.h>
#include <string.h>

static uint64_t readInteger(const uint8_t* p, size_t bytes) {
    uint64_t value = 0;
    for (size_t i = 0; i < bytes; ++i) value = (value << 8) | p[i];
    return value;
}

static void writeInteger(uint8_t* p, uint64_t value, size_t bytes) {
    for (size_t i = bytes; i != 0; --i) {
        p[i - 1] = (uint8_t)value;
        value >>= 8;
    }
}

static size_t reportSize(const TF_PACKET_REPORT* r) {
    if (r == NULL || r->connectionEpoch == 0 || r->reportSequence == 0 ||
        r->receiverClockEpoch == 0 || r->receiverSampleTimeUs > INT64_MAX ||
        r->packetCount == 0 || r->packetCount > TF_MAX_PACKETS ||
        r->baseExtendedSequence > UINT64_MAX - (r->packetCount - 1u)) return 0;
    size_t bytes = TF_REPORT_HEADER_BYTES + (r->packetCount + 3u) / 4u;
    for (size_t i = 0; i < r->packetCount; ++i) {
        if (r->status[i] > TF_UNKNOWN) return 0;
        if (r->status[i] == TF_RECEIVED) {
            if (r->firstArrivalTimeUs[i] > r->receiverSampleTimeUs ||
                r->receiverSampleTimeUs - r->firstArrivalTimeUs[i] > TF_MAX_ARRIVAL_AGE_US) return 0;
            bytes += 3;
        }
        else if (r->firstArrivalTimeUs[i] != TF_NO_ARRIVAL) return 0;
    }
    return bytes;
}

size_t TfEncodeReport(const TF_PACKET_REPORT* r, uint8_t* output, size_t capacity) {
    const size_t bytes = reportSize(r);
    if (bytes == 0 || output == NULL || capacity < bytes) return 0;
    memset(output, 0, bytes);
    writeInteger(output, TF_WIRE_VERSION, 2);
    writeInteger(output + 2, bytes, 2);
    writeInteger(output + 4, TF_VIDEO_FLOW, 2);
    writeInteger(output + 8, r->connectionEpoch, 8);
    writeInteger(output + 16, r->reportSequence, 8);
    writeInteger(output + 24, r->receiverClockEpoch, 8);
    writeInteger(output + 32, r->receiverSampleTimeUs, 8);
    writeInteger(output + 40, r->baseExtendedSequence, 8);
    writeInteger(output + 48, r->packetCount, 2);
    size_t ageOffset = TF_REPORT_HEADER_BYTES + (r->packetCount + 3u) / 4u;
    for (size_t i = 0; i < r->packetCount; ++i) {
        output[TF_REPORT_HEADER_BYTES + i / 4] |= (uint8_t)(r->status[i] << (2 * (i % 4)));
        if (r->status[i] == TF_RECEIVED) {
            writeInteger(output + ageOffset, r->receiverSampleTimeUs - r->firstArrivalTimeUs[i], 3);
            ageOffset += 3;
        }
    }
    return bytes;
}

bool TfDecodeReport(const uint8_t* payload, size_t length, TF_PACKET_REPORT* report) {
    if (payload == NULL || report == NULL || length < TF_REPORT_HEADER_BYTES ||
        length > TF_MAX_REPORT_BYTES || readInteger(payload, 2) != TF_WIRE_VERSION ||
        readInteger(payload + 2, 2) != length || readInteger(payload + 4, 2) != TF_VIDEO_FLOW ||
        readInteger(payload + 6, 2) != 0 || readInteger(payload + 50, 2) != 0) return false;
    TF_PACKET_REPORT r;
    memset(&r, 0, sizeof(r));
    r.connectionEpoch = readInteger(payload + 8, 8);
    r.reportSequence = readInteger(payload + 16, 8);
    r.receiverClockEpoch = readInteger(payload + 24, 8);
    r.receiverSampleTimeUs = readInteger(payload + 32, 8);
    r.baseExtendedSequence = readInteger(payload + 40, 8);
    r.packetCount = (uint16_t)readInteger(payload + 48, 2);
    if (r.packetCount == 0 || r.packetCount > TF_MAX_PACKETS) return false;
    const size_t statusBytes = (r.packetCount + 3u) / 4u;
    size_t offset = TF_REPORT_HEADER_BYTES + statusBytes;
    if (offset > length) return false;
    if ((r.packetCount % 4u) && (payload[offset - 1] >> (2 * (r.packetCount % 4u))) != 0) return false;
    for (size_t i = 0; i < r.packetCount; ++i) {
        r.status[i] = (uint8_t)((payload[TF_REPORT_HEADER_BYTES + i / 4] >> (2 * (i % 4))) & 3u);
        r.firstArrivalTimeUs[i] = TF_NO_ARRIVAL;
        if (r.status[i] == TF_RECEIVED) {
            if (offset + 3 > length) return false;
            const uint64_t age = readInteger(payload + offset, 3);
            if (age > r.receiverSampleTimeUs) return false;
            r.firstArrivalTimeUs[i] = r.receiverSampleTimeUs - age;
            offset += 3;
        }
    }
    if (offset != length || reportSize(&r) != length) return false;
    *report = r;
    return true;
}

static bool validReady(const TF_READY* r) {
    return r != NULL && r->connectionEpoch != 0 && r->senderSampleTimeUs <= INT64_MAX &&
        r->reportIntervalMs >= 20 && r->reportIntervalMs <= 250 &&
        r->maxPacketsPerReport != 0 && r->maxPacketsPerReport <= TF_MAX_PACKETS &&
        r->maxFeedbackWireBytesPerSecond != 0 && r->maxFeedbackWireBytesPerSecond <= 1048576;
}

size_t TfEncodeReady(const TF_READY* r, uint8_t* output, size_t capacity) {
    if (!validReady(r) || output == NULL || capacity < TF_READY_BYTES) return 0;
    memset(output, 0, TF_READY_BYTES);
    writeInteger(output, TF_WIRE_VERSION, 2);
    writeInteger(output + 2, TF_READY_BYTES, 2);
    writeInteger(output + 4, TF_VIDEO_FLOW, 2);
    writeInteger(output + 8, r->connectionEpoch, 8);
    writeInteger(output + 16, r->submittedThroughExclusive, 8);
    writeInteger(output + 24, r->senderSampleTimeUs, 8);
    writeInteger(output + 32, r->reportIntervalMs, 2);
    writeInteger(output + 34, r->maxPacketsPerReport, 2);
    writeInteger(output + 36, r->maxFeedbackWireBytesPerSecond, 4);
    return TF_READY_BYTES;
}

bool TfDecodeReady(const uint8_t* payload, size_t length, TF_READY* ready) {
    if (payload == NULL || ready == NULL || length != TF_READY_BYTES ||
        readInteger(payload, 2) != TF_WIRE_VERSION || readInteger(payload + 2, 2) != length ||
        readInteger(payload + 4, 2) != TF_VIDEO_FLOW || readInteger(payload + 6, 2) != 0) return false;
    TF_READY r = {readInteger(payload + 8, 8), readInteger(payload + 16, 8),
        readInteger(payload + 24, 8), (uint16_t)readInteger(payload + 32, 2),
        (uint16_t)readInteger(payload + 34, 2), (uint32_t)readInteger(payload + 36, 4)};
    if (!validReady(&r)) return false;
    *ready = r;
    return true;
}

size_t TfEncodeVideoIdentity(uint64_t epoch, uint64_t sequence, uint8_t* output, size_t capacity) {
    if (!epoch || sequence == UINT64_MAX || output == NULL || capacity < TF_VIDEO_IDENTITY_BYTES) return 0;
    writeInteger(output, epoch, 8);
    writeInteger(output + 8, sequence, 8);
    return TF_VIDEO_IDENTITY_BYTES;
}

bool TfDecodeVideoIdentity(const uint8_t* payload, size_t length, uint64_t* epoch, uint64_t* sequence) {
    if (payload == NULL || epoch == NULL || sequence == NULL || length != TF_VIDEO_IDENTITY_BYTES) return false;
    const uint64_t e = readInteger(payload, 8), s = readInteger(payload + 8, 8);
    if (!e || s == UINT64_MAX) return false;
    *epoch = e; *sequence = s;
    return true;
}

bool TfParseEpoch(const char* decimal, uint64_t* epoch) {
    if (decimal == NULL || epoch == NULL || decimal[0] < '1' || decimal[0] > '9') return false;
    uint64_t value = 0;
    size_t i;
    for (i = 0; decimal[i] != 0; ++i) {
        if (i >= 20 || decimal[i] < '0' || decimal[i] > '9') return false;
        const unsigned digit = (unsigned)(decimal[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *epoch = value;
    return true;
}

size_t TfEncodeProbePadding(uint16_t rtpSequence, size_t paddingBytes, uint8_t* output, size_t capacity) {
    if (paddingBytes == 0 || paddingBytes > TF_PROBE_PADDING_MAX_BYTES || output == NULL) return 0;
    const size_t length = TF_PROBE_PADDING_HEADER_BYTES + paddingBytes;
    if (capacity < length) return 0;
    memset(output, 0, length);
    output[0] = 0xa0;
    output[1] = TF_PROBE_PADDING_PAYLOAD_TYPE;
    writeInteger(output + 2, rtpSequence, 2);
    output[length - 1] = (uint8_t)paddingBytes;
    return length;
}

bool TfDecodeProbePadding(const uint8_t* payload, size_t length, uint16_t* rtpSequence) {
    if (payload == NULL || length <= TF_PROBE_PADDING_HEADER_BYTES ||
        length > TF_PROBE_PADDING_HEADER_BYTES + TF_PROBE_PADDING_MAX_BYTES ||
        payload[0] != 0xa0 || payload[1] != TF_PROBE_PADDING_PAYLOAD_TYPE ||
        payload[length - 1] != length - TF_PROBE_PADDING_HEADER_BYTES) return false;
    for (size_t i = 4; i < length - 1; ++i) {
        if (payload[i] != 0) return false;
    }
    if (rtpSequence != NULL) *rtpSequence = (uint16_t)readInteger(payload + 2, 2);
    return true;
}
