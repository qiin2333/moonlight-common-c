#include "Limelight-internal.h"
#include "TransportFeedbackWire.h"
#include <stdio.h>

static unsigned checks;
static unsigned failures;
static unsigned scenarios;
#define CHECK(expression) do { checks++; if (!(expression)) { failures++; fprintf(stderr, "line %d: %s\n", __LINE__, #expression); } } while (0)

static void configure(bool control, bool feedback, uint32_t offeredControl, uint32_t offeredFeedback,
                      uint32_t supportedEncryption, bool controlOnly) {
    endVideoPacketFeedbackConnection();
    CHECK(LiSetVideoPacketControlEnabled(control));
    CHECK(LiSetVideoPacketFeedbackEnabled(feedback));
    LiInitializeStreamConfiguration(&StreamConfig);
    StreamConfig.width = 1280;
    StreamConfig.height = 720;
    StreamConfig.fps = 30;
    StreamConfig.bitrate = 10000;
    StreamConfig.packetSize = 1392;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    StreamConfig.controlOnly = controlOnly;
    EncryptionFeaturesSupported = supportedEncryption;
    EncryptionFeaturesRequested = 0;
    EncryptionFeaturesEnabled = 0;
    VideoPacketControlSupportedVersion = offeredControl;
    VideoPacketFeedbackSupportedVersion = offeredFeedback;
    VideoPacketFeedbackConnectionEpoch = 0;
}

static void announceScenario(const char* name, bool control, bool feedback, uint32_t offeredControl,
                             uint32_t offeredFeedback, uint32_t encryption, bool controlOnly,
                             bool expectControl, bool expectFeedback, int expectPacketSize) {
    scenarios++;
    configure(control, feedback, offeredControl, offeredFeedback, encryption, controlOnly);
    beginVideoPacketFeedbackConnection();
    int length = 0;
    char* sdp = getSdpPayloadForStreamConfig(14, &length);
    CHECK(sdp != NULL);
    if (sdp != NULL) {
        CHECK(length > 0);
        CHECK((strstr(sdp, "a=x-ss-video[0].packetControlVersion:1 ") != NULL) == expectControl);
        CHECK((strstr(sdp, "a=x-ss-video[0].packetFeedbackVersion:2 ") != NULL) == expectFeedback);
        CHECK(StreamConfig.packetSize == expectPacketSize);
        free(sdp);
    }
    // An offer/request is never a negotiated control lease.
    CHECK(!LiGetVideoPacketControlNegotiated());
    endVideoPacketFeedbackConnection();
    printf("{\"scenario\":\"%s\",\"control_attribute\":%s,\"feedback_attribute\":%s,\"packet_size\":%d}\n",
           name, expectControl ? "true" : "false", expectFeedback ? "true" : "false", StreamConfig.packetSize);
}

static void testStrictCapabilityParser(void) {
    scenarios++;
    CHECK(parseVideoPacketControlSupportedVersion(NULL) == 0);
    CHECK(parseVideoPacketControlSupportedVersion("a=x-ss-video[0].packetFeedbackVersion:2\r\n") == 0);
    CHECK(parseVideoPacketControlSupportedVersion("a=x-ss-video[0].packetControlVersion:1\r\n") == 1);
    CHECK(parseVideoPacketControlSupportedVersion("v=0\r\na=x-ss-video[0].packetControlVersion:1 \r\n") == 1);
    CHECK(parseVideoPacketControlSupportedVersion("a=x-ss-video[0].packetControlVersion:1") == 1);
    CHECK(parseVideoPacketControlSupportedVersion("a=x-ss-video[0].packetControlVersion:0\r\n") == 0);
    CHECK(parseVideoPacketControlSupportedVersion("a=x-ss-video[0].packetControlVersion:2\r\n") == 0);
    CHECK(parseVideoPacketControlSupportedVersion("a=x-ss-video[0].packetControlVersion:01\r\n") == 0);
    CHECK(parseVideoPacketControlSupportedVersion("a=x-ss-video[0].packetControlVersion:1unknown\r\n") == 0);
    CHECK(parseVideoPacketControlSupportedVersion("a=unknownx-ss-video[0].packetControlVersion:1\r\n") == 0);
    CHECK(parseVideoPacketControlSupportedVersion("a=x-ss-video[0].packetControlVersion:1\r\na=x-ss-video[0].packetControlVersion:0\r\n") == 0);
    CHECK(parseVideoPacketControlSupportedVersion("a=x-ss-video[0].packetControlVersion:1\r\na=x-ss-video[0].packetControlVersion:1\r\n") == 0);
}

