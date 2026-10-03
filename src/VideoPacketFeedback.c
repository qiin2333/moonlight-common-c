#include "VideoPacketFeedback.h"

#include <limits.h>
#include <string.h>

void VfInitialize(VIDEO_PACKET_FEEDBACK* state, uint64_t expectedEpoch) {
    memset(state, 0, sizeof(*state));
    state->expectedConnectionEpoch = expectedEpoch;
    state->nextReportSequence = 1;
    state->tokens = VF_FEEDBACK_BURST_BYTES;
}

bool VfAcceptReady(VIDEO_PACKET_FEEDBACK* state, VIDEO_NETWORK_OBSERVER* observer,
                   const TF_READY* ready, uint64_t nowUs) {
    uint8_t check[TF_READY_BYTES];
    if (ready == NULL || state->exhausted || !state->expectedConnectionEpoch || nowUs > INT64_MAX ||
        ready->connectionEpoch != state->expectedConnectionEpoch ||
        observer->snapshot.connectionEpoch != state->expectedConnectionEpoch ||
        !TfEncodeReady(ready, check, sizeof(check))) return false;
    if (state->active && (ready->senderSampleTimeUs <= state->ready.senderSampleTimeUs ||
        ready->submittedThroughExclusive < state->ready.submittedThroughExclusive ||
        nowUs < state->lastReadyUs || ready->reportIntervalMs != state->ready.reportIntervalMs ||
        ready->maxPacketsPerReport != state->ready.maxPacketsPerReport ||
        ready->maxFeedbackWireBytesPerSecond != state->ready.maxFeedbackWireBytesPerSecond)) return false;
    state->ready = *ready;
    state->lastReadyUs = nowUs;
    state->active = true;
    VnCoverSubmittedThrough(observer, ready->submittedThroughExclusive, nowUs);
    return true;
}

bool VfObserveAuthenticated(VIDEO_PACKET_FEEDBACK* state, VIDEO_NETWORK_OBSERVER* observer,
                            uint64_t epoch, uint64_t sequence, uint32_t udpBytes, uint64_t arrivalUs) {
    if (!state->expectedConnectionEpoch || epoch != state->expectedConnectionEpoch ||
        observer->snapshot.connectionEpoch != epoch || sequence == UINT64_MAX || arrivalUs > INT64_MAX) return false;
    VnObserveExtended(observer, sequence, udpBytes, arrivalUs);
    return true;
}

