#include "ControllerHaptics.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void) {
    // Old client keeps global PCM semantics on both host versions.
    CHECK(LiPcmHapticsFeatures(true, false, 0) == 0x04);
    CHECK(LiPcmHapticsFeatures(true, false, 0x400) == 0x04);
    // New client must leave the old host's synthesizer enabled.
    CHECK(LiPcmHapticsFeatures(true, true, 0x80) == 0);
    CHECK(LiPcmHapticsFeatures(true, true, 0x400) == 0x24);
    CHECK(LiPcmHapticsFeatures(false, true, 0x400) == 0);
    CHECK(LiShouldSendControllerPcm(0x04, false));
    CHECK(!LiShouldSendControllerPcm(0x24, false));
    CHECK(LiShouldSendControllerPcm(0x24, true));
    CHECK(!LiShouldSendControllerPcm(0x20, true));
    CHECK(!LiShouldSendControllerPcm(0x08, true)); // IR stays independent

    const uint8_t golden[] = {0,0,0,8, 0x0B,0,0,0x55, 15,1,0,0};
    uint8_t packet[sizeof(golden)], player = 0;
    bool ready = false;
    CHECK(sizeof(SS_CONTROLLER_HAPTICS_PACKET) == sizeof(golden));
    CHECK(LiParseControllerHapticsState(golden, sizeof(golden), &player, &ready));
    CHECK(player == 15 && ready);
    for (size_t size = 0; size < sizeof(golden); size++) {
        CHECK(!LiParseControllerHapticsState(golden, size, &player, &ready));
    }
    CHECK(!LiParseControllerHapticsState(golden, 13, &player, &ready));
    // Reject invalid player, mode, reserved fields, size and message type.
    for (size_t i = 0; i < sizeof(golden); i++) {
        memcpy(packet, golden, sizeof(packet));
        packet[i] = 0xFF;
        CHECK(!LiParseControllerHapticsState(packet, sizeof(packet), &player, &ready));
    }
    memcpy(packet, golden, sizeof(packet));
    packet[8] = 0; packet[9] = 0;
    CHECK(LiParseControllerHapticsState(packet, sizeof(packet), &player, &ready));
    CHECK(player == 0 && !ready);
    puts("controller haptics protocol tests passed");
    return 0;
}
