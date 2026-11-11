#ifndef TG_MTPROTO_DH_H
#define TG_MTPROTO_DH_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tg {

extern const uint32_t SERVER_DH_PARAMS_OK_ID;
extern const uint32_t SERVER_DH_INNER_DATA_ID;
extern const uint32_t CLIENT_DH_INNER_DATA_ID;
extern const uint32_t SET_CLIENT_DH_PARAMS_ID;
extern const uint32_t DH_GEN_OK_ID;

struct ServerDhInner {
    std::vector<uint8_t> nonce;
    std::vector<uint8_t> serverNonce;
    uint32_t g;
    std::vector<uint8_t> dhPrime;
    std::vector<uint8_t> gA;
    uint32_t serverTime;

    ServerDhInner();
};

void deriveTmpAes(const uint8_t newNonce[32],
                  const uint8_t serverNonce[16],
                  uint8_t key[32],
                  uint8_t iv[32]);

ServerDhInner parseServerDhParams(const uint8_t* encryptedAnswer,
                                  size_t len,
                                  const uint8_t nonce[16],
                                  const uint8_t serverNonce[16],
                                  const uint8_t newNonce[32]);

ServerDhInner parseServerDhParams(const uint8_t* encryptedAnswer,
                                  size_t len,
                                  const std::vector<uint8_t>& nonce,
                                  const std::vector<uint8_t>& serverNonce,
                                  const std::vector<uint8_t>& newNonce);

bool checkDhParams(const ServerDhInner& inner);

// These helpers expose the existing core AES-IGE implementation using fixed-size
// MTProto keys and IVs.
std::vector<uint8_t> aesIgeEncryptBytes(const std::vector<uint8_t>& data,
                                        const uint8_t key[32],
                                        const uint8_t iv[32]);
std::vector<uint8_t> aesIgeDecryptBytes(const std::vector<uint8_t>& data,
                                        const uint8_t key[32],
                                        const uint8_t iv[32]);

std::vector<uint8_t> sha1Bytes(const uint8_t* data, size_t len);
std::vector<uint8_t> sha1Bytes(const std::vector<uint8_t>& data);

// randomB is interpreted as a 256-byte big-endian integer. The returned g_b
// and authKey are both padded to 256 bytes.
std::vector<uint8_t> computeClientDh(const ServerDhInner& inner,
                                     const uint8_t randomB[256],
                                     uint8_t authKey[256],
                                     uint64_t& authKeyId);

uint64_t deriveAuthKeyId(const uint8_t authKey[256]);
std::vector<uint8_t> deriveServerSalt(const uint8_t newNonce[32],
                                     const uint8_t serverNonce[16]);

std::vector<uint8_t> buildClientDhInnerEncrypted(
    const ServerDhInner& inner,
    const uint8_t newNonce[32],
    const std::vector<uint8_t>& gB,
    const uint8_t* randomPadding,
    size_t randomPaddingLength);

std::vector<uint8_t> buildClientDhInnerEncrypted(
    const uint8_t nonce[16],
    const uint8_t serverNonce[16],
    const uint8_t newNonce[32],
    const std::vector<uint8_t>& gB,
    const uint8_t* randomPadding,
    size_t randomPaddingLength);

std::vector<uint8_t> computeDhGenNewNonceHash1(const uint8_t newNonce[32],
                                                const uint8_t authKey[256]);
void checkDhGenOk(const uint8_t newNonce[32],
                  const uint8_t authKey[256],
                  const uint8_t serverNewNonceHash1[16]);

} // namespace tg

#endif
