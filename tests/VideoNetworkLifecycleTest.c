#include "Limelight-internal.h"
#include <rs.h>

#include <stdatomic.h>
#include <stdio.h>

static int failures;
static atomic_bool readerRunning;
static atomic_int readerFailures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures++; \
        } \
    } while (0)

static void reader(void* context) {
    (void)context;
    while (atomic_load_explicit(&readerRunning, memory_order_relaxed)) {
        LI_VIDEO_NETWORK_SNAPSHOT snapshot;
        const bool available = LiGetVideoNetworkSnapshot(&snapshot);
        if (snapshot.version != LI_VIDEO_NETWORK_SNAPSHOT_VERSION || snapshot.size != sizeof(snapshot) ||
            (available && (!snapshot.enabled || snapshot.connectionEpoch == 0))) {
            atomic_fetch_add_explicit(&readerFailures, 1, memory_order_relaxed);
        }
    }
}

static void testSamplingLifecycle(void) {
    LI_VIDEO_NETWORK_SNAPSHOT snapshot;
    CHECK(!LiGetVideoNetworkSnapshot(NULL));
    CHECK(!LiGetVideoNetworkSnapshot(&snapshot));
    initializeVideoStream();
    CHECK(!LiSetVideoNetworkObservationEnabled(true));
    CHECK(!LiGetVideoNetworkSnapshot(&snapshot));
    destroyVideoStream();

    CHECK(LiSetVideoNetworkObservationEnabled(true));
    initializeVideoStream();
    CHECK(!LiSetVideoNetworkObservationEnabled(false));
    CHECK(LiGetVideoNetworkSnapshot(&snapshot));
    const uint64_t epoch = snapshot.connectionEpoch;
    notifyVideoNetworkBlockResult(4, 4, true, false);
    notifyVideoNetworkBlockResult(4, 3, true, true);
    notifyVideoNetworkBlockResult(4, 1, false, false);
    CHECK(LiGetVideoNetworkSnapshot(&snapshot));
    CHECK(snapshot.completedBlocks == 2);
    CHECK(snapshot.completedFrames == 1);
    CHECK(snapshot.recoveredDataPackets == 1);
    CHECK(snapshot.failedObservedBlocks == 1);
    CHECK(snapshot.uniquePackets == 0); // Synthetic reconstruction is separate.
    destroyVideoStream();
    CHECK(!LiGetVideoNetworkSnapshot(&snapshot));
    initializeVideoStream();
    CHECK(LiGetVideoNetworkSnapshot(&snapshot));
    CHECK(snapshot.connectionEpoch > epoch);
    CHECK(snapshot.completedBlocks == 0);
    destroyVideoStream();
}

static int addPacket(RTP_VIDEO_QUEUE* queue, uint16_t sequence, uint32_t frame,
                     uint8_t block, uint32_t index) {
    const int size = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    char* buffer = calloc(1, size + sizeof(RTPV_QUEUE_ENTRY));
    CHECK(buffer != NULL);
    if (buffer == NULL) {
        return RTPF_RET_REJECTED;
    }
    PRTP_PACKET rtp = (PRTP_PACKET)buffer;
    PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(buffer + MAX_RTP_HEADER_SIZE);
    rtp->header = 0x90;
    rtp->sequenceNumber = sequence; // Host order, as VideoStream.c submits it.
    rtp->timestamp = 90000;
    nv->frameIndex = LE32(frame);
    nv->streamPacketIndex = LE32((uint32_t)sequence << 8);
    nv->flags = FLAG_CONTAINS_PIC_DATA | (index == 0 ? FLAG_SOF : 0) | (index == 3 ? FLAG_EOF : 0);
    nv->multiFecFlags = 0x10;
    nv->multiFecBlocks = (block << 4) | (1 << 6);
    nv->fecInfo = LE32((4U << 22) | (index << 12) | (25U << 4));
    const int ret = RtpvAddPacket(queue, rtp, size, (PRTPV_QUEUE_ENTRY)(buffer + size));
    if (ret != RTPF_RET_QUEUED) {
        free(buffer);
    }
    return ret;
}

static void testRealQueueReportsNormalAndFailedBlocksOnce(void) {
    RTP_VIDEO_QUEUE queue;
    LI_VIDEO_NETWORK_SNAPSHOT snapshot;
    initializeVideoStream();
    RtpvInitializeQueue(&queue);
    for (uint32_t i = 0; i < 4; i++) {
        CHECK(addPacket(&queue, (uint16_t)(100 + i), 1, 0, i) == RTPF_RET_QUEUED);
    }
    CHECK(LiGetVideoNetworkSnapshot(&snapshot));
    CHECK(snapshot.completedBlocks == 1); // The no-recovery path is observed.
    CHECK(snapshot.completedFrames == 0); // Block 1 is still outstanding.
    CHECK(addPacket(&queue, 100, 1, 0, 0) == RTPF_RET_REJECTED);
    CHECK(addPacket(&queue, 105, 1, 1, 0) == RTPF_RET_QUEUED);
    CHECK(addPacket(&queue, 110, 2, 0, 0) == RTPF_RET_QUEUED);
    CHECK(LiGetVideoNetworkSnapshot(&snapshot));
    CHECK(snapshot.completedBlocks == 1);
    CHECK(snapshot.failedObservedBlocks == 1);
    CHECK(snapshot.recoveredDataPackets == 0);
    CHECK(snapshot.uniquePackets == 0); // Queue callbacks cannot create arrivals.
    RtpvCleanupQueue(&queue);
    destroyVideoStream();
}

