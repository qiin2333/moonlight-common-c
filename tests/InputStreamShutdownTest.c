// Exercise the production sender with an ACK that never arrives during shutdown.
#define PltSleepMs testSleepMs
#define isControlDataInTransit testControlDataInTransit
#define flushInputOnControlStream testFlushInputOnControlStream
#define sendInputPacketOnControlStream testSendInputPacketOnControlStream
#include "../src/InputStream.c"
#undef PltSleepMs
#undef isControlDataInTransit
#undef flushInputOnControlStream
#undef sendInputPacketOnControlStream

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(EXIT_FAILURE); \
        } \
    } while (0)

static int ackChecks;
static int keyReleases;

void testSleepMs(int ms) {
    CHECK(ms == 10);
    ConnectionInterrupted = true;
}

bool testControlDataInTransit(void) {
    // A broken sender must fail promptly instead of hanging the test runner.
    CHECK(++ackChecks == 1);
    return true;
}

void testFlushInputOnControlStream(void) {}

int testSendInputPacketOnControlStream(unsigned char* data, int length,
                                      uint8_t channelId, uint32_t flags, bool moreData) {
    NV_INPUT_HEADER* header = (NV_INPUT_HEADER*)data;
    (void)length;
    (void)channelId;
    (void)flags;
    (void)moreData;
    CHECK(header->magic == LE32(KEY_UP_EVENT_MAGIC));
    keyReleases++;
    return 0;
}

int main(void) {
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 500;
    AppVersionQuad[3] = 0; // Exercise GFE's ACK wait (Sunshine may skip it).
    CHECK(initializeInputStream() == 0);
    initialized = true;

    CHECK(LiSendUtf8TextEvent("ab", 2) == 0);
    CHECK(LiSendKeyboardEvent(0x41, KEY_ACTION_UP, 0) == 0);
    LbqSignalQueueDrain(&packetQueue);
    inputSendThreadProc(NULL);

    CHECK(ackChecks == 1);
    CHECK(keyReleases == 1);
    CHECK(LbqGetItemCount(&packetQueue) == 0);

    initialized = false;
    LbqSignalQueueShutdown(&packetQueue);
    LbqSignalQueueShutdown(&packetHolderFreeList);
    destroyInputStream();
    puts("Input stream shutdown regression passed");
    return 0;
}
