#include "VideoNetwork.h"

#include <string.h>

static VIDEO_NETWORK_SLOT* slotAt(VIDEO_NETWORK_OBSERVER* observer, uint64_t sequence) {
    return &observer->slots[sequence % LI_VIDEO_NETWORK_HISTORY];
}

void VnInitialize(VIDEO_NETWORK_OBSERVER* observer, uint64_t connectionEpoch,
                  uint64_t nowUs, uint64_t reorderGraceUs, uint64_t maximumSilenceUs) {
    memset(observer, 0, sizeof(*observer));
    observer->snapshot.version = LI_VIDEO_NETWORK_SNAPSHOT_VERSION;
    observer->snapshot.size = sizeof(observer->snapshot);
    observer->snapshot.connectionEpoch = connectionEpoch;
    observer->snapshot.sequenceEpoch = 1;
    observer->snapshot.enabled = true;
    observer->startUs = nowUs;
    observer->reorderGraceUs = reorderGraceUs;
    observer->maximumSilenceUs = maximumSilenceUs;
}

void VnAdvance(VIDEO_NETWORK_OBSERVER* observer, uint64_t nowUs) {
    if (!observer->snapshot.hasSequence) {
        return;
    }
    while (observer->snapshot.settledThroughExclusive <= observer->snapshot.highestExtendedSequence) {
        VIDEO_NETWORK_SLOT* slot = slotAt(observer, observer->snapshot.settledThroughExclusive);
        if (nowUs < slot->observedUs || nowUs - slot->observedUs < observer->reorderGraceUs) {
            break;
        }
        if (slot->status == VIDEO_NETWORK_RECEIVED) {
            observer->snapshot.settledReceivedPackets++;
        }
        else {
            slot->status = VIDEO_NETWORK_MISSING;
            slot->feedbackDirty = 1;
            slot->feedbackRepeats = 2;
            observer->snapshot.missingCandidates++;
        }
        observer->snapshot.settledThroughExclusive++;
    }
}

static void abandonUnsettled(VIDEO_NETWORK_OBSERVER* observer, uint64_t throughExclusive) {
    while (observer->snapshot.settledThroughExclusive < throughExclusive) {
        VIDEO_NETWORK_SLOT* slot = slotAt(observer, observer->snapshot.settledThroughExclusive);
        if (slot->status == VIDEO_NETWORK_RECEIVED) {
            observer->snapshot.settledReceivedPackets++;
        }
        else {
            // History pressure or an ambiguous epoch cannot manufacture loss.
            observer->snapshot.unknownCandidates++;
        }
        observer->snapshot.settledThroughExclusive++;
    }
}

static void beginRange(VIDEO_NETWORK_OBSERVER* observer, uint64_t sequence, uint64_t arrivalUs) {
    observer->historyStart = sequence;
    observer->snapshot.firstExtendedSequence = sequence;
    observer->snapshot.highestExtendedSequence = sequence;
    observer->snapshot.settledThroughExclusive = sequence;
    observer->snapshot.hasSequence = true;
    memset(slotAt(observer, sequence), 0, sizeof(VIDEO_NETWORK_SLOT));
    slotAt(observer, sequence)->observedUs = arrivalUs;
    slotAt(observer, sequence)->feedbackDirty = 1;
    slotAt(observer, sequence)->feedbackRepeats = 2;
}

