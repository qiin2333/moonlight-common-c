#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// These are independent Sunshine extensions inside authenticated control
// envelopes. They do not replace either direction's existing 0x5502 message.
#define TF_READY_PACKET_TYPE 0x550d
#define TF_REPORT_PACKET_TYPE 0x550e
// RTSP negotiates the video identity/parity contract independently of the
// READY/REPORT body layout. Profile 1 must fall back before ANNOUNCE: its
// parity streamPacketIndex checks cannot interoperate with correct RS symbols.
#define TF_PACKET_FEEDBACK_PROFILE_VERSION 2
#define TF_PROFILE_STRINGIFY_INTERNAL(value) #value
#define TF_PROFILE_STRINGIFY(value) TF_PROFILE_STRINGIFY_INTERNAL(value)
#define TF_PACKET_FEEDBACK_PROFILE_VERSION_STRING TF_PROFILE_STRINGIFY(TF_PACKET_FEEDBACK_PROFILE_VERSION)
#define TF_WIRE_VERSION 1
#define TF_VIDEO_FLOW 1
#define TF_MAX_PACKETS 256
#define TF_REPORT_HEADER_BYTES 52
#define TF_MAX_REPORT_BYTES (TF_REPORT_HEADER_BYTES + TF_MAX_PACKETS / 4 + TF_MAX_PACKETS * 3)
#define TF_READY_BYTES 40
#define TF_VIDEO_IDENTITY_BYTES 16
#define TF_PROBE_PADDING_HEADER_BYTES 12
#define TF_PROBE_PADDING_MAX_BYTES 255
#define TF_PROBE_PADDING_PAYLOAD_TYPE 127
#define TF_PROBE_PADDING_PROFILE_VERSION 1
#define TF_MAX_ARRIVAL_AGE_US 0xffffffu
#define TF_NO_ARRIVAL UINT64_MAX

typedef enum _TF_PACKET_STATUS {
    TF_PENDING = 0, TF_RECEIVED = 1, TF_MISSING = 2, TF_UNKNOWN = 3
} TF_PACKET_STATUS;

typedef struct _TF_PACKET_REPORT {
    uint64_t connectionEpoch;
    uint64_t reportSequence;
    uint64_t receiverClockEpoch;
    uint64_t receiverSampleTimeUs;
    uint64_t baseExtendedSequence;
    uint16_t packetCount;
    uint8_t status[TF_MAX_PACKETS];
    uint64_t firstArrivalTimeUs[TF_MAX_PACKETS];
} TF_PACKET_REPORT;

typedef struct _TF_READY {
    uint64_t connectionEpoch;
    // One past the highest successful submission, not the reserved counter.
    // Holes below it still require intersection with the successful-send ledger.
    uint64_t submittedThroughExclusive;
    uint64_t senderSampleTimeUs;
    uint16_t reportIntervalMs;
    uint16_t maxPacketsPerReport;
    uint32_t maxFeedbackWireBytesPerSecond;
} TF_READY;

// All integer fields use network byte order. Statuses are packed into 2 bits
// each, lowest pair first. Received entries alone then carry a 24-bit age in
// microseconds relative to receiverSampleTimeUs, in sequence order. Thus first
// arrival order may differ from packet sequence order without signed truncation.
// Unknown/pending/missing entries carry TF_NO_ARRIVAL in the local API.
// Failure leaves the destination untouched; zero-length return means failure.
size_t TfEncodeReport(const TF_PACKET_REPORT* report, uint8_t* output, size_t capacity);
bool TfDecodeReport(const uint8_t* payload, size_t length, TF_PACKET_REPORT* report);
size_t TfEncodeReady(const TF_READY* ready, uint8_t* output, size_t capacity);
bool TfDecodeReady(const uint8_t* payload, size_t length, TF_READY* ready);
// The identity is inside the authenticated video plaintext, before the RTP
// datagram, only after explicit RTSP negotiation. It is outside RS symbols.
size_t TfEncodeVideoIdentity(uint64_t connectionEpoch, uint64_t sequence, uint8_t* output, size_t capacity);
bool TfDecodeVideoIdentity(const uint8_t* payload, size_t length, uint64_t* connectionEpoch, uint64_t* sequence);
bool TfParseEpoch(const char* decimal, uint64_t* epoch);

// Canonical RTP padding-only packet: V2/P, no extension/CSRC/marker/media,
// negotiated PT 127, zero timestamp/SSRC/padding, and the final padding count.
// The caller must negotiate this separately, authenticate the full identity,
// and observe then discard it before RS/codec processing. No nonce is allocated
// here. RTP sequence must match the authenticated transport sequence's low bits.
// Failure leaves output/sequence untouched. Decode allows a NULL sequence.
size_t TfEncodeProbePadding(uint16_t rtpSequence, size_t paddingBytes, uint8_t* output, size_t capacity);
bool TfDecodeProbePadding(const uint8_t* payload, size_t length, uint16_t* rtpSequence);

// Diagnostic helper only; negotiated attribution uses the full video identity.
// Use an authenticated sender watermark or a previously unambiguous packet as
// the reference. A half-cycle tie, invalid 24-bit input, underflow or overflow
// is unknown rather than a guessed wrap. A stale reference beyond half a cycle
// cannot be repaired by this helper; the caller must request a new watermark.
bool TfUnwrapSequence24(uint32_t wireSequence, uint64_t reference, uint64_t* sequence);

#ifdef __cplusplus
}
#endif
