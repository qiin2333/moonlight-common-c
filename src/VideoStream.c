#include "Limelight-internal.h"
#include "VideoNetwork.h"
#include "VideoPacketFeedback.h"

#include <stdatomic.h>

#define FIRST_FRAME_MAX 1500
#define FIRST_FRAME_TIMEOUT_SEC 10

#define FIRST_FRAME_PORT 47996

static RTP_VIDEO_QUEUE rtpQueue;

static SOCKET rtpSocket = INVALID_SOCKET;
static SOCKET firstFrameSocket = INVALID_SOCKET;

static PPLT_CRYPTO_CONTEXT decryptionCtx;

static PLT_THREAD udpPingThread;
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

// PyroWave is intentionally queued even when the renderer advertises
// CAPABILITY_DIRECT_SUBMIT for legacy codecs. Its decode/present callback is
// synchronous, so it must be consumed by the decoder thread rather than the
// receive thread.
static bool videoNeedsDecoderThread(void) {
    return (NegotiatedVideoFormat == VIDEO_FORMAT_PYROWAVE &&
                (VideoCallbacks.capabilities & CAPABILITY_PULL_RENDERER) == 0) ||
           (VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0;
}

static bool receivedDataFromPeer;
static uint64_t firstDataTimeMs;
static bool receivedFullFrame;
static _Atomic uint64_t rtpVideoBytesReceived;
static VIDEO_NETWORK_OBSERVER networkObserver;
static atomic_flag networkObserverLock = ATOMIC_FLAG_INIT;
static bool networkObservationEnabled;
static bool videoStreamInitialized;
static uint64_t networkConnectionEpoch;
static bool packetFeedbackRequested;
static bool packetFeedbackConnectionActive;
static bool packetControlRequested;
static bool packetControlNegotiated;
static VIDEO_PACKET_FEEDBACK packetFeedback;
uint32_t VideoPacketFeedbackSupportedVersion;
uint64_t VideoPacketFeedbackConnectionEpoch;
uint32_t VideoPacketControlSupportedVersion;
uint32_t TransportPolicyStatusSupportedVersion;
static bool transportPolicyStatusNegotiated;
static TPS_STATUS_RECEIVER transportPolicyStatus;

// Lifetime-independent lock: the snapshot API may race stream destruction,
// but it never touches a destroyed platform mutex or freed observer storage.
static void lockNetworkObserver(void) {
    unsigned spins = 0;
    while (atomic_flag_test_and_set_explicit(&networkObserverLock, memory_order_acquire)) {
        // Let the owner run when report preparation contends with receive/API reads.
        if (++spins >= 64) {
            PltSleepMs(1);
            spins = 0;
        }
    }
}

static void unlockNetworkObserver(void) {
    atomic_flag_clear_explicit(&networkObserverLock, memory_order_release);
}

bool LiSetVideoNetworkObservationEnabled(bool enabled) {
    bool accepted;
    lockNetworkObserver();
    accepted = !videoStreamInitialized && !packetFeedbackConnectionActive && (enabled || !packetFeedbackRequested);
    if (accepted) {
        networkObservationEnabled = enabled;
    }
    unlockNetworkObserver();
    return accepted;
}

bool LiSetVideoPacketFeedbackEnabled(bool enabled) {
    lockNetworkObserver();
    const bool accepted = !videoStreamInitialized && !packetFeedbackConnectionActive;
    if (accepted) { packetFeedbackRequested = enabled; if (enabled) networkObservationEnabled = true; }
    unlockNetworkObserver();
    return accepted;
}

bool LiSetVideoPacketControlEnabled(bool enabled) {
    lockNetworkObserver();
    const bool accepted = !videoStreamInitialized && !packetFeedbackConnectionActive;
    if (accepted) {
        packetControlRequested = enabled;
        packetControlNegotiated = false;
    }
    unlockNetworkObserver();
    return accepted;
}

bool LiGetVideoPacketControlNegotiated(void) {
    lockNetworkObserver();
    const bool negotiated = packetFeedbackConnectionActive && packetControlNegotiated;
    unlockNetworkObserver();
    return negotiated;
}

void beginVideoPacketFeedbackConnection(void) {
    lockNetworkObserver();
    packetFeedbackConnectionActive = true;
    packetControlNegotiated = false;
    transportPolicyStatusNegotiated = false;
    TpsInitializeReceiver(&transportPolicyStatus, 0);
    unlockNetworkObserver();
}

void endVideoPacketFeedbackConnection(void) {
    lockNetworkObserver();
    packetControlNegotiated = false;
    packetFeedbackConnectionActive = false;
    transportPolicyStatusNegotiated = false;
    TpsInitializeReceiver(&transportPolicyStatus, 0);
    unlockNetworkObserver();
}

void resetVideoPacketControlNegotiation(void) {
    lockNetworkObserver();
    packetControlNegotiated = false;
    transportPolicyStatusNegotiated = false;
    TpsInitializeReceiver(&transportPolicyStatus, 0);
    unlockNetworkObserver();
}

bool isVideoPacketControlRequested(void) {
    lockNetworkObserver();
    const bool requested = packetControlRequested;
    unlockNetworkObserver();
    return requested;
}

static bool packetControlAnnounceEligibleLocked(void) {
    return packetControlRequested && packetFeedbackRequested &&
        VideoPacketControlSupportedVersion == 1 &&
        VideoPacketFeedbackSupportedVersion == TF_PACKET_FEEDBACK_PROFILE_VERSION &&
        !StreamConfig.controlOnly && (EncryptionFeaturesEnabled & SS_ENC_VIDEO) &&
        (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2);
}

bool shouldAnnounceVideoPacketControl(void) {
    lockNetworkObserver();
    const bool eligible = packetControlAnnounceEligibleLocked();
    unlockNetworkObserver();
    return eligible;
}

void confirmVideoPacketControlNegotiation(const char* controlVersion) {
    lockNetworkObserver();
    packetControlNegotiated = packetFeedbackConnectionActive && packetControlAnnounceEligibleLocked() &&
        VideoPacketFeedbackConnectionEpoch != 0 && controlVersion != NULL && strcmp(controlVersion, "1") == 0;
    unlockNetworkObserver();
}

bool isVideoPacketFeedbackRequested(void) {
    lockNetworkObserver();
    const bool requested = packetFeedbackRequested;
    unlockNetworkObserver();
    return requested;
}

static bool policyStatusAnnounceEligibleLocked(void) {
    return packetFeedbackRequested && TransportPolicyStatusSupportedVersion == TPS_STATUS_VERSION &&
        VideoPacketFeedbackSupportedVersion == TF_PACKET_FEEDBACK_PROFILE_VERSION && !StreamConfig.controlOnly &&
        (EncryptionFeaturesEnabled & SS_ENC_VIDEO) && (EncryptionFeaturesEnabled & SS_ENC_CONTROL_V2);
}
bool shouldAnnounceTransportPolicyStatus(void) {
    lockNetworkObserver();
    const bool eligible = policyStatusAnnounceEligibleLocked();
    unlockNetworkObserver();
    return eligible;
}
void confirmTransportPolicyStatusNegotiation(const char* version) {
    lockNetworkObserver();
    transportPolicyStatusNegotiated = packetFeedbackConnectionActive && policyStatusAnnounceEligibleLocked() &&
        VideoPacketFeedbackConnectionEpoch && version != NULL && strcmp(version, "1") == 0;
    unlockNetworkObserver();
}
void notifyTransportPolicyStatus(const uint8_t* payload, size_t length) {
    TPS_STATUS_NOTICE notice;
    if (!TpsDecodeStatus(payload, length, &notice)) return;
    lockNetworkObserver();
    if (videoStreamInitialized && transportPolicyStatusNegotiated)
        TpsAcceptStatus(&transportPolicyStatus, &notice);
    unlockNetworkObserver();
}
bool LiGetTransportPolicyStatusNotice(TPS_STATUS_NOTICE* notice) {
    lockNetworkObserver();
    const bool available = packetFeedbackConnectionActive && videoStreamInitialized && transportPolicyStatusNegotiated &&
        TpsCopyStatus(&transportPolicyStatus, notice);
    unlockNetworkObserver();
    return available;
}

void notifyVideoPacketFeedbackReady(const uint8_t* payload, size_t length) {
    TF_READY ready;
    if (!TfDecodeReady(payload, length, &ready)) return;
    lockNetworkObserver();
    if (videoStreamInitialized && packetFeedbackRequested && VideoPacketFeedbackConnectionEpoch)
        VfAcceptReady(&packetFeedback, &networkObserver, &ready, PltGetMicroseconds());
    unlockNetworkObserver();
}

bool prepareVideoPacketFeedbackReport(struct _TF_PACKET_REPORT* report) {
    lockNetworkObserver();
    const bool prepared = videoStreamInitialized && packetFeedbackRequested && VideoPacketFeedbackConnectionEpoch &&
        VfPrepareReport(&packetFeedback, &networkObserver, PltGetMicroseconds(), report);
    unlockNetworkObserver();
    return prepared;
}

void commitVideoPacketFeedbackReport(const struct _TF_PACKET_REPORT* report) {
    lockNetworkObserver();
    if (videoStreamInitialized && packetFeedbackRequested && VideoPacketFeedbackConnectionEpoch)
        VfCommitQueued(&packetFeedback, &networkObserver, report);
    unlockNetworkObserver();
}

bool LiGetVideoNetworkSnapshot(LI_VIDEO_NETWORK_SNAPSHOT* snapshot) {
    bool available;
    if (snapshot == NULL) {
        return false;
    }
    const uint64_t nowUs = PltGetMicroseconds();
    lockNetworkObserver();
    available = videoStreamInitialized && networkObservationEnabled;
    if (available) {
        VnGetSnapshot(&networkObserver, nowUs, snapshot);
    }
    else {
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->version = LI_VIDEO_NETWORK_SNAPSHOT_VERSION;
        snapshot->size = sizeof(*snapshot);
    }
    unlockNetworkObserver();
    return available;
}

void notifyVideoNetworkBlockResult(uint32_t dataPackets, uint32_t receivedDataPackets,
                                   bool complete, bool lastBlock) {
    if (!networkObservationEnabled || dataPackets == 0 || receivedDataPackets > dataPackets) {
        return;
    }
    lockNetworkObserver();
    if (complete) {
        networkObserver.snapshot.completedBlocks++;
        networkObserver.snapshot.recoveredDataPackets += dataPackets - receivedDataPackets;
        if (lastBlock) {
            networkObserver.snapshot.completedFrames++;
        }
    }
    else {
        // Only known blocks are counted here. Whole unseen frames require the
        // sender ledger; they cannot be inferred from a decoder FPS value.
        networkObserver.snapshot.failedObservedBlocks++;
    }
    unlockNetworkObserver();
}

// We can't request an IDR frame until the depacketizer knows
// that a packet was lost. This timeout bounds the time that
// the RTP queue will wait for missing/reordered packets.
#define RTP_QUEUE_DELAY 10

// Desired number of video packets for the socket receive buffer.
// Local connections use 4096 packets (~4.5 MB) to absorb WiFi jitter.
// Remote connections use 8192 packets (~9 MB) for higher RTT and jitter.
#define RTP_RECV_PACKETS_LOCAL  4096
#define RTP_RECV_PACKETS_REMOTE 8192

// Initialize the video stream
void initializeVideoStream(void) {
    lockNetworkObserver();
    videoStreamInitialized = true;
    if (networkObservationEnabled) {
        const uint64_t epoch = VideoPacketFeedbackConnectionEpoch ? VideoPacketFeedbackConnectionEpoch : ++networkConnectionEpoch;
        VnInitialize(&networkObserver, epoch, PltGetMicroseconds(), 30000, VideoPacketFeedbackConnectionEpoch ? 0 : 250000);
    }
    VfInitialize(&packetFeedback, VideoPacketFeedbackConnectionEpoch);
    TpsInitializeReceiver(&transportPolicyStatus, VideoPacketFeedbackConnectionEpoch);
    unlockNetworkObserver();
    initializeVideoDepacketizer(StreamConfig.packetSize);
    RtpvInitializeQueue(&rtpQueue);
    decryptionCtx = PltCreateCryptoContext();
    receivedDataFromPeer = false;
    firstDataTimeMs = 0;
    receivedFullFrame = false;
    atomic_store_explicit(&rtpVideoBytesReceived, 0, memory_order_relaxed);
}

// Clean up the video stream
void destroyVideoStream(void) {
    lockNetworkObserver();
    videoStreamInitialized = false;
    packetControlNegotiated = false;
    unlockNetworkObserver();
    PltDestroyCryptoContext(decryptionCtx);
    destroyVideoDepacketizer();
    RtpvCleanupQueue(&rtpQueue);
}

// UDP Ping proc
static void VideoPingThreadProc(void* context) {
    char legacyPingData[] = { 0x50, 0x49, 0x4E, 0x47 };
    LC_SOCKADDR saddr;

    LC_ASSERT(VideoPortNumber != 0);

    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, VideoPortNumber);

    // We do not check for errors here. Socket errors will be handled
    // on the read-side in ReceiveThreadProc(). This avoids potential
    // issues related to receiving ICMP port unreachable messages due
    // to sending a packet prior to the host PC binding to that port.
    int pingCount = 0;
    while (!PltIsThreadInterrupted(&udpPingThread)) {
        if (VideoPingPayload.payload[0] != 0) {
            pingCount++;
            VideoPingPayload.sequenceNumber = BE32(pingCount);

            sendto(rtpSocket, (char*)&VideoPingPayload, sizeof(VideoPingPayload), 0, (struct sockaddr*)&saddr, AddrLen);
        }
        else {
            sendto(rtpSocket, legacyPingData, sizeof(legacyPingData), 0, (struct sockaddr*)&saddr, AddrLen);
        }

        PltSleepMsInterruptible(&udpPingThread, 500);
    }
}

