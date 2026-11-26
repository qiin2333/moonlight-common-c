// Negotiation and delivery of the DualSense control extensions (0x5515 and 0x5516)
// against the real SDP generator, the real send gate and the real packet tables.
// The control stream is included so the test can reach its tables and the 0x5515
// handler. An old peer must see no change.
#include "../src/ControlStream.c"
#include <stdio.h>

static unsigned failures;
#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            failures++;                                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value);                            \
        }                                                                                          \
    } while (0)

static unsigned outputCalls;
static LI_DS5_OUTPUT_EXTENSION lastOutput;
static unsigned pcmCalls;
static unsigned lastPcmChannels;

static void onOutput(const LI_DS5_OUTPUT_EXTENSION* output) {
    outputCalls++;
    lastOutput = *output;
}

static void onPcm(const LI_DS5_HAPTICS_PCM_FRAME* frame) {
    pcmCalls++;
    lastPcmChannels = frame->channelCount;
}

static void setApp(bool sunshine, int major) {
    AppVersionQuad[0] = major;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = sunshine ? -1 : 0;
}

static char* sdp(void) {
    int length = 0;
    char* payload = getSdpPayloadForStreamConfig(14, &length);
    char* copy;

    if (payload == NULL) {
        return NULL;
    }
    copy = malloc((size_t)length + 1);
    memcpy(copy, payload, (size_t)length);
    copy[length] = '\0';
    free(payload);
    return copy;
}

static void testClientOffer(void) {
    char* without;
    char* with;
    const char* line;

    setApp(true, 7);
    LiSetDs5OutputExtensionCallback(NULL);
    without = sdp();
    CHECK(without != NULL);
    // No callback registered: the SDP carries no trace of the extension.
    CHECK(without != NULL && strstr(without, "ds5Extensions") == NULL);

    LiSetDs5OutputExtensionCallback(onOutput);
    with = sdp();
    CHECK(with != NULL);
    line = with != NULL ? strstr(with, "a=x-ml-general.ds5Extensions:output=1,input-status=1") : NULL;
    CHECK(line != NULL);

    // The only difference between the two payloads is that one attribute line.
    if (without != NULL && line != NULL) {
        size_t before = (size_t)(line - with);
        size_t lineLength = strcspn(line, "\n") + 1;
        CHECK(strlen(with) == strlen(without) + lineLength);
        CHECK(strncmp(with, without, before) == 0);
        CHECK(strcmp(with + before + lineLength, without + before) == 0);
    }
    free(without);
    free(with);

    // A non-Sunshine host never gets the attribute, even with the callback set.
    setApp(false, 7);
    with = sdp();
    CHECK(with == NULL || strstr(with, "ds5Extensions") == NULL);
    free(with);
    LiSetDs5OutputExtensionCallback(NULL);
}

static void testPacketTables(void) {
    // Every per-generation table has an entry for each packet index.
    CHECK(sizeof(packetTypesGen3) == sizeof(payloadLengthsGen3));
    CHECK(sizeof(packetTypesGen4) == sizeof(payloadLengthsGen4));
    CHECK(sizeof(packetTypesGen5) == sizeof(payloadLengthsGen5));
    CHECK(sizeof(packetTypesGen7) == sizeof(payloadLengthsGen7));
    CHECK(sizeof(packetTypesGen7Enc) == sizeof(payloadLengthsGen7Enc));
    CHECK(sizeof(packetTypesGen3) / sizeof(short) == IDX_DS5_INPUT_STATUS + 1);
    CHECK(sizeof(packetTypesGen7Enc) / sizeof(short) == IDX_DS5_INPUT_STATUS + 1);
    CHECK(sizeof(preconstructedPayloadsGen7Enc) / sizeof(char*) == IDX_DS5_INPUT_STATUS + 1);
    // Only the encrypted Sunshine control stream carries the two messages.
    CHECK(packetTypesGen7Enc[IDX_DS5_OUTPUT_EXTENSION] == DS5EXT_PTYPE_OUTPUT);
    CHECK(packetTypesGen7Enc[IDX_DS5_INPUT_STATUS] == DS5EXT_PTYPE_INPUT_STATUS);
    CHECK(packetTypesGen7[IDX_DS5_OUTPUT_EXTENSION] == -1 && packetTypesGen7[IDX_DS5_INPUT_STATUS] == -1);
    CHECK(packetTypesGen5[IDX_DS5_OUTPUT_EXTENSION] == -1 && packetTypesGen5[IDX_DS5_INPUT_STATUS] == -1);
    CHECK(packetTypesGen4[IDX_DS5_OUTPUT_EXTENSION] == -1 && packetTypesGen4[IDX_DS5_INPUT_STATUS] == -1);
    CHECK(packetTypesGen3[IDX_DS5_OUTPUT_EXTENSION] == -1 && packetTypesGen3[IDX_DS5_INPUT_STATUS] == -1);
}

