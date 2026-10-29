#include "CursorStream.h"

#include <stdio.h>
#include <string.h>

typedef struct _CAPTURED_UPDATE {
    int count;
    LI_CURSOR_UPDATE update;
    uint8_t pixels[32];
} CAPTURED_UPDATE;

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void captureUpdate(const LI_CURSOR_UPDATE* update, void* context) {
    CAPTURED_UPDATE* captured = context;

    captured->count++;
    captured->update = *update;
    if (update->pixels != NULL && update->pixelDataLength <= sizeof(captured->pixels)) {
        memcpy(captured->pixels, update->pixels, update->pixelDataLength);
        captured->update.pixels = captured->pixels;
    }
}

static void testVisibilityOnly(void) {
    static const uint8_t packet[] = {
        0x01, 0x02, 0x1c, 0x00,
        0x44, 0x33, 0x22, 0x11,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    CURSOR_STREAM_STATE state;
    CAPTURED_UPDATE captured = {0};

    initializeCursorStreamState(&state);
    processCursorStreamPacket(&state, packet, sizeof(packet), captureUpdate, &captured);

    CHECK(captured.count == 1);
    CHECK(captured.update.flags == LI_CURSOR_UPDATE_FLAG_VISIBLE);
    CHECK(captured.update.shapeId == 0x11223344);
    CHECK(captured.update.pixelDataLength == 0);
    destroyCursorStreamState(&state);
}

static void testSingleChunkShape(void) {
    static const uint8_t packet[] = {
        0x01, 0x01, 0x1c, 0x00,
        0x04, 0x03, 0x02, 0x01,
        0x02, 0x00, 0x01, 0x00,
        0x01, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00,
        0x10, 0x20, 0x30, 0x40,
        0x50, 0x60, 0x70, 0x80,
    };
    static const uint8_t expectedPixels[] = {
        0x10, 0x20, 0x30, 0x40,
        0x50, 0x60, 0x70, 0x80,
    };
    CURSOR_STREAM_STATE state;
    CAPTURED_UPDATE captured = {0};

    initializeCursorStreamState(&state);
    processCursorStreamPacket(&state, packet, sizeof(packet), captureUpdate, &captured);

    CHECK(captured.count == 1);
    CHECK(captured.update.flags == LI_CURSOR_UPDATE_FLAG_SHAPE);
    CHECK(captured.update.shapeId == 0x01020304);
    CHECK(captured.update.width == 2);
    CHECK(captured.update.height == 1);
    CHECK(captured.update.hotspotX == 1);
    CHECK(captured.update.hotspotY == 0);
    CHECK(captured.update.pixelDataLength == sizeof(expectedPixels));
    CHECK(memcmp(captured.update.pixels, expectedPixels, sizeof(expectedPixels)) == 0);
    destroyCursorStreamState(&state);
}

static void testMultiChunkShape(void) {
    static const uint8_t first[] = {
        0x01, 0x03, 0x1c, 0x00,
        0xdd, 0xcc, 0xbb, 0xaa,
        0x02, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x01, 0x00,
        0x10, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x06, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
    };
    static const uint8_t second[] = {
        0x01, 0x03, 0x1c, 0x00,
        0xdd, 0xcc, 0xbb, 0xaa,
        0x02, 0x00, 0x02, 0x00,
        0x00, 0x00, 0x01, 0x00,
        0x10, 0x00, 0x00, 0x00,
        0x06, 0x00, 0x00, 0x00,
        0x0a, 0x00, 0x00, 0x00,
        0x06, 0x07, 0x08, 0x09, 0x0a,
        0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    };
    static const uint8_t expectedPixels[] = {
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f,
    };
    CURSOR_STREAM_STATE state;
    CAPTURED_UPDATE captured = {0};

    initializeCursorStreamState(&state);
    processCursorStreamPacket(&state, first, sizeof(first), captureUpdate, &captured);
    CHECK(captured.count == 0);
    processCursorStreamPacket(&state, second, sizeof(second), captureUpdate, &captured);

    CHECK(captured.count == 1);
    CHECK(captured.update.flags ==
          (LI_CURSOR_UPDATE_FLAG_SHAPE | LI_CURSOR_UPDATE_FLAG_VISIBLE));
    CHECK(captured.update.shapeId == 0xaabbccdd);
    CHECK(captured.update.pixelDataLength == sizeof(expectedPixels));
    CHECK(memcmp(captured.update.pixels, expectedPixels, sizeof(expectedPixels)) == 0);
    destroyCursorStreamState(&state);
}

static void testOutOfOrderChunkRecovers(void) {
    static const uint8_t first[] = {
        0x01, 0x01, 0x1c, 0x00,
        0x78, 0x56, 0x34, 0x12,
        0x02, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0xaa, 0xbb, 0xcc, 0xdd,
    };
    static const uint8_t second[] = {
        0x01, 0x01, 0x1c, 0x00,
        0x78, 0x56, 0x34, 0x12,
        0x02, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x11, 0x22, 0x33, 0x44,
    };
    CURSOR_STREAM_STATE state;
    CAPTURED_UPDATE captured = {0};

    initializeCursorStreamState(&state);
    processCursorStreamPacket(&state, second, sizeof(second), captureUpdate, &captured);
    CHECK(captured.count == 0);
    processCursorStreamPacket(&state, first, sizeof(first), captureUpdate, &captured);
    processCursorStreamPacket(&state, second, sizeof(second), captureUpdate, &captured);
    CHECK(captured.count == 1);
    destroyCursorStreamState(&state);
}

static void testMalformedPacketResetsPartialShape(void) {
    static const uint8_t first[] = {
        0x01, 0x01, 0x1c, 0x00,
        0x78, 0x56, 0x34, 0x12,
        0x02, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0xaa, 0xbb, 0xcc, 0xdd,
    };
    static const uint8_t malformed[] = {
        0x01, 0x81, 0x1c, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static const uint8_t second[] = {
        0x01, 0x01, 0x1c, 0x00,
        0x78, 0x56, 0x34, 0x12,
        0x02, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x08, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x11, 0x22, 0x33, 0x44,
    };
    CURSOR_STREAM_STATE state;
    CAPTURED_UPDATE captured = {0};

    initializeCursorStreamState(&state);
    processCursorStreamPacket(&state, first, sizeof(first), captureUpdate, &captured);
    processCursorStreamPacket(&state, malformed, sizeof(malformed), captureUpdate, &captured);
    processCursorStreamPacket(&state, second, sizeof(second), captureUpdate, &captured);

    CHECK(captured.count == 0);
    CHECK(state.pixels == NULL);
    destroyCursorStreamState(&state);
}

// Cursor protocol v2. The byte vectors match Foundation Sunshine's
// tests/unit/test_host_cursor.cpp.

static void putLe16(uint8_t* p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void putLe32(uint8_t* p, uint32_t value) {
    putLe16(p, (uint16_t)value);
    putLe16(p + 2, (uint16_t)(value >> 16));
}

static const uint8_t kV2State[CURSOR_STREAM_V2_STATE_SIZE] = {
    0x02, 0x01, 0x05, 0x00,
    0x04, 0x03, 0x02, 0x01,
    0x0d, 0x0c, 0x0b, 0x0a,
    0x80, 0x07, 0x38, 0x04,
    0x00, 0x80, 0x01, 0x00,
    0xfe, 0xff, 0xff, 0xff,
    0x44, 0x33, 0x22, 0x11,
    0x01, 0x00, 0x00, 0x00,
    0x02, 0x00, 0x00, 0x00,
    0x03, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00,
};

typedef struct _CAPTURED_V2 {
    int states;
    int shapes;
    LI_CURSOR_STATE state;
    LI_CURSOR_SHAPE shape;
    uint8_t first;
    uint8_t middle;
    uint8_t last;
} CAPTURED_V2;

static void captureV2State(const LI_CURSOR_STATE* state, void* context) {
    CAPTURED_V2* captured = context;

    captured->states++;
    captured->state = *state;
}

static void captureV2Shape(const LI_CURSOR_SHAPE* shape, void* context) {
    CAPTURED_V2* captured = context;
    const size_t size = (size_t)shape->width * shape->height * 4;

    captured->shapes++;
    captured->shape = *shape;
    captured->shape.pixels = NULL;
    captured->first = shape->pixels[0];
    captured->middle = shape->pixels[size / 2];
    captured->last = shape->pixels[size - 1];
}

static size_t makeV2Chunk(uint8_t* out, uint32_t shapeId, uint16_t width, uint16_t height,
                          uint32_t offset, uint16_t length, uint8_t fill) {
    memset(out, 0, CURSOR_STREAM_V2_SHAPE_HEADER_SIZE);
    out[0] = CURSOR_STREAM_V2_PROTOCOL_VERSION;
    out[1] = CURSOR_STREAM_V2_KIND_SHAPE;
    out[2] = CURSOR_STREAM_V2_FORMAT_BGRA8;
    putLe32(out + 4, shapeId);
    putLe16(out + 8, width);
    putLe16(out + 10, height);
    putLe16(out + 12, 1);
    putLe16(out + 14, 2);
    putLe32(out + 16, (uint32_t)width * height * 4);
    putLe32(out + 20, offset);
    putLe16(out + 24, length);
    memset(out + CURSOR_STREAM_V2_SHAPE_HEADER_SIZE, fill, length);
    return CURSOR_STREAM_V2_SHAPE_HEADER_SIZE + (size_t)length;
}

static int feedV2(CURSOR_STREAM_V2_STATE* state, const uint8_t* payload, size_t size, CAPTURED_V2* captured) {
    return processCursorStreamV2Packet(state, payload, size, captureV2State, captureV2Shape, captured);
}

static void testV2StateFields(void) {
    LI_CURSOR_STATE state;

    CHECK(decodeCursorStreamV2State(kV2State, sizeof(kV2State), &state));
    CHECK(state.flags == (LI_CURSOR_STATE_FLAG_VISIBLE | LI_CURSOR_STATE_FLAG_HAS_POSITION));
    CHECK(state.sequence == 0x01020304);
    CHECK(state.shapeId == 0x0a0b0c0d);
    CHECK(state.referenceWidth == 1920);
    CHECK(state.referenceHeight == 1080);
    CHECK(state.scaleQ16 == 0x18000);
    CHECK(state.x == -2);
    CHECK(state.y == 0x11223344);
    CHECK(state.clipLeft == 1);
    CHECK(state.clipTop == 2);
    CHECK(state.clipRight == 3);
    CHECK(state.clipBottom == 4);
    CHECK(!decodeCursorStreamV2State(NULL, sizeof(kV2State), &state));
}

static void testV2StateTruncationAndFields(void) {
    static const struct {
        size_t offset;
        uint8_t value;
    } bad[] = {
        { 0, 1 },     // version 1
        { 0, 3 },     // a later version
        { 1, 2 },     // shape kind
        { 1, 0 },     // unknown kind
        { 2, 0x40 },  // unknown flag
        { 3, 1 },     // reserved
    };
    uint8_t bytes[CURSOR_STREAM_V2_STATE_SIZE + 4];
    LI_CURSOR_STATE state;
    size_t size;
    size_t i;

    for (size = 0; size <= sizeof(bytes); size++) {
        memset(bytes, 0, sizeof(bytes));
        memcpy(bytes, kV2State, size < sizeof(kV2State) ? size : sizeof(kV2State));
        CHECK(decodeCursorStreamV2State(bytes, size, &state) == (size == sizeof(kV2State)));
    }

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memcpy(bytes, kV2State, sizeof(kV2State));
        bytes[bad[i].offset] = bad[i].value;
        CHECK(!decodeCursorStreamV2State(bytes, sizeof(kV2State), &state));
    }

    memcpy(bytes, kV2State, sizeof(kV2State));
    putLe16(bytes + 12, 0);
    CHECK(!decodeCursorStreamV2State(bytes, sizeof(kV2State), &state));
    memcpy(bytes, kV2State, sizeof(kV2State));
    putLe16(bytes + 14, 0);
    CHECK(!decodeCursorStreamV2State(bytes, sizeof(kV2State), &state));
    memcpy(bytes, kV2State, sizeof(kV2State));
    putLe32(bytes + 16, 0);
    CHECK(!decodeCursorStreamV2State(bytes, sizeof(kV2State), &state));

    // An inverted clip is rejected, but clip fields are ignored without HAS_CLIP
    memcpy(bytes, kV2State, sizeof(kV2State));
    bytes[2] |= LI_CURSOR_STATE_FLAG_HAS_CLIP;
    putLe32(bytes + 28, 10);
    CHECK(!decodeCursorStreamV2State(bytes, sizeof(kV2State), &state));
    bytes[2] &= (uint8_t)~LI_CURSOR_STATE_FLAG_HAS_CLIP;
    CHECK(decodeCursorStreamV2State(bytes, sizeof(kV2State), &state));
}

static void testV2Dispatch(void) {
    static uint8_t chunk[CURSOR_STREAM_V2_SHAPE_HEADER_SIZE + 64];
    CURSOR_STREAM_V2_STATE state;
    CAPTURED_V2 captured = {0};
    size_t size;

    initializeCursorStreamV2State(&state);
    CHECK(feedV2(&state, kV2State, sizeof(kV2State), &captured));
    CHECK(captured.states == 1 && captured.state.shapeId == 0x0a0b0c0d);

    size = makeV2Chunk(chunk, 5, 4, 4, 0, 64, 0x11);
    CHECK(feedV2(&state, chunk, size, &captured));
    CHECK(captured.shapes == 1);
    CHECK(captured.shape.shapeId == 5 && captured.shape.width == 4 && captured.shape.height == 4);
    CHECK(captured.shape.hotspotX == 1 && captured.shape.hotspotY == 2);
    CHECK(captured.first == 0x11 && captured.last == 0x11);
    CHECK(state.pixels == NULL);

    // A NULL callback drops that kind without failing
    CHECK(processCursorStreamV2Packet(&state, kV2State, sizeof(kV2State), NULL, captureV2Shape, &captured));
    CHECK(processCursorStreamV2Packet(&state, chunk, size, captureV2State, NULL, &captured));
    CHECK(captured.states == 1 && captured.shapes == 1);

    // v1 payloads and unknown kinds are not v2 messages
    CHECK(!feedV2(&state, chunk, 1, &captured));
    chunk[0] = CURSOR_STREAM_PROTOCOL_VERSION;
    CHECK(!feedV2(&state, chunk, size, &captured));
    makeV2Chunk(chunk, 5, 4, 4, 0, 64, 0x11);
    chunk[1] = 3;
    CHECK(!feedV2(&state, chunk, size, &captured));
    CHECK(!processCursorStreamV2Packet(&state, NULL, size, captureV2State, captureV2Shape, &captured));
    CHECK(captured.states == 1 && captured.shapes == 1);
    destroyCursorStreamV2State(&state);
}

static void testV2ShapeReassembly(void) {
    static uint8_t chunk[CURSOR_STREAM_V2_SHAPE_HEADER_SIZE + CURSOR_STREAM_V2_MAX_CHUNK + 8];
    const uint32_t total = 256 * 256 * 4;
    CURSOR_STREAM_V2_STATE state;
    CAPTURED_V2 captured = {0};
    uint32_t offset;
    size_t size;

    initializeCursorStreamV2State(&state);

    // The largest shape, in 16 chunks with a distinct fill each
    for (offset = 0; offset < total; offset += CURSOR_STREAM_V2_MAX_CHUNK) {
        size = makeV2Chunk(chunk, 6, 256, 256, offset, CURSOR_STREAM_V2_MAX_CHUNK,
                           (uint8_t)(offset / CURSOR_STREAM_V2_MAX_CHUNK));
        CHECK(feedV2(&state, chunk, size, &captured));
        CHECK(captured.shapes == (offset + CURSOR_STREAM_V2_MAX_CHUNK == total ? 1 : 0));
    }
    CHECK(captured.first == 0 && captured.middle == 8 && captured.last == 15);

    // A chunk that does not continue the shape in flight discards it
    size = makeV2Chunk(chunk, 7, 128, 128, 0, CURSOR_STREAM_V2_MAX_CHUNK, 1);
    CHECK(feedV2(&state, chunk, size, &captured));
    size = makeV2Chunk(chunk, 7, 128, 128, 2 * CURSOR_STREAM_V2_MAX_CHUNK, CURSOR_STREAM_V2_MAX_CHUNK, 1);
    CHECK(!feedV2(&state, chunk, size, &captured));
    CHECK(state.pixels == NULL);
    size = makeV2Chunk(chunk, 7, 128, 128, CURSOR_STREAM_V2_MAX_CHUNK, CURSOR_STREAM_V2_MAX_CHUNK, 1);
    CHECK(!feedV2(&state, chunk, size, &captured));
    size = makeV2Chunk(chunk, 8, 128, 128, 0, CURSOR_STREAM_V2_MAX_CHUNK, 1);
    CHECK(feedV2(&state, chunk, size, &captured));
    size = makeV2Chunk(chunk, 9, 128, 128, CURSOR_STREAM_V2_MAX_CHUNK, CURSOR_STREAM_V2_MAX_CHUNK, 1);
    CHECK(!feedV2(&state, chunk, size, &captured));
    size = makeV2Chunk(chunk, 10, 4, 4, 32, 32, 1);
    CHECK(!feedV2(&state, chunk, size, &captured));

    // A state between chunks does not disturb the assembly
    size = makeV2Chunk(chunk, 11, 128, 128, 0, CURSOR_STREAM_V2_MAX_CHUNK, 2);
    CHECK(feedV2(&state, chunk, size, &captured));
    CHECK(feedV2(&state, kV2State, sizeof(kV2State), &captured));
    size = makeV2Chunk(chunk, 11, 128, 128, CURSOR_STREAM_V2_MAX_CHUNK, CURSOR_STREAM_V2_MAX_CHUNK, 2);
    CHECK(feedV2(&state, chunk, size, &captured));
    CHECK(state.receivedSize == 2 * CURSOR_STREAM_V2_MAX_CHUNK);

    CHECK(captured.shapes == 1);
    destroyCursorStreamV2State(&state);
    CHECK(state.pixels == NULL && state.receivedSize == 0);
}

static void testV2ShapeHeaderValidation(void) {
    static const struct {
        size_t offset;
        uint32_t value;
        int width;
    } bad[] = {
        { 0, 1, 1 },     // version
        { 2, 2, 1 },     // format
        { 3, 1, 1 },     // reserved
        { 26, 1, 2 },    // reserved
        { 4, 0, 4 },     // zero id
        { 8, 0, 2 },     // zero width
        { 8, 257, 2 },   // width over 256
        { 10, 0, 2 },    // zero height
        { 10, 257, 2 },  // height over 256
        { 12, 4, 2 },    // hotspot x outside
        { 14, 4, 2 },    // hotspot y outside
        { 16, 63, 4 },   // total size
        { 20, 64, 4 },   // offset past the end
        { 24, 0, 2 },    // zero length
    };
    static uint8_t chunk[CURSOR_STREAM_V2_SHAPE_HEADER_SIZE + CURSOR_STREAM_V2_MAX_CHUNK + 8];
    CURSOR_STREAM_V2_STATE state;
    CAPTURED_V2 captured = {0};
    size_t size;
    size_t i;

    initializeCursorStreamV2State(&state);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        size = makeV2Chunk(chunk, 12, 4, 4, 0, 64, 1);
        if (bad[i].width == 1) {
            chunk[bad[i].offset] = (uint8_t)bad[i].value;
        }
        else if (bad[i].width == 2) {
            putLe16(chunk + bad[i].offset, (uint16_t)bad[i].value);
        }
        else {
            putLe32(chunk + bad[i].offset, bad[i].value);
        }
        CHECK(!feedV2(&state, chunk, size, &captured));
    }

    size = makeV2Chunk(chunk, 13, 128, 128, 0, CURSOR_STREAM_V2_MAX_CHUNK, 1);
    putLe16(chunk + 24, CURSOR_STREAM_V2_MAX_CHUNK + 1);
    CHECK(!feedV2(&state, chunk, size + 1, &captured));
    size = makeV2Chunk(chunk, 13, 4, 4, 32, 64, 1);
    CHECK(!feedV2(&state, chunk, size, &captured));

    // Every truncation and extension of a valid chunk
    for (size = 0; size <= CURSOR_STREAM_V2_SHAPE_HEADER_SIZE + 64 + 4; size++) {
        int expected = size == CURSOR_STREAM_V2_SHAPE_HEADER_SIZE + 64;
        int before = captured.shapes;

        makeV2Chunk(chunk, 14, 4, 4, 0, 64, 1);
        CHECK(feedV2(&state, chunk, size, &captured) == expected);
        CHECK(captured.shapes == before + expected);
    }

    CHECK(captured.states == 0);
    destroyCursorStreamV2State(&state);
}

static void testV2RequestEncoding(void) {
    static const uint8_t expected[CURSOR_STREAM_V2_REQUEST_SIZE] = {
        0x02, 0x03, 0x00, 0x00, 0x11, 0x22, 0x33, 0x44,
    };
    uint8_t out[CURSOR_STREAM_V2_REQUEST_SIZE];

    encodeCursorStreamV2Request(out, LI_CURSOR_CONTROL_SUBSCRIBE | LI_CURSOR_CONTROL_CLIENT_DRAWS, 0x44332211);
    CHECK(memcmp(out, expected, sizeof(out)) == 0);
    encodeCursorStreamV2Request(out, 0xff, 0);
    CHECK(out[1] == (LI_CURSOR_CONTROL_SUBSCRIBE | LI_CURSOR_CONTROL_CLIENT_DRAWS));
}

static void testV1ParserIgnoresV2(void) {
    static uint8_t chunk[CURSOR_STREAM_V2_SHAPE_HEADER_SIZE + 64];
    CURSOR_STREAM_STATE state;
    CAPTURED_UPDATE captured = {0};
    size_t size = makeV2Chunk(chunk, 5, 4, 4, 0, 64, 0x11);

    initializeCursorStreamState(&state);
    processCursorStreamPacket(&state, kV2State, sizeof(kV2State), captureUpdate, &captured);
    processCursorStreamPacket(&state, chunk, (int)size, captureUpdate, &captured);
    CHECK(captured.count == 0);
    destroyCursorStreamState(&state);
}

int main(void) {
    testVisibilityOnly();
    testSingleChunkShape();
    testMultiChunkShape();
    testOutOfOrderChunkRecovers();
    testMalformedPacketResetsPartialShape();
    testV2StateFields();
    testV2StateTruncationAndFields();
    testV2Dispatch();
    testV2ShapeReassembly();
    testV2ShapeHeaderValidation();
    testV2RequestEncoding();
    testV1ParserIgnoresV2();

    if (failures != 0) {
        fprintf(stderr, "%d cursor protocol golden-vector checks failed\n", failures);
        return 1;
    }

    puts("cursor protocol golden vectors passed");
    return 0;
}