bool VfPrepareReport(VIDEO_PACKET_FEEDBACK* state, VIDEO_NETWORK_OBSERVER* observer,
                     uint64_t nowUs, TF_PACKET_REPORT* report) {
    if (report == NULL || !state->active || state->exhausted || nowUs > INT64_MAX ||
        nowUs < state->lastReadyUs || nowUs - state->lastReadyUs >= VF_READY_TIMEOUT_US ||
        nowUs < state->tokenTimeUs || observer->snapshot.connectionEpoch != state->expectedConnectionEpoch) return false;
    VnAdvance(observer, nowUs);
    if (!observer->snapshot.hasSequence) return false;
    if (nowUs >= state->nextCycleUs) {
        state->nextCycleUs = nowUs + (uint64_t)state->ready.reportIntervalMs * 1000;
        state->reportsInCycle = 0;
    }
    if (state->reportsInCycle >= 8) return false;
    const uint64_t elapsed = nowUs - state->tokenTimeUs;
    const uint64_t refill = elapsed >= 1000000 ? state->ready.maxFeedbackWireBytesPerSecond :
        elapsed * state->ready.maxFeedbackWireBytesPerSecond / 1000000;
    const uint64_t available = state->tokens + refill;
    state->tokens = available < VF_FEEDBACK_BURST_BYTES ? (uint32_t)available : VF_FEEDBACK_BURST_BYTES;
    state->tokenTimeUs = nowUs;
    uint64_t first = 0;
    bool found = false;
    // New state transitions take priority over the second best-effort copy.
    for (unsigned pass = 0; pass < 2 && !found; ++pass) {
        for (uint64_t s = observer->historyStart; s <= observer->snapshot.highestExtendedSequence; ++s) {
            const VIDEO_NETWORK_SLOT* slot = &observer->slots[s % LI_VIDEO_NETWORK_HISTORY];
            if (pass == 0 ? slot->feedbackDirty : slot->feedbackRepeats) { first = s; found = true; break; }
        }
    }
    if (!found) return false;
    if (state->nextReportSequence == UINT64_MAX) { state->exhausted = true; return false; }
    TF_PACKET_REPORT r;
    memset(&r, 0, sizeof(r));
    r.connectionEpoch = state->expectedConnectionEpoch;
    r.receiverClockEpoch = observer->snapshot.sequenceEpoch;
    r.receiverSampleTimeUs = nowUs;
    r.baseExtendedSequence = first;
    const uint64_t count = observer->snapshot.highestExtendedSequence - first + 1;
    r.packetCount = (uint16_t)(count < state->ready.maxPacketsPerReport ? count : state->ready.maxPacketsPerReport);
    size_t bytes = TF_REPORT_HEADER_BYTES + (r.packetCount + 3u) / 4u;
    for (unsigned i = 0; i < r.packetCount; ++i) {
        const VIDEO_NETWORK_SLOT* slot = &observer->slots[(first + i) % LI_VIDEO_NETWORK_HISTORY];
        r.status[i] = slot->status;
        r.firstArrivalTimeUs[i] = TF_NO_ARRIVAL;
        if (slot->status == VIDEO_NETWORK_RECEIVED) {
            if (slot->arrivalUs > nowUs || nowUs - slot->arrivalUs > TF_MAX_ARRIVAL_AGE_US) r.status[i] = TF_UNKNOWN;
            else { r.firstArrivalTimeUs[i] = slot->arrivalUs; bytes += 3; }
        }
    }
    if (bytes + VF_WIRE_OVERHEAD_BOUND > state->tokens) return false;
    state->tokens -= (uint32_t)(bytes + VF_WIRE_OVERHEAD_BOUND);
    state->reportsInCycle++;
    r.reportSequence = state->nextReportSequence++;
    *report = r;
    return true;
}

void VfCommitQueued(VIDEO_PACKET_FEEDBACK* state, VIDEO_NETWORK_OBSERVER* observer,
                    const TF_PACKET_REPORT* report) {
    if (report == NULL || report->reportSequence == 0 || report->reportSequence >= state->nextReportSequence ||
        report->reportSequence <= state->lastCommittedReportSequence ||
        report->connectionEpoch != state->expectedConnectionEpoch ||
        report->receiverClockEpoch != observer->snapshot.sequenceEpoch ||
        report->packetCount == 0 || report->packetCount > TF_MAX_PACKETS ||
        report->baseExtendedSequence > UINT64_MAX - (report->packetCount - 1u)) return;
    state->lastCommittedReportSequence = report->reportSequence;
    for (unsigned i = 0; i < report->packetCount; ++i) {
        const uint64_t sequence = report->baseExtendedSequence + i;
        if (sequence < observer->historyStart || sequence > observer->snapshot.highestExtendedSequence) continue;
        VIDEO_NETWORK_SLOT* slot = &observer->slots[sequence % LI_VIDEO_NETWORK_HISTORY];
        const bool tooOld = slot->status == VIDEO_NETWORK_RECEIVED && report->status[i] == TF_UNKNOWN &&
            report->receiverSampleTimeUs >= slot->arrivalUs && report->receiverSampleTimeUs - slot->arrivalUs > TF_MAX_ARRIVAL_AGE_US;
        if ((!tooOld && report->status[i] != slot->status) ||
            (report->status[i] == TF_RECEIVED && report->firstArrivalTimeUs[i] != slot->arrivalUs)) continue;
        slot->feedbackDirty = 0;
        if (slot->feedbackRepeats) slot->feedbackRepeats--;
    }
}