static void testSendGate(void) {
    setApp(true, 7);
    CHECK(initializeControlStream() == 0);
    CHECK(packetTypes == packetTypesGen7Enc);

    // Old host: it declared nothing. Nothing is sent.
    Ds5ExtensionMask = 0;
    CHECK(LiSendDs5InputStatus(0, LI_DS5_IN_HEADSET, 1, 0, 0) == LI_ERR_UNSUPPORTED);
    CHECK(LiGetHostDs5Extensions() == 0);

    // A host that declared only "output" does not get 0x5516.
    Ds5ExtensionMask = DS5EXT_MASK_OUTPUT_V1;
    CHECK(LiSendDs5InputStatus(0, LI_DS5_IN_HEADSET, 1, 0, 0) == LI_ERR_UNSUPPORTED);
    CHECK(LiGetHostDs5Extensions() == LI_DS5_EXT_OUTPUT);

    // Declared, but the control stream is not connected: its own error, still nothing sent.
    Ds5ExtensionMask = DS5EXT_MASK_INPUT_STATUS_V1;
    CHECK(LiSendDs5InputStatus(0, LI_DS5_IN_HEADSET, 1, 0, 0) == -3);
    CHECK(LiGetHostDs5Extensions() == LI_DS5_EXT_INPUT_STATUS);
    // A malformed argument is refused before anything else is looked at.
    CHECK(LiSendDs5InputStatus(0, 0x08, 0, 0, 0) == -1);
    CHECK(LiSendDs5InputStatus(0, LI_DS5_IN_HEADSET, 2, 0, 0) == -1);

    // A non-Sunshine host is never a target.
    setApp(false, 7);
    CHECK(LiSendDs5InputStatus(0, LI_DS5_IN_HEADSET, 1, 0, 0) == LI_ERR_UNSUPPORTED);
    // Neither is a host on a control stream that has no such packet type.
    setApp(true, 5);
    stopping = true;
    destroyControlStream();
    CHECK(initializeControlStream() == 0);
    CHECK(packetTypes == packetTypesGen5);
    CHECK(LiSendDs5InputStatus(0, LI_DS5_IN_HEADSET, 1, 0, 0) == LI_ERR_UNSUPPORTED);
    Ds5ExtensionMask = 0;
}

static void testOutputDelivery(void) {
    static const uint8_t good[16] = {
        0x01, 0x00, 0x02, 0x03, 0xa5, 0x00, 0x00, 0x00,
        0x1b, 0x00, 0x7f, 0x00, 0x00, 0x01, 0x00, 0x02,
    };
    uint8_t bad[16];

    // Without a callback the message is dropped.
    LiSetDs5OutputExtensionCallback(NULL);
    outputCalls = 0;
    processDs5OutputExtension(good, sizeof(good));
    CHECK(outputCalls == 0);

    LiSetDs5OutputExtensionCallback(onOutput);
    processDs5OutputExtension(good, sizeof(good));
    CHECK(outputCalls == 1);
    CHECK(lastOutput.controllerNumber == 0x0302);
    CHECK(lastOutput.validMask == (LI_DS5_OUT_PLAYER_LEDS | LI_DS5_OUT_SPEAKER_VOLUME |
                                   LI_DS5_OUT_MIC_MUTE | LI_DS5_OUT_LIGHTBAR_BRIGHTNESS));
    CHECK(lastOutput.playerLedMask == 0x1b);
    CHECK(lastOutput.speakerVolume == 0x7f);
    CHECK(lastOutput.micMute == 1);
    CHECK(lastOutput.lightbarBrightness == 2);
    CHECK(lastOutput.muteLed == 0 && lastOutput.headphoneVolume == 0 && lastOutput.micVolume == 0 &&
          lastOutput.audioRouting == 0);

    // A malformed body reaches the callback in no part.
    outputCalls = 0;
    processDs5OutputExtension(good, 15);
    processDs5OutputExtension(good, 0);
    processDs5OutputExtension(good, -1);
    memcpy(bad, good, sizeof(bad)); bad[0] = 2;
    processDs5OutputExtension(bad, sizeof(bad));
    memcpy(bad, good, sizeof(bad)); bad[7] = 0x80;
    processDs5OutputExtension(bad, sizeof(bad));
    memcpy(bad, good, sizeof(bad)); bad[9] = 1;
    processDs5OutputExtension(bad, sizeof(bad));
    CHECK(outputCalls == 0);
    LiSetDs5OutputExtensionCallback(NULL);
}

