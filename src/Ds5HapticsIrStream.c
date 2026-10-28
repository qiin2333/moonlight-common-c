#include "Ds5HapticsIrStream.h"

#include <math.h>
#include <string.h>

static uint16_t readLe16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t readLe32(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

static uint64_t readLe64(const uint8_t* data) {
    return (uint64_t)readLe32(data) | ((uint64_t)readLe32(data + 4) << 32);
}

static float readLeFloat(const uint8_t* data) {
    uint32_t bits = readLe32(data);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool isNormalized(float value) {
    return isfinite(value) && value >= 0.0f && value <= 1.0f;
}

bool processDs5HapticsIrStreamPacket(const uint8_t* payload,
                                     int payloadLength,
                                     Ds5HapticsIrStreamCallback callback,
                                     void* context) {
    LI_DS5_HAPTICS_IR_FRAME_V2 frame;
    const uint8_t knownFlags = LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY |
                               LI_DS5_HAPTICS_IR_FLAG_PARTIAL |
                               LI_DS5_HAPTICS_IR_FLAG_STREAM_END |
                               LI_DS5_HAPTICS_IR_FLAG_SILENT;
    int lane;

    if (payload == NULL || callback == NULL ||
            payloadLength != DS5_HAPTICS_IR_STREAM_WIRE_SIZE ||
            payload[0] != DS5_HAPTICS_IR_STREAM_PROTOCOL_VERSION ||
            (payload[1] & ~knownFlags) != 0 ||
            readLe16(payload + 2) != DS5_HAPTICS_IR_STREAM_WIRE_SIZE ||
            readLe16(payload + 6) != 0 || readLe32(payload + 68) != 0) {
        return false;
    }

    memset(&frame, 0, sizeof(frame));
    frame.flags = payload[1];
    frame.controllerNumber = readLe16(payload + 4);
    frame.sourceSequenceNumber = readLe32(payload + 8);
    frame.timestampUs = readLe64(payload + 12);
    frame.sourceFrameCount = readLe32(payload + 20);

    for (lane = 0; lane < 2; lane++) {
        const uint8_t* wireLane = payload + 24 + lane * 20;
        LI_DS5_HAPTICS_IR_LANE_V2* outLane = &frame.lanes[lane];
        outLane->rmsAmplitude = readLeFloat(wireLane);
        outLane->peakAmplitude = readLeFloat(wireLane + 4);
        outLane->transientStrength = readLeFloat(wireLane + 8);
        outLane->lowBandRatio = readLeFloat(wireLane + 12);
        outLane->zeroCrossingRateHz = readLeFloat(wireLane + 16);
        if (!isNormalized(outLane->rmsAmplitude) ||
                !isNormalized(outLane->peakAmplitude) ||
                outLane->rmsAmplitude > outLane->peakAmplitude ||
                !isNormalized(outLane->transientStrength) ||
                !isNormalized(outLane->lowBandRatio) ||
                !isfinite(outLane->zeroCrossingRateHz) ||
                outLane->zeroCrossingRateHz < 0.0f ||
                outLane->zeroCrossingRateHz > 48000.0f) {
            return false;
        }
    }

    frame.laneCorrelation = readLeFloat(payload + 64);
    if (!isfinite(frame.laneCorrelation) ||
            frame.laneCorrelation < -1.0f || frame.laneCorrelation > 1.0f) {
        return false;
    }

    callback(&frame, context);
    return true;
}

bool processDs5HapticsIrV3StreamPacket(const uint8_t* payload,
                                      int payloadLength,
                                      Ds5HapticsIrV3StreamCallback callback,
                                      void* context) {
    LI_DS5_HAPTICS_IR_FRAME_V3 frame;
    const uint8_t knownFlags = LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY |
                               LI_DS5_HAPTICS_IR_FLAG_PARTIAL |
                               LI_DS5_HAPTICS_IR_FLAG_STREAM_END |
                               LI_DS5_HAPTICS_IR_FLAG_SILENT;
    unsigned int i;
    if (payload == NULL || callback == NULL ||
            payloadLength < DS5_HAPTICS_IR_V3_STREAM_BASE_SIZE ||
            payloadLength > DS5_HAPTICS_IR_V3_STREAM_MAX_SIZE ||
            payload[0] != DS5_HAPTICS_IR_V3_STREAM_PROTOCOL_VERSION ||
            (payload[1] & ~knownFlags) != 0 ||
            readLe16(payload + 2) != (unsigned int)payloadLength ||
            readLe16(payload + 6) != LI_DS5_HAPTICS_IR_V3_PROFILE ||
            payload[28] > LI_DS5_HAPTICS_IR_V3_MAX_EVENTS ||
            payload[29] != 2 || readLe16(payload + 30) != 0 ||
            payloadLength != DS5_HAPTICS_IR_V3_STREAM_BASE_SIZE +
                payload[28] * DS5_HAPTICS_IR_V3_STREAM_EVENT_SIZE) {
        return false;
    }

    memset(&frame, 0, sizeof(frame));
    frame.flags = payload[1];
    frame.controllerNumber = readLe16(payload + 4);
    frame.profile = readLe16(payload + 6);
    frame.epoch = readLe32(payload + 8);
    frame.frameSequence = readLe32(payload + 12);
    frame.windowEndUs = readLe64(payload + 16);
    frame.sourceFrameCount = readLe32(payload + 24);
    frame.eventCount = payload[28];
    // As in v2, sourceFrameCount describes the source interval; it is not a
    // payload length or the analyzer SDK's per-call block-size limit.
    if (frame.sourceFrameCount == 0 && !(frame.flags & LI_DS5_HAPTICS_IR_FLAG_STREAM_END)) {
        return false;
    }
    for (i = 0; i < 2; i++) {
        const uint8_t* lane = payload + 32 + i * 16;
        LI_DS5_HAPTICS_IR_LANE_V3* out = &frame.lanes[i];
        out->continuousLow = readLeFloat(lane);
        out->continuousHigh = readLeFloat(lane + 4);
        out->rawRms = readLeFloat(lane + 8);
        out->rawPeak = readLeFloat(lane + 12);
        if (!isNormalized(out->continuousLow) || !isNormalized(out->continuousHigh) ||
                !isNormalized(out->rawRms) || !isNormalized(out->rawPeak) ||
                out->rawRms > out->rawPeak) {
            return false;
        }
        // Continuous envelopes carry a release tail and may exceed rawPeak.
    }
    for (i = 0; i < frame.eventCount; i++) {
        const uint8_t* event = payload + DS5_HAPTICS_IR_V3_STREAM_BASE_SIZE +
            i * DS5_HAPTICS_IR_V3_STREAM_EVENT_SIZE;
        LI_DS5_HAPTICS_IR_EVENT_V3* out = &frame.events[i];
        if (readLe32(event + 4) != 0) {
            return false;
        }
        out->eventId = readLe32(event);
        out->sourceTimeUs = readLe64(event + 8);
        out->leftStrength = readLeFloat(event + 16);
        out->rightStrength = readLeFloat(event + 20);
        out->confidence = readLeFloat(event + 24);
        out->sharpness = readLeFloat(event + 28);
        if (out->sourceTimeUs > frame.windowEndUs ||
                !isNormalized(out->leftStrength) || !isNormalized(out->rightStrength) ||
                !isNormalized(out->confidence) || !isNormalized(out->sharpness)) {
            return false;
        }
    }

    callback(&frame, context);
    return true;
}
