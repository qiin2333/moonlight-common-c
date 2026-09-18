#pragma once

#include "Limelight.h"
#include "Input.h"
#include <stddef.h>

// Shared client feature bits (x-ml-general.featureFlags).
#define ML_FF_DS5_HAPTICS_PCM 0x04
#define ML_FF_CONTROLLER_HAPTICS 0x20

static inline uint32_t LiPcmHapticsFeatures(bool callback, bool perController, uint32_t hostFeatures) {
    if (!callback || (perController && !(hostFeatures & LI_FF_CONTROLLER_HAPTICS))) return 0;
    return ML_FF_DS5_HAPTICS_PCM | (perController ? ML_FF_CONTROLLER_HAPTICS : 0);
}

static inline bool LiShouldSendControllerPcm(uint32_t clientFeatures, bool ready) {
    return (clientFeatures & ML_FF_DS5_HAPTICS_PCM) != 0 &&
        (!(clientFeatures & ML_FF_CONTROLLER_HAPTICS) || ready);
}

// Byte-wise parsing avoids alignment and endian assumptions on either side.
static inline bool LiParseControllerHapticsState(const void* data, size_t size, uint8_t* player, bool* ready) {
    if (data == NULL || player == NULL || ready == NULL || size != 12) return false;
    const uint8_t* p = (const uint8_t*)data;
    if (p[0] != 0 || p[1] != 0 || p[2] != 0 || p[3] != 8 ||
        p[4] != 0x0B || p[5] != 0 || p[6] != 0 || p[7] != 0x55 ||
        p[8] >= 16 || p[9] > 1 || p[10] != 0 || p[11] != 0) return false;
    *player = p[8];
    *ready = p[9] != 0;
    return true;
}
