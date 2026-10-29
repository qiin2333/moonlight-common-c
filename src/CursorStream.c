#include "CursorStream.h"

#include <stdlib.h>
#include <string.h>

#define SS_CURSOR_FLAG_SHAPE 0x01
#define SS_CURSOR_FLAG_VISIBLE 0x02
#define SS_CURSOR_MAX_WIDTH 256
#define SS_CURSOR_MAX_HEIGHT 256
#define SS_CURSOR_MAX_BYTES (SS_CURSOR_MAX_WIDTH * SS_CURSOR_MAX_HEIGHT * 4)

#define CURSOR_WIRE_HEADER_SIZE 28

static uint16_t readLe16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t readLe32(const uint8_t* data) {
    return (uint32_t)data[0] |
           ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) |
           ((uint32_t)data[3] << 24);
}

void initializeCursorStreamState(PCURSOR_STREAM_STATE state) {
    memset(state, 0, sizeof(*state));
}

void destroyCursorStreamState(PCURSOR_STREAM_STATE state) {
    free(state->pixels);
    memset(state, 0, sizeof(*state));
}

void processCursorStreamPacket(PCURSOR_STREAM_STATE state,
                               const uint8_t* payload,
                               int payloadLength,
                               CursorStreamUpdateCallback callback,
                               void* context) {
    LI_CURSOR_UPDATE update;
    uint8_t flags;
    uint16_t headerSize;
    uint32_t shapeId;
    uint16_t width;
    uint16_t height;
    int16_t hotspotX;
    int16_t hotspotY;
    uint32_t totalSize;
    uint32_t offset;
    uint16_t chunkSize;
    bool hasShape;
    bool visible;

    if (state == NULL || payload == NULL || callback == NULL ||
            payloadLength < CURSOR_WIRE_HEADER_SIZE) {
        return;
    }

    flags = payload[1];
    if (payload[0] != CURSOR_STREAM_PROTOCOL_VERSION ||
            (flags & ~(SS_CURSOR_FLAG_SHAPE | SS_CURSOR_FLAG_VISIBLE)) != 0) {
        destroyCursorStreamState(state);
        return;
    }

    headerSize = readLe16(payload + 2);
    shapeId = readLe32(payload + 4);
    width = readLe16(payload + 8);
    height = readLe16(payload + 10);
    hotspotX = (int16_t)readLe16(payload + 12);
    hotspotY = (int16_t)readLe16(payload + 14);
    totalSize = readLe32(payload + 16);
    offset = readLe32(payload + 20);
    chunkSize = readLe16(payload + 24);
    hasShape = (flags & SS_CURSOR_FLAG_SHAPE) != 0;
    visible = (flags & SS_CURSOR_FLAG_VISIBLE) != 0;

    if (headerSize != CURSOR_WIRE_HEADER_SIZE ||
            chunkSize != payloadLength - (int)headerSize) {
        destroyCursorStreamState(state);
        return;
    }

    if (!hasShape) {
        if (width != 0 || height != 0 || totalSize != 0 || offset != 0 || chunkSize != 0) {
            destroyCursorStreamState(state);
            return;
        }

        destroyCursorStreamState(state);
        memset(&update, 0, sizeof(update));
        update.flags = visible ? LI_CURSOR_UPDATE_FLAG_VISIBLE : 0;
        update.shapeId = shapeId;
        callback(&update, context);
        return;
    }

    if (width == 0 || width > SS_CURSOR_MAX_WIDTH ||
            height == 0 || height > SS_CURSOR_MAX_HEIGHT ||
            hotspotX < 0 || hotspotX >= (int16_t)width ||
            hotspotY < 0 || hotspotY >= (int16_t)height ||
            totalSize == 0 || totalSize > SS_CURSOR_MAX_BYTES ||
            totalSize != (uint32_t)width * (uint32_t)height * 4u ||
            chunkSize == 0 || offset > totalSize || chunkSize > totalSize - offset) {
        destroyCursorStreamState(state);
        return;
    }

    if (offset == 0) {
        destroyCursorStreamState(state);
        state->pixels = malloc(totalSize);
        if (state->pixels == NULL) {
            return;
        }

        state->shapeId = shapeId;
        state->totalSize = totalSize;
        state->width = width;
        state->height = height;
        state->hotspotX = hotspotX;
        state->hotspotY = hotspotY;
        state->visible = visible;
    }

    if (state->pixels == NULL ||
            state->shapeId != shapeId ||
            state->totalSize != totalSize ||
            state->width != width ||
            state->height != height ||
            state->hotspotX != hotspotX ||
            state->hotspotY != hotspotY ||
            state->visible != visible ||
            state->receivedSize != offset) {
        destroyCursorStreamState(state);
        return;
    }

    memcpy(state->pixels + offset, payload + headerSize, chunkSize);
    state->receivedSize += chunkSize;
    if (state->receivedSize != state->totalSize) {
        return;
    }

    memset(&update, 0, sizeof(update));
    update.flags = LI_CURSOR_UPDATE_FLAG_SHAPE |
                   (state->visible ? LI_CURSOR_UPDATE_FLAG_VISIBLE : 0);
    update.shapeId = state->shapeId;
    update.width = state->width;
    update.height = state->height;
    update.hotspotX = state->hotspotX;
    update.hotspotY = state->hotspotY;
    update.pixels = state->pixels;
    update.pixelDataLength = state->totalSize;
    callback(&update, context);
    destroyCursorStreamState(state);
}