static void testProbePaddingNegotiation(void) {
    scenarios++;
    CHECK(parseVideoProbePaddingSupportedVersion(NULL) == 0);
    CHECK(parseVideoProbePaddingSupportedVersion("a=x-ss-video[0].packetProbeVersion:1\r\n") == 1);
    const char* invalid[] = {"a=x-ss-video[0].packetProbeVersion:01\r\n", "a=x-ss-video[0].packetProbeVersion:2\r\n",
        "a=x-ss-video[0].packetProbeVersion:1x\r\n", "a=x-ss-video[0].packetProbeVersion:1\r\na=x-ss-video[0].packetProbeVersion:1\r\n"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        CHECK(parseVideoProbePaddingSupportedVersion(invalid[i]) == 0);
    for (unsigned capability = 0; capability <= 2; ++capability) {
        configure(true, true, 1, 2, SS_ENC_VIDEO | SS_ENC_CONTROL_V2, false);
        VideoProbePaddingSupportedVersion = capability;
        beginVideoPacketFeedbackConnection();
        int length = 0;
        char* sdp = getSdpPayloadForStreamConfig(14, &length);
        CHECK(sdp != NULL);
        if (sdp) {
            CHECK((strstr(sdp, "a=x-ss-video[0].packetProbeVersion:1 ") != NULL) == (capability == 1));
            free(sdp);
        }
        CHECK(!isVideoProbePaddingNegotiated());
        confirmVideoProbePaddingNegotiation("1");
        CHECK(!isVideoProbePaddingNegotiated()); // An acknowledged feedback epoch is required.
        VideoPacketFeedbackConnectionEpoch = 42;
        const char* rejected[] = {NULL, "0", "2", "01", "1x", "1 "};
        for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); ++i) {
            confirmVideoProbePaddingNegotiation(rejected[i]);
            CHECK(!isVideoProbePaddingNegotiated());
        }
        confirmVideoProbePaddingNegotiation("1");
        CHECK(isVideoProbePaddingNegotiated() == (capability == 1));
        resetVideoPacketControlNegotiation();
        CHECK(!isVideoProbePaddingNegotiated());
        endVideoPacketFeedbackConnection();
    }
    for (unsigned gate = 0; gate < 5; ++gate) {
        configure(false, gate != 0, 1, gate == 1 ? 1 : 2, SS_ENC_VIDEO | SS_ENC_CONTROL_V2, gate == 2);
        VideoProbePaddingSupportedVersion = 1;
        EncryptionFeaturesEnabled = gate == 3 ? SS_ENC_VIDEO : gate == 4 ? SS_ENC_CONTROL_V2 : SS_ENC_VIDEO | SS_ENC_CONTROL_V2;
        beginVideoPacketFeedbackConnection();
        VideoPacketFeedbackConnectionEpoch = 42;
        CHECK(!shouldAnnounceVideoProbePadding());
        confirmVideoProbePaddingNegotiation("1");
        CHECK(!isVideoProbePaddingNegotiated());
        endVideoPacketFeedbackConnection();
    }
    VideoProbePaddingSupportedVersion = 0;
}

static void testAcknowledgementAndLifecycle(void) {
    scenarios++;
    configure(true, true, 1, 2, SS_ENC_VIDEO | SS_ENC_CONTROL_V2, false);
    EncryptionFeaturesEnabled = SS_ENC_VIDEO | SS_ENC_CONTROL_V2;
    beginVideoPacketFeedbackConnection();
    CHECK(!LiSetVideoPacketControlEnabled(false));
    CHECK(!LiSetVideoPacketFeedbackEnabled(false));
    CHECK(!LiSetVideoNetworkObservationEnabled(false));
    CHECK(isVideoPacketControlRequested());
    CHECK(isVideoPacketFeedbackRequested());
    confirmVideoPacketControlNegotiation("1");
    CHECK(!LiGetVideoPacketControlNegotiated()); // No acknowledged feedback epoch.
    VideoPacketFeedbackConnectionEpoch = UINT64_C(42);
    const char* rejected[] = {NULL, "0", "2", "01", "1unknown", "1 "};
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        confirmVideoPacketControlNegotiation(rejected[i]);
        CHECK(!LiGetVideoPacketControlNegotiated());
    }
    confirmVideoPacketControlNegotiation("1");
    CHECK(LiGetVideoPacketControlNegotiated());
    resetVideoPacketControlNegotiation(); // RTSP failure clears even a prior valid ACK.
    CHECK(!LiGetVideoPacketControlNegotiated());
    confirmVideoPacketControlNegotiation("1");
    CHECK(LiGetVideoPacketControlNegotiated());
    initializeVideoStream();
    CHECK(!LiSetVideoPacketControlEnabled(false));
    CHECK(!LiSetVideoPacketFeedbackEnabled(false));
    destroyVideoStream();
    CHECK(!LiGetVideoPacketControlNegotiated());
    CHECK(!LiSetVideoPacketControlEnabled(false)); // Cleanup has not released connection lifetime yet.
    LiStopConnection(); // Real public stop also clears/unfreezes before a video stream existed.
    CHECK(!LiGetVideoPacketControlNegotiated());
    CHECK(LiSetVideoPacketControlEnabled(false));
    CHECK(LiSetVideoPacketFeedbackEnabled(false));
    beginVideoPacketFeedbackConnection();
    confirmVideoPacketControlNegotiation("1"); // A stale epoch/ACK cannot authorize this opt-out reconnect.
    CHECK(!LiGetVideoPacketControlNegotiated());
    LiStopConnection();
}