void VnObserveOriginal(VIDEO_NETWORK_OBSERVER* observer, uint16_t sequence,
                       uint32_t udpBytes, uint64_t arrivalUs) {
    uint64_t extendedSequence;
    bool reset = false;
    bool freshRange = false;
    int delta = 0;

    if (!observer->snapshot.hasSequence) {
        extendedSequence = sequence;
        beginRange(observer, extendedSequence, arrivalUs);
        freshRange = true;
    }
    else {
        delta = (int)sequence - (int)(uint16_t)observer->snapshot.highestExtendedSequence;
        if (delta > 32767) {
            delta -= 65536;
        }
        else if (delta < -32768) {
            delta += 65536;
        }
        reset = delta == -32768 || arrivalUs < observer->lastArrivalUs ||
            (observer->maximumSilenceUs != 0 &&
             arrivalUs - observer->lastArrivalUs > observer->maximumSilenceUs);

        if (reset) {
            // Once a half-cycle can be ambiguous, choose a new local epoch.
            // Do not claim how many packets were transmitted during the gap.
            abandonUnsettled(observer, observer->snapshot.highestExtendedSequence + 1);
            observer->snapshot.sequenceEpoch++;
            observer->snapshot.coverageResets++;
            extendedSequence = sequence;
            beginRange(observer, extendedSequence, arrivalUs);
            freshRange = true;
        }
        else if (delta < 0 && (uint64_t)(-delta) > observer->snapshot.highestExtendedSequence) {
            observer->snapshot.untrackedPackets++;
            observer->lastArrivalUs = arrivalUs;
            return;
        }
        else {
            extendedSequence = delta < 0
                ? observer->snapshot.highestExtendedSequence - (uint64_t)(-delta)
                : observer->snapshot.highestExtendedSequence + (uint64_t)delta;
        }
    }

    observer->lastArrivalUs = arrivalUs;
    if (!freshRange) {
        VnAdvance(observer, arrivalUs);
    }

    if (!reset && extendedSequence < observer->historyStart) {
        observer->snapshot.untrackedPackets++;
        return;
    }

    if (!reset && delta >= LI_VIDEO_NETWORK_HISTORY) {
        abandonUnsettled(observer, observer->snapshot.highestExtendedSequence + 1);
        observer->snapshot.unknownCandidates += (uint64_t)delta - 1;
        observer->snapshot.coverageResets++;
        observer->snapshot.sequenceEpoch++;
        beginRange(observer, extendedSequence, arrivalUs);
        reset = true;
    }

    if (extendedSequence > observer->snapshot.highestExtendedSequence) {
        const uint64_t oldHigh = observer->snapshot.highestExtendedSequence;
        if (extendedSequence - observer->historyStart >= LI_VIDEO_NETWORK_HISTORY) {
            const uint64_t newStart = extendedSequence - LI_VIDEO_NETWORK_HISTORY + 1;
            abandonUnsettled(observer, newStart);
            observer->historyStart = newStart;
        }
        for (uint64_t s = oldHigh + 1; s <= extendedSequence; s++) {
            VIDEO_NETWORK_SLOT* slot = slotAt(observer, s);
            memset(slot, 0, sizeof(*slot));
            slot->observedUs = arrivalUs;
            slot->feedbackDirty = 1;
            slot->feedbackRepeats = 2;
        }
        observer->snapshot.highestExtendedSequence = extendedSequence;
    }

    VIDEO_NETWORK_SLOT* slot = slotAt(observer, extendedSequence);
    if (slot->status == VIDEO_NETWORK_RECEIVED) {
        observer->snapshot.duplicatePackets++;
        return;
    }
    if (slot->status == VIDEO_NETWORK_MISSING) {
        observer->snapshot.latePackets++;
        observer->snapshot.settledReceivedPackets++;
    }
    if (extendedSequence < observer->snapshot.highestExtendedSequence) {
        observer->snapshot.reorderedPackets++;
    }
    slot->status = VIDEO_NETWORK_RECEIVED;
    slot->feedbackDirty = 1;
    slot->feedbackRepeats = 2;
    slot->arrivalUs = arrivalUs;
    slot->udpBytes = udpBytes;
    observer->snapshot.uniquePackets++;
    observer->snapshot.uniqueUdpBytes += udpBytes;
}

void VnGetSnapshot(const VIDEO_NETWORK_OBSERVER* observer, uint64_t nowUs,
                   LI_VIDEO_NETWORK_SNAPSHOT* snapshot) {
    *snapshot = observer->snapshot;
    snapshot->sampleTimeUs = nowUs;
    snapshot->observationDurationUs = nowUs >= observer->startUs ? nowUs - observer->startUs : 0;
}

bool VnParseVideoPayload(const uint8_t* payload, size_t length, bool multiFecCapable,
                        uint16_t* rtpSequence, uint32_t* transportSequence24) {
    // Moonlight's fixed video extension is four bytes; CSRCs, padding and
    // other RTP versions are not part of this transport format.
    if (payload == NULL || length < 32 || payload[0] != 0x90) {
        return false;
    }
    const uint32_t fecInfo = (uint32_t)payload[28] | ((uint32_t)payload[29] << 8) |
        ((uint32_t)payload[30] << 16) | ((uint32_t)payload[31] << 24);
    const uint32_t data = fecInfo >> 22;
    const uint32_t percentage = (fecInfo >> 4) & 0xff;
    const uint32_t index = (fecInfo >> 12) & 0x3ff;
    const uint32_t parity = (data * percentage + 99) / 100;
    const uint8_t block = (payload[27] >> 4) & 3;
    const uint8_t lastBlock = (payload[27] >> 6) & 3;
    if (data == 0 || index >= data + parity ||
        (percentage != 0 && data + parity > 255) ||
        (multiFecCapable && block > lastBlock)) {
        return false;
    }
    const uint16_t sequence = (uint16_t)(((uint16_t)payload[2] << 8) | payload[3]);
    if (rtpSequence != NULL) *rtpSequence = sequence;
    if (transportSequence24 != NULL) {
        const uint32_t streamIndex = (uint32_t)payload[16] | ((uint32_t)payload[17] << 8) |
            ((uint32_t)payload[18] << 16) | ((uint32_t)payload[19] << 24);
        // This field in parity shards contains RS symbols, not a transmitted
        // sequence. Rewriting it after RS encoding corrupts recovered headers.
        *transportSequence24 = index < data ? streamIndex >> 8 : UINT32_MAX;
    }
    return true;
}