#define CURSOR_V2_STATE_FLAGS_ALL (LI_CURSOR_STATE_FLAG_VISIBLE | \
                                   LI_CURSOR_STATE_FLAG_FALLBACK | \
                                   LI_CURSOR_STATE_FLAG_HAS_POSITION | \
                                   LI_CURSOR_STATE_FLAG_HAS_CLIP | \
                                   LI_CURSOR_STATE_FLAG_COMPOSITED | \
                                   LI_CURSOR_STATE_FLAG_RESYNC)
#define CURSOR_V2_CONTROL_FLAGS_ALL (LI_CURSOR_CONTROL_SUBSCRIBE | LI_CURSOR_CONTROL_CLIENT_DRAWS)

void initializeCursorStreamV2State(PCURSOR_STREAM_V2_STATE state) {
    memset(state, 0, sizeof(*state));
}

void destroyCursorStreamV2State(PCURSOR_STREAM_V2_STATE state) {
    free(state->pixels);
    memset(state, 0, sizeof(*state));
}

bool decodeCursorStreamV2State(const uint8_t* payload, size_t payloadLength, PLI_CURSOR_STATE state) {
    LI_CURSOR_STATE decoded;

    if (payload == NULL || state == NULL ||
            payloadLength != CURSOR_STREAM_V2_STATE_SIZE ||
            payload[0] != CURSOR_STREAM_V2_PROTOCOL_VERSION ||
            payload[1] != CURSOR_STREAM_V2_KIND_STATE ||
            (payload[2] & ~CURSOR_V2_STATE_FLAGS_ALL) != 0 ||
            payload[3] != 0) {
        return false;
    }

    decoded.flags = payload[2];
    decoded.sequence = readLe32(payload + 4);
    decoded.shapeId = readLe32(payload + 8);
    decoded.referenceWidth = readLe16(payload + 12);
    decoded.referenceHeight = readLe16(payload + 14);
    decoded.scaleQ16 = readLe32(payload + 16);
    decoded.x = (int32_t)readLe32(payload + 20);
    decoded.y = (int32_t)readLe32(payload + 24);
    decoded.clipLeft = (int32_t)readLe32(payload + 28);
    decoded.clipTop = (int32_t)readLe32(payload + 32);
    decoded.clipRight = (int32_t)readLe32(payload + 36);
    decoded.clipBottom = (int32_t)readLe32(payload + 40);

    if (decoded.referenceWidth == 0 || decoded.referenceHeight == 0 || decoded.scaleQ16 == 0) {
        return false;
    }
    if ((decoded.flags & LI_CURSOR_STATE_FLAG_HAS_CLIP) != 0 &&
            (decoded.clipLeft > decoded.clipRight || decoded.clipTop > decoded.clipBottom)) {
        return false;
    }

    *state = decoded;
    return true;
}

static bool cursorV2ShapeChunkValid(const uint8_t* payload, size_t payloadLength) {
    uint32_t width;
    uint32_t height;
    uint32_t totalSize;
    uint32_t offset;
    uint32_t length;

    if (payloadLength < CURSOR_STREAM_V2_SHAPE_HEADER_SIZE ||
            payload[0] != CURSOR_STREAM_V2_PROTOCOL_VERSION ||
            payload[1] != CURSOR_STREAM_V2_KIND_SHAPE ||
            payload[2] != CURSOR_STREAM_V2_FORMAT_BGRA8 ||
            payload[3] != 0 ||
            readLe16(payload + 26) != 0) {
        return false;
    }

    width = readLe16(payload + 8);
    height = readLe16(payload + 10);
    totalSize = readLe32(payload + 16);
    offset = readLe32(payload + 20);
    length = readLe16(payload + 24);

    return readLe32(payload + 4) != 0 &&
           width != 0 && width <= LI_CURSOR_SHAPE_MAX_DIMENSION &&
           height != 0 && height <= LI_CURSOR_SHAPE_MAX_DIMENSION &&
           readLe16(payload + 12) < width &&
           readLe16(payload + 14) < height &&
           totalSize == width * height * 4 &&
           length != 0 && length <= CURSOR_STREAM_V2_MAX_CHUNK &&
           payloadLength == CURSOR_STREAM_V2_SHAPE_HEADER_SIZE + (size_t)length &&
           offset <= totalSize && length <= totalSize - offset;
}