static void testReaderDuringDestructionAndReconnect(void) {
    PLT_THREAD thread;
    atomic_store(&readerRunning, true);
    atomic_store(&readerFailures, 0);
    const int err = PltCreateThread("NetworkReader", reader, NULL, &thread);
    CHECK(err == 0);
    if (err != 0) {
        return;
    }
    for (unsigned i = 0; i < 200; i++) {
        initializeVideoStream();
        notifyVideoNetworkBlockResult(4, 4, true, true);
        destroyVideoStream();
    }
    atomic_store(&readerRunning, false);
    PltJoinThread(&thread);
    CHECK(atomic_load(&readerFailures) == 0);
}

static void testRealRsRecoveryPreservesPayload(void) {
    // D=200, requested 1%, minimum 5: an encodable header uses F=3%, P=6.
    // Generate real RS parity, lose five source shards, and pass originals
    // through the actual queue. Keep a second block outstanding so the test
    // can inspect recovered payloads before the video depacketizer consumes them.
    enum {data = 200, parity = 6, total = data + parity};
    uint8_t* shards[total] = {0};
    const int size = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    const int payloadOffset = MAX_RTP_HEADER_SIZE + sizeof(NV_VIDEO_PACKET);
    RTP_VIDEO_QUEUE queue;
    LI_VIDEO_NETWORK_SNAPSHOT snapshot;
    initializeVideoStream();
    RtpvInitializeQueue(&queue);
    for (unsigned i = 0; i < total; i++) {
        shards[i] = calloc(1, size);
        CHECK(shards[i] != NULL);
        if (shards[i] == NULL) {
            goto cleanup;
        }
        if (i < data) {
            PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(shards[i] + MAX_RTP_HEADER_SIZE);
            nv->streamPacketIndex = LE32((100U + i) << 8);
            nv->frameIndex = LE32(1);
            nv->flags = FLAG_CONTAINS_PIC_DATA | (i == 0 ? FLAG_SOF : 0) | (i == data - 1 ? FLAG_EOF : 0);
            nv->multiFecFlags = 0x10;
            nv->multiFecBlocks = 1 << 6;
            for (int j = payloadOffset; j < size; j++) {
                shards[i][j] = (uint8_t)(i * 7 + (unsigned)j);
            }
        }
    }
    reed_solomon* rs = reed_solomon_new(data, parity);
    CHECK(rs != NULL);
    if (rs == NULL) {
        goto cleanup;
    }
    CHECK(reed_solomon_encode(rs, shards, total, size) == 0);
    reed_solomon_release(rs);
    for (unsigned i = 0; i < total; i++) {
        if (i >= 10 && i < 15) {
            continue;
        }
        char* buffer = calloc(1, size + sizeof(RTPV_QUEUE_ENTRY));
        CHECK(buffer != NULL);
        if (buffer == NULL) {
            goto cleanup;
        }
        memcpy(buffer, shards[i], size);
        PRTP_PACKET rtp = (PRTP_PACKET)buffer;
        PNV_VIDEO_PACKET nv = (PNV_VIDEO_PACKET)(buffer + MAX_RTP_HEADER_SIZE);
        rtp->header = 0x90;
        rtp->sequenceNumber = (uint16_t)(100 + i);
        rtp->timestamp = 90000;
        nv->frameIndex = LE32(1);
        nv->multiFecBlocks = 1 << 6;
        nv->fecInfo = LE32((data << 22) | (i << 12) | (3U << 4));
        const int ret = RtpvAddPacket(&queue, rtp, size, (PRTPV_QUEUE_ENTRY)(buffer + size));
        if (ret != RTPF_RET_QUEUED) {
            free(buffer); // The final redundant tail is deliberately rejected.
        }
    }
    CHECK(LiGetVideoNetworkSnapshot(&snapshot));
    CHECK(snapshot.completedBlocks == 1);
    CHECK(snapshot.recoveredDataPackets == 5);
    CHECK(snapshot.failedObservedBlocks == 0);
    CHECK(snapshot.uniquePackets == 0);
    CHECK(queue.completedFecBlockList.count == data);
    unsigned checked = 0;
    for (PRTPV_QUEUE_ENTRY entry = queue.completedFecBlockList.head; entry != NULL; entry = entry->next) {
        const unsigned index = (uint16_t)(entry->packet->sequenceNumber - 100);
        CHECK(index < data);
        if (index < data) {
            const PNV_VIDEO_PACKET recovered = (PNV_VIDEO_PACKET)((uint8_t*)entry->packet + MAX_RTP_HEADER_SIZE);
            CHECK(recovered->streamPacketIndex == LE32((100U + index) << 8));
            CHECK(memcmp((uint8_t*)entry->packet + payloadOffset, shards[index] + payloadOffset,
                         size - payloadOffset) == 0);
            checked++;
        }
    }
    CHECK(checked == data);
cleanup:
    for (unsigned i = 0; i < total; i++) {
        free(shards[i]);
    }
    RtpvCleanupQueue(&queue);
    destroyVideoStream();
}

int main(void) {
    CHECK(initializePlatform() == 0);
    LiInitializeStreamConfiguration(&StreamConfig);
    StreamConfig.packetSize = 1024;
    AppVersionQuad[0] = 7;
    AppVersionQuad[1] = 1;
    AppVersionQuad[2] = 431;
    AppVersionQuad[3] = 0;
    CHECK(initializeControlStream() == 0);
    testSamplingLifecycle();
    testRealQueueReportsNormalAndFailedBlocksOnce();
    testRealRsRecoveryPreservesPayload();
    testReaderDuringDestructionAndReconnect();
    CHECK(LiSetVideoNetworkObservationEnabled(false));
    destroyControlStream();
    cleanupPlatform();
    printf("Video network lifecycle: 4 scenarios, %d failures\n", failures);
    return failures != 0;
}
