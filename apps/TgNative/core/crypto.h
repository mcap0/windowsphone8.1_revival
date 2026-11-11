#ifndef TG_CRYPTO_H
#define TG_CRYPTO_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tg {

std::vector<uint8_t> sha1(const uint8_t* data, std::size_t len);
std::vector<uint8_t> sha256(const uint8_t* data, std::size_t len);

void aesIgeEncrypt(const uint8_t key[32], const uint8_t iv[32],
                   uint8_t* data, std::size_t len);
void aesIgeDecrypt(const uint8_t key[32], const uint8_t iv[32],
                   uint8_t* data, std::size_t len);

void factorPq(uint64_t pq, uint64_t& p, uint64_t& q);
uint64_t mulMod(uint64_t a, uint64_t b, uint64_t mod);
uint64_t modPow(uint64_t base, uint64_t exp, uint64_t mod);

std::vector<uint8_t> rsaRawEncrypt(const uint8_t* n, std::size_t nLen,
                                   uint32_t e, const uint8_t* m,
                                   std::size_t mLen);

} // namespace tg

#endif
