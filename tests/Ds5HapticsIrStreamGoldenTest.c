#include "Ds5HapticsIrStream.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void writeLe16(uint8_t* data, uint16_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
}

static void writeLe32(uint8_t* data, uint32_t value) {
    data[0] = (uint8_t)value;
    data[1] = (uint8_t)(value >> 8);
    data[2] = (uint8_t)(value >> 16);
    data[3] = (uint8_t)(value >> 24);
}

static void writeLe64(uint8_t* data, uint64_t value) {
    writeLe32(data, (uint32_t)value);
    writeLe32(data + 4, (uint32_t)(value >> 32));
}

static void writeLeFloat(uint8_t* data, float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    writeLe32(data, bits);
}

static void makeValidPacket(uint8_t packet[DS5_HAPTICS_IR_STREAM_WIRE_SIZE]) {
    int lane;
    memset(packet, 0, DS5_HAPTICS_IR_STREAM_WIRE_SIZE);
    packet[0] = DS5_HAPTICS_IR_STREAM_PROTOCOL_VERSION;
    packet[1] = LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY;
    writeLe16(packet + 2, DS5_HAPTICS_IR_STREAM_WIRE_SIZE);
    writeLe16(packet + 4, 3);
    writeLe32(packet + 8, UINT32_C(0x11223344));
    writeLe64(packet + 12, UINT64_C(0x0102030405060708));
    writeLe32(packet + 20, 240);
    for (lane = 0; lane < 2; lane++) {
        uint8_t* out = packet + 24 + lane * 20;
        writeLeFloat(out, 0.25f + lane * 0.1f);
        writeLeFloat(out + 4, 0.5f + lane * 0.1f);
        writeLeFloat(out + 8, 0.75f);
        writeLeFloat(out + 12, 0.2f);
        writeLeFloat(out + 16, 1200.0f + lane * 100.0f);
    }
    writeLeFloat(packet + 64, -0.5f);
}

static void captureFrame(const LI_DS5_HAPTICS_IR_FRAME_V2* frame, void* context) {
    LI_DS5_HAPTICS_IR_FRAME_V2* captured = context;
    *captured = *frame;
}

static void testValidFrame(void) {
    uint8_t packet[DS5_HAPTICS_IR_STREAM_WIRE_SIZE];
    LI_DS5_HAPTICS_IR_FRAME_V2 captured = {0};
    makeValidPacket(packet);

    CHECK(processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    CHECK(captured.flags == LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY);
    CHECK(captured.controllerNumber == 3);
    CHECK(captured.sourceSequenceNumber == UINT32_C(0x11223344));
    CHECK(captured.timestampUs == UINT64_C(0x0102030405060708));
    CHECK(captured.sourceFrameCount == 240);
    CHECK(fabsf(captured.lanes[0].rmsAmplitude - 0.25f) < 0.0001f);
    CHECK(fabsf(captured.lanes[1].zeroCrossingRateHz - 1300.0f) < 0.0001f);
    CHECK(fabsf(captured.laneCorrelation + 0.5f) < 0.0001f);
}

static void testMalformedFrames(void) {
    uint8_t packet[DS5_HAPTICS_IR_STREAM_WIRE_SIZE];
    LI_DS5_HAPTICS_IR_FRAME_V2 captured = {0};
    makeValidPacket(packet);

    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet) - 1, captureFrame, &captured));
    packet[0] = 1;
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    packet[0] = DS5_HAPTICS_IR_STREAM_PROTOCOL_VERSION;
    packet[1] = 0x80;
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    packet[1] = 0;
    writeLe16(packet + 6, 1);
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    writeLe16(packet + 6, 0);
    writeLe32(packet + 24, UINT32_C(0x7fc00000));
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    makeValidPacket(packet);
    writeLeFloat(packet + 24, 0.75f);
    writeLeFloat(packet + 28, 0.5f);
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
    makeValidPacket(packet);
    writeLeFloat(packet + 64, 1.1f);
    CHECK(!processDs5HapticsIrStreamPacket(packet, sizeof(packet), captureFrame, &captured));
}


