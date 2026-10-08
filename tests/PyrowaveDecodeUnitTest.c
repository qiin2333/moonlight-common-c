// Exercise the real owned-frame/queued DECODE_UNIT path without a network session.
#define notifyKeyFrameReceived testNotifyKeyFrameReceived
#define connectionReceivedCompleteFrame testConnectionReceivedCompleteFrame
#define LiGetCurrentHostDisplayHdrMode testGetCurrentHostDisplayHdrMode
#include "../src/VideoDepacketizer.c"
#include "../src/DynamicHdr.h"

#include <stdio.h>
#include <string.h>

void testNotifyKeyFrameReceived(void) {}
void testConnectionReceivedCompleteFrame(uint32_t frameNumber, bool frameIsLTR) {
    (void)frameNumber;
    (void)frameIsLTR;
}
bool testGetCurrentHostDisplayHdrMode(void) { return true; }

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); return 1; \
} } while (0)

int main(void) {
    static const uint8_t codec[] = {1, 2, 3, 4};
    static const uint8_t metadata[] = {0, 4, 0, 9, 0, 0, 0, 1, 42};
    VIDEO_FRAME_HANDLE frameHandle;
    PDECODE_UNIT decodeUnit;
    NegotiatedVideoFormat = VIDEO_FORMAT_PYROWAVE;
    NegotiatedDynamicHdrFormat = DYNAMIC_HDR_FORMAT_VIVID_PQ;
    StreamConfig.hdrMode = 1;
    StreamConfig.fps = 60;
    VideoCallbacks.capabilities = CAPABILITY_PULL_RENDERER;
    initializeVideoDepacketizer(1392);
    for (unsigned frame = 1; frame <= 2; ++frame) {
        uint8_t* allocation = malloc(sizeof(PYROWAVE_FRAME_ENTRY) + sizeof(codec) + sizeof(metadata));
        CHECK(allocation != NULL);
        memcpy(allocation + sizeof(PYROWAVE_FRAME_ENTRY), codec, sizeof(codec));
        memcpy(allocation + sizeof(PYROWAVE_FRAME_ENTRY) + sizeof(codec), metadata, sizeof(metadata));
        allocation[sizeof(PYROWAVE_FRAME_ENTRY) + sizeof(codec) + sizeof(metadata) - 1] = (uint8_t)frame;
        CHECK(queueOwnedFrame(allocation, sizeof(codec), sizeof(metadata)));
        frameType = FRAME_TYPE_IDR;
        nextFrameNumber = frame + 1;
        reassembleFrame(frame, false);
    }
    // Reset/reuse receive-side state after queueing: both queued frames must retain
    // their own metadata, not a pointer into the reassembler or the next frame.
    LiPyrowaveReassemblyReset(&pyrowaveReassembly);
    for (unsigned frame = 1; frame <= 2; ++frame) {
        CHECK(LiPollNextVideoFrame(&frameHandle, &decodeUnit));
        CHECK(decodeUnit->frameNumber == (int)frame);
        CHECK(decodeUnit->fullLength == sizeof(codec));
        CHECK(decodeUnit->pyrowaveMetadataLength == sizeof(metadata));
        CHECK(decodeUnit->pyrowaveMetadata[sizeof(metadata) - 1] == (uint8_t)frame);
        CHECK(memcmp(decodeUnit->bufferList->data, codec, sizeof(codec)) == 0);
        LiCompleteVideoFrame(frameHandle, DR_OK);
    }
    CHECK(!LiPollNextVideoFrame(&frameHandle, &decodeUnit));

    CHECK(LiPyrowaveDynamicHdrMetadataType(DYNAMIC_HDR_FORMAT_NONE) == 0);
    CHECK(LiPyrowaveDynamicHdrMetadataType(-1) == 0);
    CHECK(LiPyrowaveDynamicHdrMetadataType(6) == 0);
    // Validate all negotiated dynamic formats using the production TLV parser.
    for (int format = DYNAMIC_HDR_FORMAT_HDR10_PLUS;
            format <= DYNAMIC_HDR_FORMAT_DOLBY_VISION_PROFILE_84; ++format) {
        const bool hlg = format == DYNAMIC_HDR_FORMAT_VIVID_HLG ||
                         format == DYNAMIC_HDR_FORMAT_DOLBY_VISION_PROFILE_84;
        const uint16_t type = LiPyrowaveDynamicHdrMetadataType(format);
        uint8_t tlv[] = {0, (uint8_t)type, 0, 9, 0, 0, 0, 1, 42,
                        0, 6, 0, 9, 0, 0, 0, 2, 3, 232};
        uint8_t duplicate[sizeof(tlv) * 2];
        const size_t length = hlg ? sizeof(tlv) : 9;
        NegotiatedDynamicHdrFormat = format;
        StreamConfig.hdrMode = hlg ? 2 : 1;
        CHECK(type != 0);
        CHECK(processPyrowaveMetadata(tlv, length, LI_PYROWAVE_METADATA_FLAG_PROTECTED));
        CHECK(!processPyrowaveMetadata(NULL, 0, 0));
        memcpy(duplicate, tlv, length);
        memcpy(duplicate + length, tlv, length);
        CHECK(!processPyrowaveMetadata(duplicate, length * 2, LI_PYROWAVE_METADATA_FLAG_PROTECTED));
        tlv[3] = LI_PYROWAVE_METADATA_FLAG_PROTECTED;
        CHECK(!processPyrowaveMetadata(tlv, length, LI_PYROWAVE_METADATA_FLAG_PROTECTED));
        tlv[3] |= LI_PYROWAVE_METADATA_FLAG_OPTIONAL;
        CHECK(!processPyrowaveMetadata(tlv, length, LI_PYROWAVE_METADATA_FLAG_PROTECTED));
        tlv[3] = LI_PYROWAVE_METADATA_FLAG_REQUIRED;
        CHECK(!processPyrowaveMetadata(tlv, length, LI_PYROWAVE_METADATA_FLAG_PROTECTED));
        tlv[3] |= LI_PYROWAVE_METADATA_FLAG_PROTECTED;
        tlv[1] = type == LI_PYROWAVE_METADATA_HDR10_PLUS
                   ? LI_PYROWAVE_METADATA_HDR_VIVID : LI_PYROWAVE_METADATA_HDR10_PLUS;
        CHECK(!processPyrowaveMetadata(tlv, length, LI_PYROWAVE_METADATA_FLAG_PROTECTED));
        tlv[1] = (uint8_t)type;
        if (hlg) {
            uint8_t duplicatePeak[sizeof(tlv) + 10];
            CHECK(!processPyrowaveMetadata(tlv, 9, LI_PYROWAVE_METADATA_FLAG_PROTECTED));
            tlv[18] = tlv[17] = 0;
            CHECK(!processPyrowaveMetadata(tlv, length, LI_PYROWAVE_METADATA_FLAG_PROTECTED));
            tlv[17] = 3;
            tlv[18] = 232;
            tlv[12] = LI_PYROWAVE_METADATA_FLAG_PROTECTED;
            CHECK(!processPyrowaveMetadata(tlv, length, LI_PYROWAVE_METADATA_FLAG_PROTECTED));
            tlv[12] |= LI_PYROWAVE_METADATA_FLAG_REQUIRED;
            memcpy(duplicatePeak, tlv, sizeof(tlv));
            memcpy(duplicatePeak + sizeof(tlv), tlv + 9, 10);
            CHECK(!processPyrowaveMetadata(duplicatePeak, sizeof(duplicatePeak),
                                          LI_PYROWAVE_METADATA_FLAG_PROTECTED));
        }
        else {
            CHECK(!processPyrowaveMetadata(tlv, sizeof(tlv), LI_PYROWAVE_METADATA_FLAG_PROTECTED));
        }
    }
    NegotiatedDynamicHdrFormat = DYNAMIC_HDR_FORMAT_NONE;
    CHECK(processPyrowaveMetadata(NULL, 0, 0));
    CHECK(frameHostProcessingLatency == 0);
    destroyVideoDepacketizer();
    puts("PASS: same-frame metadata ownership and production depacketizer validation");
    return 0;
}
