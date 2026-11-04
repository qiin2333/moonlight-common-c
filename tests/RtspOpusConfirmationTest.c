/* Exercise the production ANNOUNCE validator with parsed RTSP responses. */
#include "../src/RtspConnection.c"

static unsigned failures;

static void checkResponse(const char* name, const char* headers, bool expected) {
    char wire[1024];
    RTSP_MESSAGE response = {0};
    int length = snprintf(wire, sizeof(wire), "RTSP/1.0 200 OK\r\nCSeq: 7\r\n%s\r\n", headers);
    if (length < 0 || (size_t)length >= sizeof(wire)) {
        fprintf(stderr, "%s: response exceeds test buffer\n", name);
        failures++;
        return;
    }

    int result = parseRtspMessage(&response, wire, length);
    if (result != RTSP_ERROR_SUCCESS) {
        fprintf(stderr, "%s: RTSP parsing failed: %d\n", name, result);
        failures++;
        return;
    }

    if (response.message.response.statusCode != 200 ||
            isOpusConfirmationValid(response.options) != expected) {
        fprintf(stderr, "%s: expected Opus confirmation to be %s\n",
                name, expected ? "accepted" : "rejected");
        failures++;
    }
    freeMessage(&response);
}

int main(void) {
    checkResponse("legacy host", "", true);
    checkResponse("lowercase Opus", "X-SS-Audio-Codec: opus\r\n", true);
    checkResponse("Vibepollo Opus", "X-SS-Audio-Codec: Opus\r\n", true);
    checkResponse("uppercase Opus", "X-SS-Audio-Codec: OPUS\r\n", true);
    checkResponse("mixed-case Opus", "x-ss-audio-codec:\toPuS \t\r\n", true);

    checkResponse("different codec", "X-SS-Audio-Codec: AC3\r\n", false);
    checkResponse("codec prefix", "X-SS-Audio-Codec: opusX\r\n", false);
    checkResponse("empty codec", "X-SS-Audio-Codec:\r\n", false);
    checkResponse("duplicate codec",
                  "X-SS-Audio-Codec: opus\r\nX-SS-Audio-Codec: opus\r\n", false);
    checkResponse("duplicate mixed-case header",
                  "X-SS-Audio-Codec: Opus\r\nx-ss-audio-codec: opus\r\n", false);
    checkResponse("conflicting duplicate codec",
                  "X-SS-Audio-Codec: AC3\r\nx-ss-audio-codec: opus\r\n", false);

    const char* matHeaders[] = {
        "X-SS-Mat-SHA256", "X-SS-Mat-Cycles", "X-SS-Mat-Carrier",
        "X-SS-Mat-Clock", "X-SS-Mat-Live-Epoch", "X-SS-Mat-Live-Domain",
        "X-SS-Mat-Live-Crypto"
    };
    for (unsigned i = 0; i < sizeof(matHeaders) / sizeof(matHeaders[0]); i++) {
        char headers[256];
        snprintf(headers, sizeof(headers), "X-SS-Audio-Codec: Opus\r\n%s: value\r\n", matHeaders[i]);
        checkResponse(matHeaders[i], headers, false);
        // The legacy missing-codec allowance must not permit MAT metadata.
        snprintf(headers, sizeof(headers), "%s: value\r\n", matHeaders[i]);
        checkResponse(matHeaders[i], headers, false);
    }

    printf("RTSP Opus confirmation: %u failures\n", failures);
    return failures == 0 ? 0 : 1;
}
