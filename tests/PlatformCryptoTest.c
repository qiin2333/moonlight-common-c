#include "PlatformCrypto.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static unsigned failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); failures++; \
} } while (0)

int main(void) {
    // NIST AES-128-GCM known-answer vector: zero key, 96-bit zero IV,
    // one zero plaintext block and no additional authenticated data.
    unsigned char key[16] = {0}, iv[12] = {0}, input[16] = {0};
    const unsigned char expectedCiphertext[16] = {
        0x03,0x88,0xda,0xce,0x60,0xb6,0xa3,0x92,0xf3,0x28,0xc2,0xb9,0x71,0xb2,0xfe,0x78
    };
    const unsigned char expectedTag[16] = {
        0xab,0x6e,0x47,0xd4,0x2c,0xec,0x13,0xbd,0xf5,0x3a,0x67,0xb2,0x12,0x57,0xbd,0xdf
    };
    unsigned char ciphertext[64] = {0}, plaintext[64] = {0}, tag[16] = {0};
    int ciphertextLength = -1, plaintextLength = -1;
    PPLT_CRYPTO_CONTEXT encrypt = PltCreateCryptoContext();
    PPLT_CRYPTO_CONTEXT decrypt = PltCreateCryptoContext();
    CHECK(encrypt != NULL && decrypt != NULL);
    if (encrypt == NULL || decrypt == NULL) {
        PltDestroyCryptoContext(encrypt);
        PltDestroyCryptoContext(decrypt);
        return 1;
    }
    CHECK(PltEncryptMessage(encrypt, ALGORITHM_AES_GCM, 0, key, sizeof(key),
          iv, sizeof(iv), tag, sizeof(tag), input, sizeof(input), ciphertext, &ciphertextLength));
    CHECK(ciphertextLength == (int)sizeof(input));
    CHECK(memcmp(ciphertext, expectedCiphertext, sizeof(expectedCiphertext)) == 0);
    CHECK(memcmp(tag, expectedTag, sizeof(expectedTag)) == 0);
    CHECK(PltDecryptMessage(decrypt, ALGORITHM_AES_GCM, 0, key, sizeof(key),
          iv, sizeof(iv), tag, sizeof(tag), ciphertext, sizeof(input), plaintext, &plaintextLength));
    CHECK(plaintextLength == (int)sizeof(input));
    CHECK(memcmp(plaintext, input, sizeof(input)) == 0);
    tag[0] ^= 1;
    CHECK(!PltDecryptMessage(decrypt, ALGORITHM_AES_GCM, 0, key, sizeof(key),
          iv, sizeof(iv), tag, sizeof(tag), ciphertext, sizeof(input), plaintext, &plaintextLength));
    tag[0] ^= 1;
    CHECK(PltDecryptMessage(decrypt, ALGORITHM_AES_GCM, 0, key, sizeof(key),
          iv, sizeof(iv), tag, sizeof(tag), ciphertext, sizeof(input), plaintext, &plaintextLength));
    CHECK(plaintextLength == (int)sizeof(input));
    CHECK(memcmp(plaintext, input, sizeof(input)) == 0);
    // Reuse each key context for a new nonce and a non-block-aligned packet.
    unsigned char packet[31];
    for (unsigned i = 0; i < sizeof(packet); i++) packet[i] = (unsigned char)(i + 1);
    iv[11] = 1;
    CHECK(PltEncryptMessage(encrypt, ALGORITHM_AES_GCM, 0, key, sizeof(key),
          iv, sizeof(iv), tag, sizeof(tag), packet, sizeof(packet), ciphertext, &ciphertextLength));
    CHECK(ciphertextLength == (int)sizeof(packet));
    CHECK(PltDecryptMessage(decrypt, ALGORITHM_AES_GCM, 0, key, sizeof(key),
          iv, sizeof(iv), tag, sizeof(tag), ciphertext, sizeof(packet), plaintext, &plaintextLength));
    CHECK(plaintextLength == (int)sizeof(packet));
    CHECK(memcmp(plaintext, packet, sizeof(packet)) == 0);
    PltDestroyCryptoContext(encrypt);
    PltDestroyCryptoContext(decrypt);
    printf("AES-GCM known answer, authentication rejection and reuse: %u failures\n", failures);
    return failures != 0;
}
