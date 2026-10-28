#include "Limelight-internal.h"
#include <stddef.h>
#include <stdio.h>

static unsigned failures;
#define CHECK(value)                                                                               \
    do {                                                                                           \
        if (!(value)) {                                                                            \
            failures++;                                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #value);                            \
        }                                                                                          \
    } while (0)

static void onV2(const LI_DS5_HAPTICS_IR_FRAME_V2 *frame) { (void)frame; }
static void onV3(const LI_DS5_HAPTICS_IR_FRAME_V3 *frame) { (void)frame; }
static void onPcm(const LI_DS5_HAPTICS_PCM_FRAME *frame) { (void)frame; }

static void checkCallbackSourceContract(void) {
    struct {
        uint8_t before[16];
        CONNECTION_LISTENER_CALLBACKS callbacks;
        uint8_t after[16];
    } storage;
    // Existing v2 source can keep its initializer after a synchronized rebuild.
    CONNECTION_LISTENER_CALLBACKS v2Only = {.ds5HapticsIrV2 = onV2};
    unsigned i;
    CHECK(v2Only.ds5HapticsIrV2 == onV2 && v2Only.ds5HapticsIrV3 == NULL);
    CHECK(offsetof(CONNECTION_LISTENER_CALLBACKS, ds5HapticsIrV3) ==
          offsetof(CONNECTION_LISTENER_CALLBACKS, ds5HapticsIrV2) + sizeof(ConnListenerDs5HapticsIrV2));
    memset(&storage, 0xa5, sizeof(storage));
    LiInitializeConnectionCallbacks(&storage.callbacks);
    CHECK(storage.callbacks.ds5HapticsPcm == NULL);
    CHECK(storage.callbacks.ds5HapticsIrV2 == NULL);
    CHECK(storage.callbacks.ds5HapticsIrV3 == NULL);
    for (i = 0; i < sizeof(storage.before); i++) {
        CHECK(storage.before[i] == 0xa5 && storage.after[i] == 0xa5);
    }
}

static void checkFeatures(bool v2, bool v3, bool pcm, bool sunshine) {
    int length = 0;
    char *sdp;
    const char *field;
    uint32_t flags;
    AppVersionQuad[3] = sunshine ? -1 : 0;
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    ListenerCallbacks.ds5HapticsIrV2 = v2 ? onV2 : NULL;
    ListenerCallbacks.ds5HapticsIrV3 = v3 ? onV3 : NULL;
    ListenerCallbacks.ds5HapticsPcm = pcm ? onPcm : NULL;
    sdp = getSdpPayloadForStreamConfig(14, &length);
    CHECK(sdp != NULL && length > 0);
    if (sdp == NULL)
        return;
    field = strstr(sdp, "a=x-ml-general.featureFlags:");
    if (!sunshine) {
        CHECK(field == NULL);
        free(sdp);
        return;
    }
    CHECK(field != NULL);
    if (field != NULL) {
        flags = (uint32_t)strtoul(field + strlen("a=x-ml-general.featureFlags:"), NULL, 10);
        CHECK((flags & ML_FF_DS5_HAPTICS_IR_V2) == (v2 ? ML_FF_DS5_HAPTICS_IR_V2 : 0));
        CHECK((flags & ML_FF_DS5_HAPTICS_IR_V3) == (v3 ? ML_FF_DS5_HAPTICS_IR_V3 : 0));
        CHECK((flags & ML_FF_DS5_HAPTICS_PCM) == (pcm ? ML_FF_DS5_HAPTICS_PCM : 0));
        CHECK((flags & (ML_FF_FEC_STATUS | ML_FF_SESSION_ID_V1)) ==
              (ML_FF_FEC_STATUS | ML_FF_SESSION_ID_V1));
    }
    free(sdp);
}

int main(void) {
    checkCallbackSourceContract();
    CHECK(ML_FF_DS5_HAPTICS_IR_V3 == 0x80);
    CHECK(
        (ML_FF_DS5_HAPTICS_IR_V3 & (ML_FF_FEC_STATUS | ML_FF_SESSION_ID_V1 | ML_FF_DS5_HAPTICS_PCM |
                                    ML_FF_DS5_HAPTICS_IR_V2 | ML_FF_REMOTE_TEXT_CONTEXT)) == 0);
    CHECK(initializePlatform() == 0);
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = -1;
    NegotiatedVideoFormat = VIDEO_FORMAT_H264;
    RtspPortNumber = 53010;
    VideoPortNumber = 52998;
    AudioPacketDuration = 5;
    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    ((struct sockaddr_in *)&RemoteAddr)->sin_family = AF_INET;
    ((struct sockaddr_in *)&RemoteAddr)->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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
    CHECK(initializeControlStream() == 0);
    checkFeatures(false, false, false, true);
    checkFeatures(true, false, false, true);
    checkFeatures(false, true, false, true);
    // Both IR versions must remain advertised so old hosts see the v2 bit.
    checkFeatures(true, true, false, true);
    checkFeatures(false, false, true, true);
    checkFeatures(true, true, false, false);
    LiInitializeConnectionCallbacks(&ListenerCallbacks);
    destroyControlStream();
    cleanupPlatform();
    printf("DualSense IR negotiation: %u failures\n", failures);
    return failures == 0 ? 0 : 1;
}