static bool processCursorStreamV2Shape(PCURSOR_STREAM_V2_STATE state,
                                       const uint8_t* payload,
                                       size_t payloadLength,
                                       CursorStreamShapeCallback callback,
                                       void* context) {
    LI_CURSOR_SHAPE shape;
    uint32_t shapeId;
    uint16_t width;
    uint16_t height;
    uint16_t hotspotX;
    uint16_t hotspotY;
    uint32_t totalSize;
    uint32_t offset;
    uint16_t length;

    if (!cursorV2ShapeChunkValid(payload, payloadLength)) {
        destroyCursorStreamV2State(state);
        return false;
    }

    shapeId = readLe32(payload + 4);
    width = readLe16(payload + 8);
    height = readLe16(payload + 10);
    hotspotX = readLe16(payload + 12);
    hotspotY = readLe16(payload + 14);
    totalSize = readLe32(payload + 16);
    offset = readLe32(payload + 20);
    length = readLe16(payload + 24);

    if (offset == 0) {
        uint8_t* pixels = realloc(state->pixels, totalSize);
        if (pixels == NULL) {
            destroyCursorStreamV2State(state);
            return false;
        }

        state->pixels = pixels;
        state->shapeId = shapeId;
        state->totalSize = totalSize;
        state->receivedSize = 0;
        state->width = width;
        state->height = height;
        state->hotspotX = hotspotX;
        state->hotspotY = hotspotY;
    }
    else if (state->pixels == NULL ||
             state->shapeId != shapeId ||
             state->totalSize != totalSize ||
             state->width != width ||
             state->height != height ||
             state->hotspotX != hotspotX ||
             state->hotspotY != hotspotY ||
             state->receivedSize != offset) {
        // Not the continuation of the shape in flight
        destroyCursorStreamV2State(state);
        return false;
    }

    memcpy(state->pixels + offset, payload + CURSOR_STREAM_V2_SHAPE_HEADER_SIZE, length);
    state->receivedSize += length;
    if (state->receivedSize != state->totalSize) {
        return true;
    }

    if (callback != NULL) {
        shape.shapeId = state->shapeId;
        shape.width = state->width;
        shape.height = state->height;
        shape.hotspotX = state->hotspotX;
        shape.hotspotY = state->hotspotY;
        shape.pixels = state->pixels;
        callback(&shape, context);
    }
    destroyCursorStreamV2State(state);
    return true;
}

bool processCursorStreamV2Packet(PCURSOR_STREAM_V2_STATE state,
                                 const uint8_t* payload,
                                 size_t payloadLength,
                                 CursorStreamStateCallback stateCallback,
                                 CursorStreamShapeCallback shapeCallback,
                                 void* context) {
    if (state == NULL || payload == NULL || payloadLength < 2 ||
            payload[0] != CURSOR_STREAM_V2_PROTOCOL_VERSION) {
        return false;
    }

    if (payload[1] == CURSOR_STREAM_V2_KIND_STATE) {
        LI_CURSOR_STATE decoded;

        if (!decodeCursorStreamV2State(payload, payloadLength, &decoded)) {
            return false;
        }
        if (stateCallback != NULL) {
            stateCallback(&decoded, context);
        }
        return true;
    }
    else if (payload[1] == CURSOR_STREAM_V2_KIND_SHAPE) {
        return processCursorStreamV2Shape(state, payload, payloadLength, shapeCallback, context);
    }

    return false;
}

void encodeCursorStreamV2Request(uint8_t* out, uint8_t flags, uint32_t resendShapeId) {
    out[0] = CURSOR_STREAM_V2_PROTOCOL_VERSION;
    out[1] = (uint8_t)(flags & CURSOR_V2_CONTROL_FLAGS_ALL);
    out[2] = 0;
    out[3] = 0;
    out[4] = (uint8_t)resendShapeId;
    out[5] = (uint8_t)(resendShapeId >> 8);
    out[6] = (uint8_t)(resendShapeId >> 16);
    out[7] = (uint8_t)(resendShapeId >> 24);
}