static unsigned long featureFlagsOf(const char* payload) {
    const char* flags = payload != NULL ? strstr(payload, "x-ml-general.featureFlags:") : NULL;

    return flags != NULL ? strtoul(flags + strlen("x-ml-general.featureFlags:"), NULL, 10) : 0;
}

static void checkOffer(const char* expected) {
    // expected == NULL: the SDP must carry no ds5Extensions attribute at all.
    char* payload = sdp();
    const char* line = payload != NULL ? strstr(payload, "a=x-ml-general.ds5Extensions:") : NULL;

    CHECK(payload != NULL);
    if (expected == NULL) {
        CHECK(line == NULL);
    }
    else if (line != NULL) {
        size_t length = strcspn(line, "\r\n");
        // The generator ends every attribute with a space before the line break.
        while (length > 0 && line[length - 1] == ' ') {
            length--;
        }
        char want[96];
        snprintf(want, sizeof(want), "a=x-ml-general.ds5Extensions:%s", expected);
        CHECK(length == strlen(want) && strncmp(line, want, length) == 0);
    }
    else {
        CHECK(line != NULL);
    }
    free(payload);
}

static void testPadAudioOffer(void) {
    char* payload;

    setApp(true, 7);
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    LiSetDs5OutputExtensionCallback(onOutput);

    // Not asked for: the offer is the plain one, with or without a PCM callback.
    LiSetDs5PadAudioOffer(LI_DS5_PAD_AUDIO_OFF);
    checkOffer("output=1,input-status=1");
    ListenerCallbacks.ds5HapticsPcm = onPcm;
    checkOffer("output=1,input-status=1");

    // Asked for, and the client can take PCM frames: the token is added, the PCM feature flag is set.
    LiSetDs5PadAudioOffer(LI_DS5_PAD_AUDIO_PCM);
    checkOffer("output=1,input-status=1,pad-audio=1");
    payload = sdp();
    CHECK((featureFlagsOf(payload) & ML_FF_DS5_HAPTICS_PCM) != 0);
    free(payload);

    // Asked for, but no PCM callback: nothing is offered for the speaker.
    ListenerCallbacks.ds5HapticsPcm = NULL;
    checkOffer("output=1,input-status=1");

    // The token does not depend on the output callback.
    ListenerCallbacks.ds5HapticsPcm = onPcm;
    LiSetDs5OutputExtensionCallback(NULL);
    checkOffer("pad-audio=1");
    ListenerCallbacks.ds5HapticsPcm = NULL;
    checkOffer(NULL);

    // A mode this library does not define is the same as off.
    ListenerCallbacks.ds5HapticsPcm = onPcm;
    LiSetDs5OutputExtensionCallback(onOutput);
    LiSetDs5PadAudioOffer(2);
    checkOffer("output=1,input-status=1");
    LiSetDs5PadAudioOffer(-1);
    checkOffer("output=1,input-status=1");

    // A non-Sunshine host never sees the attribute.
    LiSetDs5PadAudioOffer(LI_DS5_PAD_AUDIO_PCM);
    setApp(false, 7);
    payload = sdp();
    CHECK(payload == NULL || strstr(payload, "ds5Extensions") == NULL);
    free(payload);

    LiSetDs5PadAudioOffer(LI_DS5_PAD_AUDIO_OFF);
    LiSetDs5OutputExtensionCallback(NULL);
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
}