// Receive thread proc
static void VideoReceiveThreadProc(void* context) {
    int err;
    int bufferSize, receiveSize, decryptedSize, minSize;
    char* buffer;
    char* encryptedBuffer;
    int queueStatus;
    bool useSelect;
    int waitingForVideoMs;
    bool encrypted;

    encrypted = !!(EncryptionFeaturesEnabled & SS_ENC_VIDEO);
    decryptedSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE +
        (VideoPacketFeedbackConnectionEpoch ? TF_VIDEO_IDENTITY_BYTES : 0);
    minSize = sizeof(RTP_PACKET) + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    receiveSize = decryptedSize + ((EncryptionFeaturesEnabled & SS_ENC_VIDEO) ? sizeof(ENC_VIDEO_HEADER) : 0);
    bufferSize = decryptedSize + sizeof(RTPV_QUEUE_ENTRY);
    buffer = NULL;

    if (setNonFatalRecvTimeoutMs(rtpSocket, UDP_RECV_POLL_TIMEOUT_MS) < 0) {
        // SO_RCVTIMEO failed, so use select() to wait
        useSelect = true;
    }
    else {
        // SO_RCVTIMEO timeout set for recv()
        useSelect = false;
    }

    // Allocate a staging buffer to use for each received packet
    if (encrypted) {
        encryptedBuffer = (char*)malloc(receiveSize);
        if (encryptedBuffer == NULL) {
            Limelog("Video Receive: malloc() failed\n");
            ListenerCallbacks.connectionTerminated(-1);
            return;
        }
    }
    else {
        encryptedBuffer = NULL;
    }

    waitingForVideoMs = 0;
    while (!PltIsThreadInterrupted(&receiveThread)) {
        PRTP_PACKET packet;

        if (buffer == NULL) {
            buffer = (char*)malloc(bufferSize);
            if (buffer == NULL) {
                Limelog("Video Receive: malloc() failed\n");
                ListenerCallbacks.connectionTerminated(-1);
                break;
            }
        }

        err = recvUdpSocket(rtpSocket,
                            encrypted ? encryptedBuffer : buffer,
                            receiveSize,
                            useSelect);
        // Save the earliest application receive timestamp, before decryption,
        // parsing and frame-queue work. This is not a kernel arrival timestamp.
        const uint64_t arrivalUs = networkObservationEnabled ? PltGetMicroseconds() : 0;
        if (err < 0) {
            Limelog("Video Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            break;
        }
        else if  (err == 0) {
            if (networkObservationEnabled) {
                lockNetworkObserver();
                VnAdvance(&networkObserver, arrivalUs);
                unlockNetworkObserver();
            }
            if (!receivedDataFromPeer) {
                // If we wait many seconds without ever receiving a video packet,
                // assume something is broken and terminate the connection.
                waitingForVideoMs += UDP_RECV_POLL_TIMEOUT_MS;
                if (waitingForVideoMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                    Limelog("Terminating connection due to lack of video traffic\n");
                    ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_TRAFFIC);
                    break;
                }
            }

            // Receive timed out; try again
            continue;
        }

        // Keep the wire-level receive size before decryption changes `err` to
        // the plaintext size. This is local accounting only; it does not
        // affect the RTP protocol or packet processing.
        const int receivedPacketLength = err;
        atomic_fetch_add_explicit(&rtpVideoBytesReceived,
                                  (uint64_t)receivedPacketLength,
                                  memory_order_relaxed);

        if (!receivedDataFromPeer) {
            receivedDataFromPeer = true;
            Limelog("Received first video packet after %d ms\n", waitingForVideoMs);

            firstDataTimeMs = PltGetMillis();
        }

#ifndef LC_FUZZING
        if (!receivedFullFrame) {
            if (PltGetMillis() - firstDataTimeMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                Limelog("Terminating connection due to lack of a successful video frame\n");
                ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_FRAME);
                break;
            }
        }
#endif

        if (err < minSize) {
            if (networkObservationEnabled) {
                lockNetworkObserver();
                networkObserver.snapshot.invalidPackets++;
                unlockNetworkObserver();
            }
            // Runt packet
            continue;
        }

        // Decrypt the packet into the buffer if encryption is enabled
        if (encrypted) {
            PENC_VIDEO_HEADER encHeader = (PENC_VIDEO_HEADER)encryptedBuffer;

            // If this frame is below our current frame number, discard it before decryption
            // to save CPU cycles decrypting FEC shards for a frame we already reassembled.
            //
            // Since this is happening _before_ decryption, this packet is not trusted yet.
            // It's imperative that we do not mutate any state based on this packet until
            // after it has been decrypted successfully!
            //
            // It's possible for an attacker to inject a fake packet that has any value of
            // header fields they want, however this provides them no benefit because we will
            // simply drop said packet here (if it's below the current frame number) or it
            // will pass this check and be dropped during decryption (if contents is tampered)
            // or after decryption in the RTP queue (if it's a replay of a previous authentic
            // packet from the host).
            //
            // In short, an attacker spoofing this value via MITM or sending malicious values
            // impersonating the host from off-link doesn't gain them anything. If they have
            // a true MITM, they can DoS our connection by just dropping all our traffic, so
            // tampering with packets to fail this check doesn't accomplish anything they
            // couldn't already do. If they're not on-link, we just throw their malicious
            // traffic away (as mentioned in the paragraph above) and continue accepting
            // legitmate video traffic.
            if (!networkObservationEnabled && encHeader->frameNumber &&
                LE32(encHeader->frameNumber) < RtpvGetCurrentFrameNumber(&rtpQueue)) {
                continue;
            }

            if (!PltDecryptMessage(decryptionCtx, ALGORITHM_AES_GCM, 0,
                                   (unsigned char*)StreamConfig.remoteInputAesKey, sizeof(StreamConfig.remoteInputAesKey),
                                   encHeader->iv, sizeof(encHeader->iv),
                                   encHeader->tag, sizeof(encHeader->tag),
                                   ((unsigned char*)(encHeader + 1)), err - sizeof(ENC_VIDEO_HEADER), // The ciphertext is after the header
                                   (unsigned char*)buffer, &err)) {
                Limelog("Failed to decrypt video packet!\n");
                if (networkObservationEnabled) {
                    lockNetworkObserver();
                    networkObserver.snapshot.authenticationFailures++;
                    unlockNetworkObserver();
                }
                continue;
            }
        }

        uint64_t transportEpoch = 0, transportSequence = 0;
        if (VideoPacketFeedbackConnectionEpoch) {
            if (!encrypted || err < TF_VIDEO_IDENTITY_BYTES ||
                !TfDecodeVideoIdentity((const uint8_t*)buffer, TF_VIDEO_IDENTITY_BYTES, &transportEpoch, &transportSequence) ||
                transportEpoch != VideoPacketFeedbackConnectionEpoch) {
                lockNetworkObserver(); networkObserver.snapshot.invalidPackets++; unlockNetworkObserver();
                continue;
            }
            err -= TF_VIDEO_IDENTITY_BYTES;
            memmove(buffer, buffer + TF_VIDEO_IDENTITY_BYTES, (size_t)err);
        }
        bool validTransportIdentity = true;
        if (networkObservationEnabled) {
            lockNetworkObserver();
            if (VideoPacketFeedbackConnectionEpoch) {
                uint16_t rtpSequence;
                uint32_t transportSequence24;
                if (!VnParseVideoPayload((const uint8_t*)buffer, (size_t)err, rtpQueue.multiFecCapable,
                    &rtpSequence, &transportSequence24) || rtpSequence != (uint16_t)transportSequence ||
                    (transportSequence24 != UINT32_MAX &&
                     transportSequence24 != (uint32_t)(transportSequence & 0xffffffu))) validTransportIdentity = false;
                else validTransportIdentity = VfObserveAuthenticated(&packetFeedback, &networkObserver, transportEpoch, transportSequence,
                    (uint32_t)receivedPacketLength, arrivalUs);
                if (!validTransportIdentity) networkObserver.snapshot.invalidPackets++;
            }
            else VnObserveVideoPayload(&networkObserver, (const uint8_t*)buffer, (size_t)err,
                    (uint32_t)receivedPacketLength, arrivalUs, rtpQueue.multiFecCapable);
            unlockNetworkObserver();
        }

        if (!validTransportIdentity || err < (int)sizeof(RTP_PACKET)) {
            continue;
        }

        // Convert fields to host byte-order
        packet = (PRTP_PACKET)&buffer[0];
        packet->sequenceNumber = BE16(packet->sequenceNumber);
        packet->timestamp = BE32(packet->timestamp);
        packet->ssrc = BE32(packet->ssrc);

        queueStatus = RtpvAddPacket(&rtpQueue, packet, err, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);

        if (queueStatus == RTPF_RET_QUEUED) {
            // The queue owns the buffer
            buffer = NULL;
        }
    }

    if (buffer != NULL) {
        free(buffer);
    }

    if (encryptedBuffer != NULL) {
        free(encryptedBuffer);
    }
}