bool VnObserveVideoPayload(VIDEO_NETWORK_OBSERVER* observer, const uint8_t* payload,
                           size_t length, uint32_t udpBytes, uint64_t arrivalUs,
                           bool multiFecCapable) {
    uint16_t sequence;
    if (!VnParseVideoPayload(payload, length, multiFecCapable, &sequence, NULL)) {
        observer->snapshot.invalidPackets++;
        return false;
    }
    VnObserveOriginal(observer, sequence, udpBytes, arrivalUs);
    return true;
}

void VnCoverSubmittedThrough(VIDEO_NETWORK_OBSERVER* observer, uint64_t throughExclusive, uint64_t nowUs) {
    if (!throughExclusive) return;
    const uint64_t high = throughExclusive - 1;
    if (observer->snapshot.hasSequence && high <= observer->snapshot.highestExtendedSequence) return;
    VnAdvance(observer, nowUs);
    const uint64_t start = throughExclusive > LI_VIDEO_NETWORK_HISTORY ? throughExclusive - LI_VIDEO_NETWORK_HISTORY : 0;
    uint64_t next;
    if (!observer->snapshot.hasSequence) {
        observer->snapshot.unknownCandidates += start;
        beginRange(observer, start, nowUs);
        next = start;
    }
    else {
        next = observer->snapshot.highestExtendedSequence + 1;
        if (start > next) {
            abandonUnsettled(observer, next);
            observer->snapshot.unknownCandidates += start - next;
            observer->snapshot.sequenceEpoch++;
            observer->snapshot.coverageResets++;
            beginRange(observer, start, nowUs);
            next = start;
        }
        else if (start > observer->historyStart) {
            abandonUnsettled(observer, start);
            observer->historyStart = start;
        }
    }
    for (uint64_t sequence = next; sequence <= high; ++sequence) {
        VIDEO_NETWORK_SLOT* slot = slotAt(observer, sequence);
        memset(slot, 0, sizeof(*slot));
        slot->observedUs = nowUs;
        slot->feedbackDirty = 1;
        slot->feedbackRepeats = 2;
    }
    observer->snapshot.highestExtendedSequence = high;
}

void VnObserveExtended(VIDEO_NETWORK_OBSERVER* observer, uint64_t sequence, uint32_t udpBytes, uint64_t arrivalUs) {
    if (sequence == UINT64_MAX || arrivalUs < observer->lastArrivalUs) {
        observer->snapshot.untrackedPackets++;
        return;
    }
    VnCoverSubmittedThrough(observer, sequence + 1, arrivalUs);
    observer->lastArrivalUs = arrivalUs;
    VnAdvance(observer, arrivalUs);
    if (sequence < observer->historyStart) { observer->snapshot.untrackedPackets++; return; }
    VIDEO_NETWORK_SLOT* slot = slotAt(observer, sequence);
    if (slot->status == VIDEO_NETWORK_RECEIVED) { observer->snapshot.duplicatePackets++; return; }
    if (slot->status == VIDEO_NETWORK_MISSING) {
        observer->snapshot.latePackets++;
        observer->snapshot.settledReceivedPackets++;
    }
    if (observer->hasOriginalSequence && sequence < observer->highestOriginalSequence) observer->snapshot.reorderedPackets++;
    if (!observer->hasOriginalSequence || sequence > observer->highestOriginalSequence) observer->highestOriginalSequence = sequence;
    observer->hasOriginalSequence = true;
    slot->status = VIDEO_NETWORK_RECEIVED;
    slot->arrivalUs = arrivalUs;
    slot->udpBytes = udpBytes;
    slot->feedbackDirty = 1;
    slot->feedbackRepeats = 2;
    observer->snapshot.uniquePackets++;
    observer->snapshot.uniqueUdpBytes += udpBytes;
}

VIDEO_NETWORK_PACKET_RECORD VnGetPacket(const VIDEO_NETWORK_OBSERVER* observer,
                                        uint64_t sequenceEpoch, uint64_t extendedSequence) {
    VIDEO_NETWORK_PACKET_RECORD record = {extendedSequence, 0, 0, VIDEO_NETWORK_UNKNOWN};
    if (sequenceEpoch != observer->snapshot.sequenceEpoch || !observer->snapshot.hasSequence ||
        extendedSequence < observer->historyStart || extendedSequence > observer->snapshot.highestExtendedSequence) {
        return record;
    }
    const VIDEO_NETWORK_SLOT* slot = &observer->slots[extendedSequence % LI_VIDEO_NETWORK_HISTORY];
    record.status = (VIDEO_NETWORK_PACKET_STATUS)slot->status;
    record.firstArrivalUs = slot->arrivalUs;
    record.udpBytes = slot->udpBytes;
    return record;
}