static void testAckRequiresEveryGate(void) {
    scenarios++;
    const uint32_t encrypted = SS_ENC_VIDEO | SS_ENC_CONTROL_V2;
    const struct { bool control, feedback; uint32_t controlVersion, feedbackVersion, enabledEncryption; bool controlOnly; } rejected[] = {
        {false, true, 1, 2, encrypted, false},
        {true, false, 1, 2, encrypted, false},
        {true, true, 0, 2, encrypted, false},
        {true, true, 2, 2, encrypted, false},
        {true, true, 1, 1, encrypted, false},
        {true, true, 1, 3, encrypted, false},
        {true, true, 1, 2, SS_ENC_CONTROL_V2, false},
        {true, true, 1, 2, SS_ENC_VIDEO, false},
        {true, true, 1, 2, encrypted, true},
    };
    for (unsigned i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
        configure(rejected[i].control, rejected[i].feedback, rejected[i].controlVersion,
                  rejected[i].feedbackVersion, encrypted, rejected[i].controlOnly);
        EncryptionFeaturesEnabled = rejected[i].enabledEncryption;
        VideoPacketFeedbackConnectionEpoch = UINT64_C(43);
        beginVideoPacketFeedbackConnection();
        confirmVideoPacketControlNegotiation("1");
        CHECK(!LiGetVideoPacketControlNegotiated());
    }
    endVideoPacketFeedbackConnection();
}

int main(void) {
    CHECK(!isVideoPacketControlRequested());
    CHECK(!LiGetVideoPacketControlNegotiated());
    CHECK(TF_PACKET_FEEDBACK_PROFILE_VERSION == 2);
    CHECK(TF_WIRE_VERSION == 1);
    CHECK(initializePlatform() == 0);
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    AppVersionQuad[0] = 7; AppVersionQuad[1] = 1; AppVersionQuad[2] = 431; AppVersionQuad[3] = -1;
    NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    RtspPortNumber = 53010; VideoPortNumber = 52998; AudioPacketDuration = 5;
    struct sockaddr_in* remote = (struct sockaddr_in*)&RemoteAddr;
    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    remote->sin_family = AF_INET;
    remote->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(initializeControlStream() == 0);
    const uint32_t encrypted = SS_ENC_VIDEO | SS_ENC_CONTROL_V2;
    announceScenario("default-off", false, false, 1, 2, encrypted, false, false, false, 1392);
    announceScenario("measurement-only", false, true, 1, 2, encrypted, false, false, true, 1344);
    announceScenario("explicit-control", true, true, 1, 2, encrypted, false, true, true, 1344);
    announceScenario("control-without-measurement", true, false, 1, 2, encrypted, false, false, false, 1392);
    announceScenario("missing-control-capability", true, true, 0, 2, encrypted, false, false, true, 1344);
    announceScenario("future-control-capability", true, true, 2, 2, encrypted, false, false, true, 1344);
    announceScenario("legacy-feedback", true, true, 1, 1, encrypted, false, false, false, 1392);
    announceScenario("future-feedback", true, true, 1, 3, encrypted, false, false, false, 1392);
    announceScenario("missing-video-encryption", true, true, 1, 2, SS_ENC_CONTROL_V2, false, false, false, 1392);
    announceScenario("missing-control-encryption", true, true, 1, 2, SS_ENC_VIDEO, false, false, false, 1392);
    announceScenario("control-only", true, true, 1, 2, encrypted, true, false, false, 1392);
    testStrictCapabilityParser();
    testProbePaddingNegotiation();
    testAcknowledgementAndLifecycle();
    testAckRequiresEveryGate();
    CHECK(LiSetVideoPacketControlEnabled(false));
    CHECK(LiSetVideoPacketFeedbackEnabled(false));
    CHECK(LiSetVideoNetworkObservationEnabled(false));
    destroyControlStream();
    cleanupPlatform();
    printf("{\"scenarios\":%u,\"checks\":%u,\"failures\":%u,\"network_handshake_tested\":false}\n", scenarios, checks, failures);
    return failures ? 1 : 0;
}