static void makeValidV3Packet(uint8_t* packet, unsigned events) {
    unsigned i;
    unsigned length = DS5_HAPTICS_IR_V3_STREAM_BASE_SIZE + events * DS5_HAPTICS_IR_V3_STREAM_EVENT_SIZE;
    memset(packet, 0, DS5_HAPTICS_IR_V3_STREAM_MAX_SIZE);
    packet[0] = 3;
    packet[1] = LI_DS5_HAPTICS_IR_FLAG_DISCONTINUITY;
    writeLe16(packet + 2, (uint16_t)length);
    writeLe16(packet + 4, 2);
    writeLe16(packet + 6, 1);
    writeLe32(packet + 8, UINT32_C(0xfffffffe));
    writeLe32(packet + 12, UINT32_C(0xfedcba98));
    writeLe64(packet + 16, UINT64_C(0x0102030405060708));
    writeLe32(packet + 24, 240);
    packet[28] = (uint8_t)events;
    packet[29] = 2;
    for (i = 0; i < 2; i++) {
        uint8_t* lane = packet + 32 + i * 16;
        // Envelope release can exceed the current raw peak.
        writeLeFloat(lane, 0.8f);
        writeLeFloat(lane + 4, 0.6f);
        writeLeFloat(lane + 8, 0.1f);
        writeLeFloat(lane + 12, 0.2f);
    }
    for (i = 0; i < events; i++) {
        uint8_t* event = packet + 64 + i * 32;
        writeLe32(event, UINT32_C(0x12345678) + i);
        writeLe64(event + 8, UINT64_C(0x0102030405060708) - i * 5000);
        writeLeFloat(event + 16, 0.25f);
        writeLeFloat(event + 20, 0.75f);
        writeLeFloat(event + 24, 1.0f);
        writeLeFloat(event + 28, 0.5f);
    }
}

static void captureV3Frame(const LI_DS5_HAPTICS_IR_FRAME_V3* frame, void* context) {
    LI_DS5_HAPTICS_IR_FRAME_V3* captured = context;
    *captured = *frame;
}

