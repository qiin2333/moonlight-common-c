#pragma once

#include "Limelight.h"

#define DS5_HAPTICS_IR_STREAM_PROTOCOL_VERSION 2
#define DS5_HAPTICS_IR_STREAM_WIRE_SIZE 72
#define DS5_HAPTICS_IR_V3_STREAM_PROTOCOL_VERSION 3
#define DS5_HAPTICS_IR_V3_STREAM_PACKET_TYPE 0x5511
#define DS5_HAPTICS_IR_V3_STREAM_BASE_SIZE 64
#define DS5_HAPTICS_IR_V3_STREAM_EVENT_SIZE 32
#define DS5_HAPTICS_IR_V3_STREAM_MAX_SIZE (DS5_HAPTICS_IR_V3_STREAM_BASE_SIZE + LI_DS5_HAPTICS_IR_V3_MAX_EVENTS * DS5_HAPTICS_IR_V3_STREAM_EVENT_SIZE)

typedef void(*Ds5HapticsIrStreamCallback)(const LI_DS5_HAPTICS_IR_FRAME_V2* frame,
                                          void* context);

bool processDs5HapticsIrStreamPacket(const uint8_t* payload,
                                     int payloadLength,
                                     Ds5HapticsIrStreamCallback callback,
                                     void* context);

typedef void(*Ds5HapticsIrV3StreamCallback)(const LI_DS5_HAPTICS_IR_FRAME_V3* frame,
                                          void* context);

bool processDs5HapticsIrV3StreamPacket(const uint8_t* payload,
                                      int payloadLength,
                                      Ds5HapticsIrV3StreamCallback callback,
                                      void* context);