void notifyKeyFrameReceived(void) {
    // Remember that we got a full frame successfully
    receivedFullFrame = true;
}

// Decoder thread proc
static void VideoDecoderThreadProc(void* context) {
    while (!PltIsThreadInterrupted(&decoderThread)) {
        VIDEO_FRAME_HANDLE frameHandle;
        PDECODE_UNIT decodeUnit;

        if (!LiWaitForNextVideoFrame(&frameHandle, &decodeUnit)) {
            return;
        }

        LiCompleteVideoFrame(frameHandle, VideoCallbacks.submitDecodeUnit(decodeUnit));
    }
}

// Read the first frame of the video stream
int readFirstFrame(void) {
    // All that matters is that we close this socket.
    // This starts the flow of video on Gen 3 servers.

    closeSocket(firstFrameSocket);
    firstFrameSocket = INVALID_SOCKET;

    return 0;
}

// Terminate the video stream
void stopVideoStream(void) {
    if (!receivedDataFromPeer) {
        Limelog("No video traffic was ever received from the host!\n");
    }

    VideoCallbacks.stop();

    // Wake up client code that may be waiting on the decode unit queue
    stopVideoDepacketizer();

    PltInterruptThread(&udpPingThread);
    PltInterruptThread(&receiveThread);
    if (videoNeedsDecoderThread()) {
        PltInterruptThread(&decoderThread);
    }

    if (firstFrameSocket != INVALID_SOCKET) {
        shutdownTcpSocket(firstFrameSocket);
    }

    PltJoinThread(&udpPingThread);
    PltJoinThread(&receiveThread);
    if (videoNeedsDecoderThread()) {
        PltJoinThread(&decoderThread);
    }

    if (firstFrameSocket != INVALID_SOCKET) {
        closeSocket(firstFrameSocket);
        firstFrameSocket = INVALID_SOCKET;
    }
    if (rtpSocket != INVALID_SOCKET) {
        closeSocket(rtpSocket);
        rtpSocket = INVALID_SOCKET;
    }

    VideoCallbacks.cleanup();
}