static void testV3Frames(void) {
    uint8_t packet[DS5_HAPTICS_IR_V3_STREAM_MAX_SIZE + 1];
    LI_DS5_HAPTICS_IR_FRAME_V3 captured = {0};
    unsigned i, count;
    for (count = 0; count <= LI_DS5_HAPTICS_IR_V3_MAX_EVENTS; count++) {
        int length = 64 + (int)count * 32;
        makeValidV3Packet(packet, count);
        CHECK(processDs5HapticsIrV3StreamPacket(packet, length, captureV3Frame, &captured));
        CHECK(captured.controllerNumber == 2 && captured.profile == 1);
        CHECK(captured.epoch == UINT32_C(0xfffffffe));
        CHECK(captured.frameSequence == UINT32_C(0xfedcba98));
        CHECK(captured.windowEndUs == UINT64_C(0x0102030405060708));
        CHECK(captured.sourceFrameCount == 240 && captured.eventCount == count);
        CHECK(captured.lanes[0].continuousLow > captured.lanes[0].rawPeak);
        CHECK(captured.lanes[1].rawRms == 0.1f);
        for (i = 0; i < count; i++) {
            CHECK(captured.events[i].eventId == UINT32_C(0x12345678) + i);
            CHECK(captured.events[i].sourceTimeUs == captured.windowEndUs - i * 5000);
            CHECK(captured.events[i].leftStrength == 0.25f);
            CHECK(captured.events[i].rightStrength == 0.75f);
            CHECK(captured.events[i].confidence == 1.0f);
            CHECK(captured.events[i].sharpness == 0.5f);
        }
        // Every truncated length must fail before invoking the callback.
        for (i = 0; i < (unsigned)length; i++) {
            CHECK(!processDs5HapticsIrV3StreamPacket(packet, (int)i, captureV3Frame, &captured));
        }
    }
    makeValidV3Packet(packet, 0);
    packet[1] = LI_DS5_HAPTICS_IR_FLAG_STREAM_END | LI_DS5_HAPTICS_IR_FLAG_SILENT;
    writeLe32(packet + 24, 0);
    writeLe32(packet + 8, 0); // Epoch zero is a legal wire value.
    CHECK(processDs5HapticsIrV3StreamPacket(packet, 64, captureV3Frame, &captured));
    CHECK(captured.epoch == 0 && captured.sourceFrameCount == 0);

    makeValidV3Packet(packet, 1);
    CHECK(!processDs5HapticsIrV3StreamPacket(NULL, 96, captureV3Frame, &captured));
    CHECK(!processDs5HapticsIrV3StreamPacket(packet, 96, NULL, &captured));
    CHECK(!processDs5HapticsIrV3StreamPacket(packet, 97, captureV3Frame, &captured));
    CHECK(!processDs5HapticsIrV3StreamPacket(packet, 321, captureV3Frame, &captured));

    // Header fields: unsupported version, flags, size, profile, lane count,
    // reserved bits and event count.
    {
        static const unsigned offsets[] = {0, 1, 2, 6, 29, 30, 28};
        static const uint8_t invalid[] = {2, 0x80, 95, 2, 1, 1, 9};
        for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            makeValidV3Packet(packet, 1);
            packet[offsets[i]] = invalid[i];
            CHECK(!processDs5HapticsIrV3StreamPacket(packet, 96, captureV3Frame, &captured));
        }
    }
    // Source intervals are independent of the producer SDK's 240-frame chunks.
    {
        static const uint32_t frameCounts[] = {1, 241, 480, UINT32_MAX};
        for (i = 0; i < sizeof(frameCounts) / sizeof(frameCounts[0]); i++) {
            makeValidV3Packet(packet, 1);
            writeLe32(packet + 24, frameCounts[i]);
            CHECK(processDs5HapticsIrV3StreamPacket(packet, 96, captureV3Frame, &captured));
            CHECK(captured.sourceFrameCount == frameCounts[i]);
        }
    }
    writeLe32(packet + 24, 0);
    CHECK(!processDs5HapticsIrV3StreamPacket(packet, 96, captureV3Frame, &captured));
    makeValidV3Packet(packet, 1);
    writeLe32(packet + 68, 1);
    CHECK(!processDs5HapticsIrV3StreamPacket(packet, 96, captureV3Frame, &captured));
    makeValidV3Packet(packet, 1);
    writeLe64(packet + 72, UINT64_C(0x0102030405060709));
    CHECK(!processDs5HapticsIrV3StreamPacket(packet, 96, captureV3Frame, &captured));

    // All normalized lane and event fields reject NaN, infinities and overflow.
    {
        static const unsigned offsets[] = {32,36,40,44,48,52,56,60,80,84,88,92};
        static const float invalid[] = {NAN, INFINITY, -INFINITY, -0.01f, 1.01f};
        unsigned j;
        for (i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
            for (j = 0; j < sizeof(invalid) / sizeof(invalid[0]); j++) {
                makeValidV3Packet(packet, 1);
                writeLeFloat(packet + offsets[i], invalid[j]);
                CHECK(!processDs5HapticsIrV3StreamPacket(packet, 96, captureV3Frame, &captured));
            }
        }
    }
    makeValidV3Packet(packet, 1);
    writeLeFloat(packet + 40, 0.5f);
    CHECK(!processDs5HapticsIrV3StreamPacket(packet, 96, captureV3Frame, &captured));
}


int main(void) {
    testValidFrame();
    testMalformedFrames();
    testV3Frames();
    if (failures != 0) {
        fprintf(stderr, "%d DualSense IR protocol checks failed\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
