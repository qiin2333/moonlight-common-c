#pragma once

#include "Limelight.h"

#include <stddef.h>

#define CURSOR_STREAM_PROTOCOL_VERSION 1

// Cursor protocol v2 (0x5509 with version 2). A session that subscribes with a
// version 2 request receives states and chunked shapes on CTRL_CHANNEL_CURSOR.
#define CURSOR_STREAM_V2_PROTOCOL_VERSION 2
#define CURSOR_STREAM_V2_KIND_STATE 1
#define CURSOR_STREAM_V2_KIND_SHAPE 2
#define CURSOR_STREAM_V2_FORMAT_BGRA8 1
#define CURSOR_STREAM_V2_STATE_SIZE 44
#define CURSOR_STREAM_V2_SHAPE_HEADER_SIZE 28
#define CURSOR_STREAM_V2_MAX_CHUNK 16384
#define CURSOR_STREAM_V2_REQUEST_SIZE 8

typedef struct _CURSOR_STREAM_STATE {
    uint8_t* pixels;
    uint32_t shapeId;
    uint32_t totalSize;
    uint32_t receivedSize;
    uint16_t width;
    uint16_t height;
    int16_t hotspotX;
    int16_t hotspotY;
    bool visible;
} CURSOR_STREAM_STATE, *PCURSOR_STREAM_STATE;

typedef void(*CursorStreamUpdateCallback)(const LI_CURSOR_UPDATE* update, void* context);

void initializeCursorStreamState(PCURSOR_STREAM_STATE state);
void destroyCursorStreamState(PCURSOR_STREAM_STATE state);
void processCursorStreamPacket(PCURSOR_STREAM_STATE state,
                               const uint8_t* payload,
                               int payloadLength,
                               CursorStreamUpdateCallback callback,
                               void* context);

// One v2 shape in flight at a time: chunks arrive in order on a reliable channel.
typedef struct _CURSOR_STREAM_V2_STATE {
    uint8_t* pixels;
    uint32_t shapeId;
    uint32_t totalSize;
    uint32_t receivedSize;
    uint16_t width;
    uint16_t height;
    uint16_t hotspotX;
    uint16_t hotspotY;
} CURSOR_STREAM_V2_STATE, *PCURSOR_STREAM_V2_STATE;

typedef void(*CursorStreamStateCallback)(const LI_CURSOR_STATE* state, void* context);
typedef void(*CursorStreamShapeCallback)(const LI_CURSOR_SHAPE* shape, void* context);

void initializeCursorStreamV2State(PCURSOR_STREAM_V2_STATE state);
void destroyCursorStreamV2State(PCURSOR_STREAM_V2_STATE state);

// Parses one v2 state payload (after the control header). Returns false for
// anything that is not exactly a valid version 2 state.
bool decodeCursorStreamV2State(const uint8_t* payload, size_t payloadLength, PLI_CURSOR_STATE state);

// Processes one v2 payload: a state is delivered at once, a shape once its last
// chunk arrives. A NULL callback drops that kind. Returns false for a malformed
// message, which also discards a partial shape.
bool processCursorStreamV2Packet(PCURSOR_STREAM_V2_STATE state,
                                 const uint8_t* payload,
                                 size_t payloadLength,
                                 CursorStreamStateCallback stateCallback,
                                 CursorStreamShapeCallback shapeCallback,
                                 void* context);

// Encodes the 8-byte version 2 request. Unknown flags are not sent.
void encodeCursorStreamV2Request(uint8_t* out, uint8_t flags, uint32_t resendShapeId);
