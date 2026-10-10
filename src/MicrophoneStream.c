#include "Limelight-internal.h"
#include "PlatformSockets.h"

#define MIC_IV_LEN 16
#define MIC_HEADER_FLAGS 0x00

static SOCKET micSocket = INVALID_SOCKET;
static PPLT_CRYPTO_CONTEXT micEncryptionCtx = NULL;

// Microphone encryption state
static uint32_t micRiKeyId = 0;
static uint16_t micSequenceNumber = 0;

#pragma pack(push, 1)
typedef struct _MICROPHONE_PACKET_HEADER {
    uint8_t flags;
    uint8_t packetType;
    uint16_t sequenceNumber;
    uint32_t timestamp;
    uint32_t ssrc;
} MICROPHONE_PACKET_HEADER, *PMICROPHONE_PACKET_HEADER;
#pragma pack(pop)

int initializeMicrophoneStream(void) {
    if (micSocket != INVALID_SOCKET) {
        return 0;
    }
    
    micEncryptionCtx = PltCreateCryptoContext();
    if (micEncryptionCtx == NULL) {
        return -1;
    }
    
    // Initialize riKeyId from the first 4 bytes of remoteInputAesIv
    memcpy(&micRiKeyId, StreamConfig.remoteInputAesIv, sizeof(micRiKeyId));
    micRiKeyId = BE32(micRiKeyId);
    
    micSequenceNumber = 0;
    
    micSocket = bindUdpSocket(RemoteAddr.ss_family, &LocalAddr, AddrLen, 0, SOCK_QOS_TYPE_AUDIO);
    if (micSocket == INVALID_SOCKET) {
        PltDestroyCryptoContext(micEncryptionCtx);
        micEncryptionCtx = NULL;
        return LastSocketFail();
    }
    
    return 0;
}

void destroyMicrophoneStream(void) {
    if (micSocket != INVALID_SOCKET) {
        closeSocket(micSocket);
        micSocket = INVALID_SOCKET;
    }
    
    if (micEncryptionCtx != NULL) {
        PltDestroyCryptoContext(micEncryptionCtx);
        micEncryptionCtx = NULL;
    }
    
    micRiKeyId = 0;
    micSequenceNumber = 0;
}

int sendMicrophoneOpusData(const unsigned char* opusData, int opusLength) {
    LC_SOCKADDR saddr;
    MICROPHONE_PACKET_HEADER header;
    unsigned char packet[MAX_MIC_PACKET_SIZE];
    int packetLength = 0;
    int err = 0;
    
    // Validate socket is initialized
    if (micSocket == INVALID_SOCKET) {
        return -1;
    }
    
    // Validate input parameters
    if (opusData == NULL || opusLength <= 0) {
        return -1;
    }
    
    // Validate opusLength doesn't exceed max payload size
    if (opusLength > MAX_MIC_PACKET_SIZE - (int)sizeof(header)) {
        Limelog("MIC: Input data too large (%d)\n", opusLength);
        return -1;
    }
    
    // Initialize header
    memset(&header, 0, sizeof(header));
    header.flags = MIC_HEADER_FLAGS;
    header.packetType = MIC_PACKET_TYPE_OPUS;
    header.sequenceNumber = LE16(micSequenceNumber);
    header.timestamp = LE32((uint32_t)PltGetMillis());
    header.ssrc = LE32(MIC_PACKET_MAGIC);
    
    if (EncryptionFeaturesEnabled & SS_ENC_MICROPHONE) {
        // Once encryption is negotiated, never fall back to sending plaintext
        if (micEncryptionCtx == NULL) {
            return -1;
        }

        unsigned char iv[MIC_IV_LEN] = {0};
        // Room for the Opus data plus a full padding block
        unsigned char paddedData[MAX_MIC_PACKET_SIZE + 16];
        unsigned char encryptedData[sizeof(paddedData)];
        int encryptedLength = (int)sizeof(encryptedData);

        // The host decrypts with standard PKCS7, stripping exactly one pad of
        // 1-16 bytes. Append that pad here so the plaintext is block aligned,
        // then encrypt the blocks as they are: CIPHER_FLAG_PAD_TO_BLOCK_SIZE adds
        // nothing to aligned input, and without CIPHER_FLAG_FINISH no backend
        // appends a padding block of its own. (Passing FINISH as well padded
        // twice on OpenSSL, so the host fed 1-15 junk bytes to its Opus decoder
        // with almost every packet, which garbled the voice.)
        int padLength = 16 - (opusLength % 16);
        int paddedLength = opusLength + padLength;
        memcpy(paddedData, opusData, opusLength);
        memset(&paddedData[opusLength], padLength, padLength);

        // IV = riKeyId + sequenceNumber in big-endian
        uint32_t ivSeq = BE32(micRiKeyId + micSequenceNumber);
        memcpy(iv, &ivSeq, sizeof(ivSeq));

        if (!PltEncryptMessage(micEncryptionCtx,
                              ALGORITHM_AES_CBC,
                              CIPHER_FLAG_RESET_IV | CIPHER_FLAG_PAD_TO_BLOCK_SIZE,
                              (unsigned char*)StreamConfig.remoteInputAesKey,
                              sizeof(StreamConfig.remoteInputAesKey),
                              iv, sizeof(iv),
                              NULL, 0,
                              paddedData, paddedLength,
                              encryptedData, &encryptedLength)) {
            Limelog("MIC: Encryption failed\n");
            return -1;
        }
        
        // Validate encrypted length is reasonable
        if (encryptedLength < 0 || encryptedLength > (int)sizeof(encryptedData)) {
            Limelog("MIC: Invalid encrypted length (%d)\n", encryptedLength);
            return -1;
        }
        
        packetLength = (int)sizeof(header) + encryptedLength;
        if (packetLength > MAX_MIC_PACKET_SIZE || packetLength > (int)sizeof(packet)) {
            Limelog("MIC: Encrypted packet too large (%d > %d)\n", packetLength, MAX_MIC_PACKET_SIZE);
            return -1;
        }
        
        // Safe copy with bounds check
        memcpy(packet, &header, sizeof(header));
        memcpy(packet + sizeof(header), encryptedData, encryptedLength);
    }
    else {
        packetLength = (int)sizeof(header) + opusLength;
        if (packetLength > MAX_MIC_PACKET_SIZE || packetLength > (int)sizeof(packet)) {
            Limelog("MIC: Packet too large (%d > %d)\n", packetLength, MAX_MIC_PACKET_SIZE);
            return -1;
        }
        
        // Safe copy with bounds check
        memcpy(packet, &header, sizeof(header));
        memcpy(packet + sizeof(header), opusData, opusLength);
    }
    
    ++micSequenceNumber;
    
    memcpy(&saddr, &RemoteAddr, sizeof(saddr));
    SET_PORT(&saddr, MicPortNumber);
    
    err = sendto(micSocket, (const char*)packet, packetLength, 0, (struct sockaddr*)&saddr, AddrLen);
    if (err < 0) {
        return LastSocketError();
    }
    
    return err;
}

bool isMicrophoneEncryptionEnabled(void) {
    return (EncryptionFeaturesEnabled & SS_ENC_MICROPHONE) != 0;
}