// Start the video stream
int startVideoStream(void* rendererContext, int drFlags) {
    int err;

    firstFrameSocket = INVALID_SOCKET;

    // This must be called before the decoder thread starts submitting
    // decode units
    LC_ASSERT(NegotiatedVideoFormat != 0);
    err = VideoCallbacks.setup(NegotiatedVideoFormat, StreamConfig.width,
        StreamConfig.height, StreamConfig.fps, rendererContext, drFlags);
    if (err != 0) {
        return err;
    }

    int recvPackets = (StreamConfig.streamingRemotely == STREAM_CFG_REMOTE)
        ? RTP_RECV_PACKETS_REMOTE : RTP_RECV_PACKETS_LOCAL;
    rtpSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen,
                              recvPackets * (StreamConfig.packetSize + MAX_RTP_HEADER_SIZE),
                              SOCK_QOS_TYPE_VIDEO);
    if (rtpSocket == INVALID_SOCKET) {
        VideoCallbacks.cleanup();
        return LastSocketError();
    }
    LiRecordStreamSocket(rtpSocket, STREAM_SOCKET_SLOT_VIDEO);

    VideoCallbacks.start();

    err = PltCreateThread("VideoRecv", VideoReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        VideoCallbacks.stop();
        closeSocket(rtpSocket);
        VideoCallbacks.cleanup();
        return err;
    }

    if (videoNeedsDecoderThread()) {
        err = PltCreateThread("VideoDec", VideoDecoderThreadProc, NULL, &decoderThread);
        if (err != 0) {
            VideoCallbacks.stop();
            PltInterruptThread(&receiveThread);
            PltJoinThread(&receiveThread);
            closeSocket(rtpSocket);
            VideoCallbacks.cleanup();
            return err;
        }
    }

    if (AppVersionQuad[0] == 3) {
        // Connect this socket to open port 47998 for our ping thread
        firstFrameSocket = connectTcpSocket(&RemoteAddr, AddrLen,
                                            FIRST_FRAME_PORT, FIRST_FRAME_TIMEOUT_SEC);
        if (firstFrameSocket == INVALID_SOCKET) {
            VideoCallbacks.stop();
            stopVideoDepacketizer();
            PltInterruptThread(&receiveThread);
            if (videoNeedsDecoderThread()) {
                PltInterruptThread(&decoderThread);
            }
            PltJoinThread(&receiveThread);
            if (videoNeedsDecoderThread()) {
                PltJoinThread(&decoderThread);
            }
            closeSocket(rtpSocket);
            VideoCallbacks.cleanup();
            return LastSocketError();
        }
    }

    // Start pinging before reading the first frame so GFE knows where
    // to send UDP data
    err = PltCreateThread("VideoPing", VideoPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        VideoCallbacks.stop();
        stopVideoDepacketizer();
        PltInterruptThread(&receiveThread);
        if (videoNeedsDecoderThread()) {
            PltInterruptThread(&decoderThread);
        }
        PltJoinThread(&receiveThread);
        if (videoNeedsDecoderThread()) {
            PltJoinThread(&decoderThread);
        }
        closeSocket(rtpSocket);
        if (firstFrameSocket != INVALID_SOCKET) {
            closeSocket(firstFrameSocket);
            firstFrameSocket = INVALID_SOCKET;
        }
        VideoCallbacks.cleanup();
        return err;
    }

    if (AppVersionQuad[0] == 3) {
        // Read the first frame to start the flow of video
        err = readFirstFrame();
        if (err != 0) {
            stopVideoStream();
            return err;
        }
    }

    return 0;
}

const RTP_VIDEO_STATS* LiGetRTPVideoStats(void) {
    return &rtpQueue.stats;
}

uint64_t LiGetRTPVideoBytesReceived(void) {
    return atomic_load_explicit(&rtpVideoBytesReceived, memory_order_relaxed);
}