static void testHostAnswer(void) {
    Ds5ExtensionMask = ds5ExtParseSdpPayload("a=x-ss-general.ds5Extensions:output=1,input-status=1,pad-audio=1\r\n");
    CHECK(LiGetHostDs5Extensions() == (LI_DS5_EXT_OUTPUT | LI_DS5_EXT_INPUT_STATUS | LI_DS5_EXT_PAD_AUDIO));
    Ds5ExtensionMask = ds5ExtParseSdpPayload("a=x-ss-general.ds5Extensions:output=1,input-status=1\r\n");
    CHECK(LiGetHostDs5Extensions() == (LI_DS5_EXT_OUTPUT | LI_DS5_EXT_INPUT_STATUS));
    Ds5ExtensionMask = 0;
}

static void testPcmDelivery(void) {
    // Two haptic channels, then four channels (speaker pair first).
    static const uint8_t stereo[] = {
        0x01, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xbb, 0x00, 0x00,
        0x02, 0x10, 0x00, 0x00, 0x11, 0x12, 0x13, 0x14,
    };
    static const uint8_t quad[] = {
        0x01, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xbb, 0x00, 0x00,
        0x04, 0x10, 0x00, 0x00, 0x11, 0x12, 0x13, 0x14, 0x21, 0x22, 0x23, 0x24,
    };

    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    ListenerCallbacks.ds5HapticsPcm = onPcm;

    // Not offered: two channels are delivered, four are not.
    LiSetDs5PadAudioOffer(LI_DS5_PAD_AUDIO_OFF);
    pcmCalls = 0;
    CHECK(processDs5HapticsStreamPacket(stereo, sizeof(stereo), dispatchDs5HapticsPcm, NULL));
    CHECK(pcmCalls == 1 && lastPcmChannels == 2);
    CHECK(processDs5HapticsStreamPacket(quad, sizeof(quad), dispatchDs5HapticsPcm, NULL));
    CHECK(pcmCalls == 1);

    // Offered: both are delivered, and the callback sees the channel count.
    LiSetDs5PadAudioOffer(LI_DS5_PAD_AUDIO_PCM);
    CHECK(processDs5HapticsStreamPacket(quad, sizeof(quad), dispatchDs5HapticsPcm, NULL));
    CHECK(pcmCalls == 2 && lastPcmChannels == 4);
    CHECK(processDs5HapticsStreamPacket(stereo, sizeof(stereo), dispatchDs5HapticsPcm, NULL));
    CHECK(pcmCalls == 3 && lastPcmChannels == 2);

    LiSetDs5PadAudioOffer(LI_DS5_PAD_AUDIO_OFF);
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
}

int main(void) {
    CHECK(initializePlatform() == 0);
    NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    RtspPortNumber = 53010;
    VideoPortNumber = 52998;
    AudioPacketDuration = 5;
    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    ((struct sockaddr_in*)&RemoteAddr)->sin_family = AF_INET;
    ((struct sockaddr_in*)&RemoteAddr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    LiInitializeStreamConfiguration(&StreamConfig);
    StreamConfig.width = 1280;
    StreamConfig.height = 720;
    StreamConfig.fps = 30;
    StreamConfig.bitrate = 10000;
    StreamConfig.packetSize = 1392;
    StreamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
    StreamConfig.streamingRemotely = STREAM_CFG_LOCAL;
    EncryptionFeaturesSupported = SS_ENC_CONTROL_V2;
    EncryptionFeaturesRequested = 0;
    EncryptionFeaturesEnabled = 0;

    testClientOffer();
    testPacketTables();
    testSendGate();
    testOutputDelivery();
    testPadAudioOffer();
    testHostAnswer();
    testPcmDelivery();

    stopping = true;
    destroyControlStream();
    cleanupPlatform();
    printf("DualSense extension negotiation: %u failures\n", failures);
    return failures == 0 ? 0 : 1;
}
