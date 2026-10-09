// Include the production depacketizer to exercise the exact Annex-B NAL
// scanning used by IDR validation. The test only calls static helpers; linker
// dead stripping removes the network and queue paths from this harness.
#include "../src/VideoDepacketizer.c"

#include <stdio.h>
#include <stdlib.h>

int NegotiatedVideoFormat;
DECODER_RENDERER_CALLBACKS VideoCallbacks;

int main(void) {
    // Prefix SEI (type 39), followed by the VPS (type 32) that starts an IDR.
    // This is the layout emitted by hosts carrying HDR10+ metadata.
    char data[] = {
        0, 0, 0, 1, (char)(39 << 1), 0x01,
        0, 0, 0, 1, (char)(32 << 1), 0x01,
    };
    BUFFER_DESC descriptor = { data, 0, (unsigned int)sizeof(data) };

    NegotiatedVideoFormat = VIDEO_FORMAT_H265;
    VideoCallbacks.capabilities = CAPABILITY_PRESERVE_HEVC_SEI;
    if (!isIdrFrameStartAfterPreservedSei(&descriptor)) {
        fprintf(stderr, "FAIL: preserved HEVC SEI must not hide IDR VPS\n");
        return EXIT_FAILURE;
    }

    data[4] = (char)(40 << 1);
    if (!shouldPreserveHevcSei(&descriptor)) {
        fprintf(stderr, "FAIL: suffix HEVC SEI should be preserved\n");
        return EXIT_FAILURE;
    }

    VideoCallbacks.capabilities = 0;
    if (shouldPreserveHevcSei(&descriptor)) {
        fprintf(stderr, "FAIL: renderer without opt-in must not preserve SEI\n");
        return EXIT_FAILURE;
    }

    puts("HEVC SEI tests passed");
    return EXIT_SUCCESS;
}
