// Exercise the response-option path used by RTSP ANNOUNCE negotiation. The
// production parser is static, so this harness includes that translation unit
// and dead-strips unrelated networking code at link time.
#include "../src/RtspConnection.c"

#include <stdio.h>
#include <stdlib.h>

CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
int NegotiatedDynamicHdrFormat;
int NegotiatedDynamicHdrFallback;

static void require(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void parseOptions(const char* format, const char* fallback) {
    OPTION_ITEM formatOption = { 0 };
    OPTION_ITEM fallbackOption = { 0 };
    RTSP_MESSAGE response = { 0 };

    formatOption.option = "X-SS-Dynamic-HDR";
    formatOption.content = (char*)format;
    formatOption.next = fallback == NULL ? NULL : &fallbackOption;
    fallbackOption.option = "X-SS-Dynamic-HDR-Fallback";
    fallbackOption.content = (char*)fallback;
    response.options = &formatOption;
    if (format == NULL) {
        response.options = fallback == NULL ? NULL : &fallbackOption;
    }

    parseDynamicHdrNegotiation(&response);
}

int main(void) {
    parseOptions(NULL, NULL);
    require(NegotiatedDynamicHdrFormat == DYNAMIC_HDR_FORMAT_NONE &&
                NegotiatedDynamicHdrFallback == DYNAMIC_HDR_FALLBACK_NONE,
            "missing response headers are safe");

    parseOptions("1junk", "5");
    require(NegotiatedDynamicHdrFormat == DYNAMIC_HDR_FORMAT_NONE &&
                NegotiatedDynamicHdrFallback == DYNAMIC_HDR_FALLBACK_DIRECT_SURFACE_MISSING,
            "malformed numeric format is rejected while fallback parses");

    parseOptions("99", "unknown_reason");
    require(NegotiatedDynamicHdrFormat == DYNAMIC_HDR_FORMAT_NONE &&
                NegotiatedDynamicHdrFallback == DYNAMIC_HDR_FALLBACK_NONE,
            "unknown values are safely ignored");

    parseOptions("4", "codec_unsupported");
    require(NegotiatedDynamicHdrFormat == DYNAMIC_HDR_FORMAT_DOLBY_VISION_PROFILE_81 &&
                NegotiatedDynamicHdrFallback == DYNAMIC_HDR_FALLBACK_CODEC_UNSUPPORTED,
            "valid host response is parsed");

    puts("RTSP dynamic HDR response tests passed");
    return EXIT_SUCCESS;
}

