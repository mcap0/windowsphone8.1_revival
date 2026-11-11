#ifndef TG_NATIVE_MTPROTO_AUTH_H
#define TG_NATIVE_MTPROTO_AUTH_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tg {

std::vector<uint8_t> buildReqPqMulti(const uint8_t nonce[16]);

struct ResPQ {
    uint8_t nonce[16];
    uint8_t serverNonce[16];
    std::vector<uint8_t> pq;
    std::vector<uint64_t> fingerprints;
};

ResPQ parseResPQ(const uint8_t* data, size_t len);

std::vector<uint8_t> buildPqInnerDataDc(
    const std::vector<uint8_t>& pq,
    const std::vector<uint8_t>& p,
    const std::vector<uint8_t>& q,
    const uint8_t nonce[16],
    const uint8_t serverNonce[16],
    const uint8_t newNonce[32],
    int32_t dc);

// Builds the 256-byte pre-RSA block from a 192-byte padded payload and a
// 32-byte AES key. Exposed so the RSA_PAD construction can be tested directly.
std::vector<uint8_t> makeRsaPadBlock(
    const uint8_t paddedData[192],
    const uint8_t aesKey[32]);

uint64_t rsaFingerprint(const uint8_t* n, size_t nLen, uint32_t e);

std::vector<uint8_t> rsaPadAndEncrypt(
    const uint8_t* data,
    size_t len,
    const uint8_t* n,
    size_t nLen,
    uint32_t e,
    const uint8_t* randomBytes,
    size_t randomLen);

} // namespace tg

#endif
