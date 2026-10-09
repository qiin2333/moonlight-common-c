#include "Limelight-internal.h"
#include "DynamicHdr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// SdpGenerator.c is intentionally linked into this small test so that the
// optional attributes are checked on the actual serialized SDP payload.
char* getSdpPayloadForStreamConfig(int rtspClientVersion, int* length);

char* RemoteAddrString;
struct sockaddr_storage RemoteAddr;
struct sockaddr_storage LocalAddr;
SOCKADDR_LEN AddrLen;
int AppVersionQuad[4];
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;
DECODER_RENDERER_CALLBACKS VideoCallbacks;
AUDIO_RENDERER_CALLBACKS AudioCallbacks;
int NegotiatedVideoFormat;
volatile bool ConnectionInterrupted;
bool HighQualitySurroundSupported;
bool HighQualitySurroundEnabled;
OPUS_MULTISTREAM_CONFIGURATION NormalQualityOpusConfig;
OPUS_MULTISTREAM_CONFIGURATION HighQualityOpusConfig;
int AudioPacketDuration;
bool AudioEncryptionEnabled;
bool ReferenceFrameInvalidationSupported;
uint32_t EncryptionFeaturesSupported;
uint32_t EncryptionFeaturesRequested;
uint32_t EncryptionFeaturesEnabled;
uint16_t RtspPortNumber = 47984;
uint16_t ControlPortNumber;
uint16_t AudioPortNumber;
uint16_t VideoPortNumber = 47998;
uint16_t MicPortNumber;
SS_PING AudioPingPayload;
SS_PING VideoPingPayload;
SS_PING MicPingPayload;
uint32_t ControlConnectData;
uint32_t SunshineFeatureFlags;
bool SunshinePenBarrelRollSupported;

void addrToUrlSafeString(struct sockaddr_storage* address, char* string, size_t stringLen) {
    (void)address;
    snprintf(string, stringLen, "127.0.0.1");
}

bool isReferenceFrameInvalidationSupportedByDecoder(void) {
    return false;
}

bool PltSafeStrcpy(char* dst, size_t dstLen, const char* src) {
    if (dstLen != 0) {
        snprintf(dst, dstLen, "%s", src);
    }
    return dstLen != 0;
}

static void require(bool condition, const char* message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(EXIT_FAILURE);
    }
}

static void testFormatParsing(void) {
    require(parseDynamicHdrFormatValue(NULL) == DYNAMIC_HDR_FORMAT_NONE, "missing format is safe");
    require(parseDynamicHdrFormatValue("1") == DYNAMIC_HDR_FORMAT_HDR10_PLUS, "HDR10+ numeric format");
    require(parseDynamicHdrFormatValue(" 5 \t") == DYNAMIC_HDR_FORMAT_DOLBY_VISION_PROFILE_84,
            "surrounding whitespace is accepted");
    require(parseDynamicHdrFormatValue("1junk") == DYNAMIC_HDR_FORMAT_NONE, "format suffix is rejected");
    require(parseDynamicHdrFormatValue("\n1") == DYNAMIC_HDR_FORMAT_NONE, "vertical whitespace is rejected");
    require(parseDynamicHdrFormatValue("99") == DYNAMIC_HDR_FORMAT_NONE, "unknown format is rejected");
    require(parseDynamicHdrFormatValue("dolby_vision_profile_81") == DYNAMIC_HDR_FORMAT_DOLBY_VISION_PROFILE_81,
            "textual format compatibility");
}

static void testFallbackParsing(void) {
    require(parseDynamicHdrFallbackValue(NULL) == DYNAMIC_HDR_FALLBACK_NONE, "missing fallback is safe");
    require(parseDynamicHdrFallbackValue("codec_unsupported") == DYNAMIC_HDR_FALLBACK_CODEC_UNSUPPORTED,
            "textual fallback");
    require(parseDynamicHdrFallbackValue(" 5 ") == DYNAMIC_HDR_FALLBACK_DIRECT_SURFACE_MISSING,
            "numeric fallback compatibility");
    require(parseDynamicHdrFallbackValue("5junk") == DYNAMIC_HDR_FALLBACK_NONE,
            "numeric fallback suffix is rejected");
    require(parseDynamicHdrFallbackValue("1") == DYNAMIC_HDR_FALLBACK_NONE, "retired fallback is rejected");
    require(parseDynamicHdrFallbackValue("codec_unsupportedjunk") == DYNAMIC_HDR_FALLBACK_NONE,
            "fallback suffix is rejected");
}

static void testSdpAttributes(void) {
    char* payload;
    int payloadLength;

    memset(&StreamConfig, 0, sizeof(StreamConfig));
    memset(&VideoCallbacks, 0, sizeof(VideoCallbacks));
    memset(&AudioCallbacks, 0, sizeof(AudioCallbacks));
    memset(&ListenerCallbacks, 0, sizeof(ListenerCallbacks));
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 500;
    AppVersionQuad[3] = -1; // Sunshine
    NegotiatedVideoFormat = VIDEO_FORMAT_H265 | VIDEO_FORMAT_H265_MAIN10;
    StreamConfig.width = 1920;
    StreamConfig.height = 1080;
    StreamConfig.fps = 60;
    StreamConfig.bitrate = 10000;
    StreamConfig.packetSize = 1024;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;

    // All fields zero preserve legacy output.
    payload = getSdpPayloadForStreamConfig(0, &payloadLength);
    require(payload != NULL, "legacy SDP generated");
    require(strstr(payload, "dynamicHdrCaps") == NULL, "legacy SDP omits dynamic HDR");
    free(payload);

    StreamConfig.dynamicHdrCaps = DYNAMIC_HDR_CAPS_HDR10_PLUS | DYNAMIC_HDR_CAPS_DOLBY_VISION_81;
    StreamConfig.dolbyVisionDirectSurface = 1;
    StreamConfig.dynamicHdrPreference = DYNAMIC_HDR_PREFERENCE_DOLBY_VISION;
    payload = getSdpPayloadForStreamConfig(0, &payloadLength);
    require(payload != NULL, "dynamic HDR SDP generated");
    require(strstr(payload, "a=x-ss-video[0].dynamicHdrCaps:9 \r\n") != NULL,
            "dynamic HDR caps serialized");
    require(strstr(payload, "a=x-ss-video[0].dolbyVisionDirectSurface:1 \r\n") != NULL,
            "direct surface serialized");
    require(strstr(payload, "a=x-ss-video[0].dynamicHdrPreference:1 \r\n") != NULL,
            "dynamic HDR preference serialized");
    free(payload);
}

int main(void) {
    testFormatParsing();
    testFallbackParsing();
    testSdpAttributes();
    puts("dynamic HDR tests passed");
    return EXIT_SUCCESS;
}
