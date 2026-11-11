#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "mtproto_dh.h"

#include "crypto.h"
#include "tl.h"

#include <mbedtls/bignum.h>
#include <mbedtls/version.h>

#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>

namespace tg {

const uint32_t SERVER_DH_PARAMS_OK_ID = 0xd0e8075cU;
const uint32_t SERVER_DH_INNER_DATA_ID = 0xb5890dbaU;
const uint32_t CLIENT_DH_INNER_DATA_ID = 0x6643b654U;
const uint32_t SET_CLIENT_DH_PARAMS_ID = 0xf5045f1fU;
const uint32_t DH_GEN_OK_ID = 0x3bcbf734U;

namespace {

class Mpi {
public:
    Mpi()
    {
        mbedtls_mpi_init(&value_);
    }

    ~Mpi()
    {
        mbedtls_mpi_free(&value_);
    }

    mbedtls_mpi* get()
    {
        return &value_;
    }

    const mbedtls_mpi* get() const
    {
        return &value_;
    }

private:
    Mpi(const Mpi&);
    Mpi& operator=(const Mpi&);

    mbedtls_mpi value_;
};

void requireMpi(int result, const char* message)
{
    if (result != 0) {
        throw std::runtime_error(message);
    }
}

uint32_t readU32(const std::vector<uint8_t>& bytes, size_t& offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("TL int truncated");
    }

    const uint32_t value =
        static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    offset += 4;
    return value;
}

void writeU32(std::vector<uint8_t>& bytes, uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

void writeU64(std::vector<uint8_t>& bytes, uint64_t value)
{
    unsigned int i;
    for (i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

void writeTlBytes(std::vector<uint8_t>& bytes,
                  const uint8_t* value,
                  size_t length)
{
    if (length > 0x00ffffffU) {
        throw std::runtime_error("TL byte string is too long");
    }
    if (length != 0 && value == NULL) {
        throw std::runtime_error("TL byte string has no data");
    }

    if (length <= 253) {
        bytes.push_back(static_cast<uint8_t>(length));
    } else {
        bytes.push_back(254);
        bytes.push_back(static_cast<uint8_t>(length));
        bytes.push_back(static_cast<uint8_t>(length >> 8));
        bytes.push_back(static_cast<uint8_t>(length >> 16));
    }

    if (length != 0) {
        bytes.insert(bytes.end(), value, value + length);
    }
    while ((bytes.size() & 3U) != 0) {
        bytes.push_back(0);
    }
}

std::vector<uint8_t> readTlBytes(const std::vector<uint8_t>& bytes,
                                 size_t& offset)
{
    if (offset >= bytes.size()) {
        throw std::runtime_error("TL byte string truncated");
    }

    const uint8_t marker = bytes[offset++];
    size_t length = 0;
    size_t prefixSize = 1;

    if (marker < 254) {
        length = marker;
    } else if (marker == 254) {
        if (offset > bytes.size() || bytes.size() - offset < 3) {
            throw std::runtime_error("TL long byte string truncated");
        }
        length = static_cast<size_t>(bytes[offset]) |
                 (static_cast<size_t>(bytes[offset + 1]) << 8) |
                 (static_cast<size_t>(bytes[offset + 2]) << 16);
        offset += 3;
        prefixSize = 4;
        if (length <= 253) {
            throw std::runtime_error("Non-canonical TL byte string length");
        }
    } else {
        throw std::runtime_error("Invalid TL byte string marker");
    }

    if (offset > bytes.size() || length > bytes.size() - offset) {
        throw std::runtime_error("TL byte string data truncated");
    }

    std::vector<uint8_t> result(bytes.begin() + offset,
                                bytes.begin() + offset + length);
    offset += length;

    const size_t padding =
        (4 - ((prefixSize + length) & 3U)) & 3U;
    if (offset > bytes.size() || padding > bytes.size() - offset) {
        throw std::runtime_error("TL byte string padding truncated");
    }
    size_t i;
    for (i = 0; i < padding; ++i) {
        if (bytes[offset + i] != 0) {
            throw std::runtime_error("Non-zero TL byte string padding");
        }
    }
    offset += padding;
    return result;
}

uint32_t rotateLeft(uint32_t value, unsigned int count)
{
    return (value << count) | (value >> (32 - count));
}

std::vector<uint8_t> sha1Internal(const uint8_t* data, size_t len)
{
    if (len != 0 && data == NULL) {
        throw std::runtime_error("SHA-1 input is null");
    }
    if (len > (std::numeric_limits<uint64_t>::max() / 8U)) {
        throw std::runtime_error("SHA-1 input is too long");
    }

    const uint64_t bitLength = static_cast<uint64_t>(len) * 8U;
    size_t paddedLength = len + 1;
    while ((paddedLength & 63U) != 56U) {
        if (paddedLength == (std::numeric_limits<size_t>::max)()) {
            throw std::runtime_error("SHA-1 input is too long");
        }
        ++paddedLength;
    }
    if (paddedLength > (std::numeric_limits<size_t>::max)() - 8U) {
        throw std::runtime_error("SHA-1 input is too long");
    }
    paddedLength += 8;

    std::vector<uint8_t> padded(paddedLength, 0);
    if (len != 0) {
        std::memcpy(&padded[0], data, len);
    }
    padded[len] = 0x80;

    unsigned int i;
    for (i = 0; i < 8; ++i) {
        padded[paddedLength - 1 - i] =
            static_cast<uint8_t>(bitLength >> (i * 8));
    }

    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xefcdab89U;
    uint32_t h2 = 0x98badcfeU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xc3d2e1f0U;

    size_t block;
    for (block = 0; block < paddedLength; block += 64) {
        uint32_t w[80];

        for (i = 0; i < 16; ++i) {
            const size_t position = block + i * 4;
            w[i] = (static_cast<uint32_t>(padded[position]) << 24) |
                   (static_cast<uint32_t>(padded[position + 1]) << 16) |
                   (static_cast<uint32_t>(padded[position + 2]) << 8) |
                   static_cast<uint32_t>(padded[position + 3]);
        }
        for (i = 16; i < 80; ++i) {
            w[i] = rotateLeft(
                w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        unsigned int t;
        for (t = 0; t < 80; ++t) {
            uint32_t f;
            uint32_t k;

            if (t < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999U;
            } else if (t < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1U;
            } else if (t < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcU;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6U;
            }

            const uint32_t temp =
                rotateLeft(a, 5) + f + e + k + w[t];
            e = d;
            d = c;
            c = rotateLeft(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    std::vector<uint8_t> digest(20);
    const uint32_t words[5] = { h0, h1, h2, h3, h4 };
    for (i = 0; i < 5; ++i) {
        digest[i * 4] = static_cast<uint8_t>(words[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(words[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(words[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(words[i]);
    }
    return digest;
}

bool equalsBytes(const uint8_t* left,
                 const uint8_t* right,
                 size_t length)
{
    return length == 0 || std::memcmp(left, right, length) == 0;
}

int deterministicPrimeRng(void* state,
                          unsigned char* output,
                          size_t length)
{
    uint32_t* value = static_cast<uint32_t*>(state);
    size_t i;

    for (i = 0; i < length; ++i) {
        *value ^= *value << 13;
        *value ^= *value >> 17;
        *value ^= *value << 5;
        output[i] = static_cast<unsigned char>(*value);
    }
    return 0;
}

int isPrime(const mbedtls_mpi* value,
            int (*rng)(void*, unsigned char*, size_t),
            void* rngState)
{
#if defined(MBEDTLS_VERSION_NUMBER) && MBEDTLS_VERSION_NUMBER >= 0x03000000
    return mbedtls_mpi_is_prime_ext(value, 40, rng, rngState);
#else
    return mbedtls_mpi_is_prime(value, rng, rngState);
#endif
}

void readMpi(Mpi& value,
             const std::vector<uint8_t>& bytes,
             const char* error)
{
    if (bytes.empty()) {
        throw std::runtime_error(error);
    }
    requireMpi(mbedtls_mpi_read_binary(value.get(), &bytes[0], bytes.size()),
               error);
}

std::vector<uint8_t> writeMpiPadded256(const mbedtls_mpi* value)
{
    if (mbedtls_mpi_bitlen(value) > 2048U) {
        throw std::runtime_error("DH result exceeds 256 bytes");
    }

    std::vector<uint8_t> result(256, 0);
    requireMpi(mbedtls_mpi_write_binary(value, &result[0], result.size()),
               "Could not serialize DH result");
    return result;
}

std::vector<uint8_t> serializeClientDhInner(const uint8_t nonce[16],
                                            const uint8_t serverNonce[16],
                                            const std::vector<uint8_t>& gB)
{
    if (nonce == NULL || serverNonce == NULL) {
        throw std::runtime_error("Client DH nonces are null");
    }

    std::vector<uint8_t> data;
    writeU32(data, CLIENT_DH_INNER_DATA_ID);
    data.insert(data.end(), nonce, nonce + 16);
    data.insert(data.end(), serverNonce, serverNonce + 16);
    writeU64(data, 0);
    writeTlBytes(data, gB.empty() ? NULL : &gB[0], gB.size());
    return data;
}

} // namespace

ServerDhInner::ServerDhInner()
    : g(0), serverTime(0)
{
}

void deriveTmpAes(const uint8_t newNonce[32],
                  const uint8_t serverNonce[16],
                  uint8_t key[32],
                  uint8_t iv[32])
{
    if (newNonce == NULL || serverNonce == NULL ||
        key == NULL || iv == NULL) {
        throw std::runtime_error(
            "Temporary AES derivation received a null buffer");
    }

    std::vector<uint8_t> nonceAndServer;
    nonceAndServer.insert(nonceAndServer.end(), newNonce, newNonce + 32);
    nonceAndServer.insert(
        nonceAndServer.end(), serverNonce, serverNonce + 16);

    std::vector<uint8_t> serverAndNonce;
    serverAndNonce.insert(
        serverAndNonce.end(), serverNonce, serverNonce + 16);
    serverAndNonce.insert(serverAndNonce.end(), newNonce, newNonce + 32);

    std::vector<uint8_t> nonceAndNonce;
    nonceAndNonce.insert(nonceAndNonce.end(), newNonce, newNonce + 32);
    nonceAndNonce.insert(nonceAndNonce.end(), newNonce, newNonce + 32);

    const std::vector<uint8_t> shaA =
        sha1Internal(&nonceAndServer[0], nonceAndServer.size());
    const std::vector<uint8_t> shaB =
        sha1Internal(&serverAndNonce[0], serverAndNonce.size());
    const std::vector<uint8_t> shaC =
        sha1Internal(&nonceAndNonce[0], nonceAndNonce.size());

    std::memcpy(key, &shaA[0], 20);
    std::memcpy(key + 20, &shaB[0], 12);

    std::memcpy(iv, &shaB[12], 8);
    std::memcpy(iv + 8, &shaC[0], 20);
    std::memcpy(iv + 28, newNonce, 4);
}

std::vector<uint8_t> sha1Bytes(const uint8_t* data, size_t len)
{
    return sha1Internal(data, len);
}

std::vector<uint8_t> sha1Bytes(const std::vector<uint8_t>& data)
{
    return sha1Internal(data.empty() ? NULL : &data[0], data.size());
}

std::vector<uint8_t> aesIgeEncryptBytes(const std::vector<uint8_t>& data,
                                        const uint8_t key[32],
                                        const uint8_t iv[32])
{
    if (key == NULL || iv == NULL) {
        throw std::runtime_error("AES key or IV is null");
    }
    if ((data.size() & 15U) != 0) {
        throw std::runtime_error("AES-IGE input length is not block aligned");
    }

    std::vector<uint8_t> result(data);
    aesIgeEncrypt(key, iv, result.empty() ? NULL : &result[0], result.size());
    return result;
}

std::vector<uint8_t> aesIgeDecryptBytes(const std::vector<uint8_t>& data,
                                        const uint8_t key[32],
                                        const uint8_t iv[32])
{
    if (key == NULL || iv == NULL) {
        throw std::runtime_error("AES key or IV is null");
    }
    if ((data.size() & 15U) != 0) {
        throw std::runtime_error("AES-IGE input length is not block aligned");
    }

    std::vector<uint8_t> result(data);
    aesIgeDecrypt(key, iv, result.empty() ? NULL : &result[0], result.size());
    return result;
}

ServerDhInner parseServerDhParams(const uint8_t* encryptedAnswer,
                                  size_t len,
                                  const uint8_t nonce[16],
                                  const uint8_t serverNonce[16],
                                  const uint8_t newNonce[32])
{
    if (encryptedAnswer == NULL || nonce == NULL ||
        serverNonce == NULL || newNonce == NULL) {
        throw std::runtime_error("Server DH answer has a null input");
    }
    if (len == 0 || (len & 15U) != 0) {
        throw std::runtime_error("Encrypted server DH answer has invalid length");
    }

    uint8_t key[32];
    uint8_t iv[32];
    deriveTmpAes(newNonce, serverNonce, key, iv);

    const std::vector<uint8_t> encrypted(encryptedAnswer,
                                         encryptedAnswer + len);
    const std::vector<uint8_t> plain =
        aesIgeDecryptBytes(encrypted, key, iv);
    if (plain.size() < 24) {
        throw std::runtime_error("Decrypted server DH answer is too short");
    }

    std::vector<uint8_t> innerAndPadding(plain.begin() + 20, plain.end());
    size_t offset = 0;
    const uint32_t constructor = readU32(innerAndPadding, offset);
    if (constructor != SERVER_DH_INNER_DATA_ID) {
        throw std::runtime_error(
            "Unexpected server_DH_inner_data constructor");
    }

    if (innerAndPadding.size() - offset < 32) {
        throw std::runtime_error("Server DH nonces are truncated");
    }

    ServerDhInner result;
    result.nonce.assign(innerAndPadding.begin() + offset,
                        innerAndPadding.begin() + offset + 16);
    offset += 16;
    result.serverNonce.assign(innerAndPadding.begin() + offset,
                              innerAndPadding.begin() + offset + 16);
    offset += 16;

    result.g = readU32(innerAndPadding, offset);
    result.dhPrime = readTlBytes(innerAndPadding, offset);
    result.gA = readTlBytes(innerAndPadding, offset);
    result.serverTime = readU32(innerAndPadding, offset);

    if (offset > innerAndPadding.size() ||
        innerAndPadding.size() - offset > 15) {
        throw std::runtime_error("Invalid server DH random padding length");
    }

    const std::vector<uint8_t> digest =
        sha1Internal(&innerAndPadding[0], offset);
    if (!equalsBytes(&plain[0], &digest[0], 20)) {
        throw std::runtime_error("Server DH inner-data SHA-1 mismatch");
    }
    if (!equalsBytes(&result.nonce[0], nonce, 16)) {
        throw std::runtime_error("Server DH nonce mismatch");
    }
    if (!equalsBytes(&result.serverNonce[0], serverNonce, 16)) {
        throw std::runtime_error("Server DH server_nonce mismatch");
    }

    return result;
}

ServerDhInner parseServerDhParams(const uint8_t* encryptedAnswer,
                                  size_t len,
                                  const std::vector<uint8_t>& nonce,
                                  const std::vector<uint8_t>& serverNonce,
                                  const std::vector<uint8_t>& newNonce)
{
    if (nonce.size() != 16 || serverNonce.size() != 16 ||
        newNonce.size() != 32) {
        throw std::runtime_error("Server DH nonces have invalid lengths");
    }

    return parseServerDhParams(encryptedAnswer, len, &nonce[0],
                               &serverNonce[0], &newNonce[0]);
}

bool checkDhParams(const ServerDhInner& inner)
{
    if (inner.dhPrime.empty() || inner.gA.empty()) {
        return false;
    }
    if (inner.g < 2 || inner.g > 7) {
        return false;
    }

    Mpi p;
    Mpi q;
    Mpi ga;
    Mpi one;
    Mpi pMinusOne;

    if (mbedtls_mpi_read_binary(p.get(), &inner.dhPrime[0],
                                inner.dhPrime.size()) != 0 ||
        mbedtls_mpi_read_binary(ga.get(), &inner.gA[0],
                                inner.gA.size()) != 0) {
        return false;
    }
    if (mbedtls_mpi_bitlen(p.get()) != 2048U ||
        mbedtls_mpi_cmp_int(p.get(), 3) <= 0) {
        return false;
    }
    if (mbedtls_mpi_lset(one.get(), 1) != 0 ||
        mbedtls_mpi_sub_int(pMinusOne.get(), p.get(), 1) != 0 ||
        mbedtls_mpi_copy(q.get(), p.get()) != 0 ||
        mbedtls_mpi_shift_r(q.get(), 1) != 0) {
        return false;
    }
    if (mbedtls_mpi_cmp_mpi(ga.get(), one.get()) <= 0 ||
        mbedtls_mpi_cmp_mpi(ga.get(), pMinusOne.get()) >= 0) {
        return false;
    }

    // Telegram's well-known 2048-bit safe prime (TDLib DhCache): skip the primality tests, which take
    // ~20 s on a Snapdragon S4. Any other prime is tested once and then remembered.
    static const uint8_t kKnownPrime[256] = {
0xc7, 0x1c, 0xae, 0xb9, 0xc6, 0xb1, 0xc9, 0x04, 0x8e, 0x6c, 0x52, 0x2f, 0x70, 0xf1, 0x3f, 0x73,
0x98, 0x0d, 0x40, 0x23, 0x8e, 0x3e, 0x21, 0xc1, 0x49, 0x34, 0xd0, 0x37, 0x56, 0x3d, 0x93, 0x0f,
0x48, 0x19, 0x8a, 0x0a, 0xa7, 0xc1, 0x40, 0x58, 0x22, 0x94, 0x93, 0xd2, 0x25, 0x30, 0xf4, 0xdb,
0xfa, 0x33, 0x6f, 0x6e, 0x0a, 0xc9, 0x25, 0x13, 0x95, 0x43, 0xae, 0xd4, 0x4c, 0xce, 0x7c, 0x37,
0x20, 0xfd, 0x51, 0xf6, 0x94, 0x58, 0x70, 0x5a, 0xc6, 0x8c, 0xd4, 0xfe, 0x6b, 0x6b, 0x13, 0xab,
0xdc, 0x97, 0x46, 0x51, 0x29, 0x69, 0x32, 0x84, 0x54, 0xf1, 0x8f, 0xaf, 0x8c, 0x59, 0x5f, 0x64,
0x24, 0x77, 0xfe, 0x96, 0xbb, 0x2a, 0x94, 0x1d, 0x5b, 0xcd, 0x1d, 0x4a, 0xc8, 0xcc, 0x49, 0x88,
0x07, 0x08, 0xfa, 0x9b, 0x37, 0x8e, 0x3c, 0x4f, 0x3a, 0x90, 0x60, 0xbe, 0xe6, 0x7c, 0xf9, 0xa4,
0xa4, 0xa6, 0x95, 0x81, 0x10, 0x51, 0x90, 0x7e, 0x16, 0x27, 0x53, 0xb5, 0x6b, 0x0f, 0x6b, 0x41,
0x0d, 0xba, 0x74, 0xd8, 0xa8, 0x4b, 0x2a, 0x14, 0xb3, 0x14, 0x4e, 0x0e, 0xf1, 0x28, 0x47, 0x54,
0xfd, 0x17, 0xed, 0x95, 0x0d, 0x59, 0x65, 0xb4, 0xb9, 0xdd, 0x46, 0x58, 0x2d, 0xb1, 0x17, 0x8d,
0x16, 0x9c, 0x6b, 0xc4, 0x65, 0xb0, 0xd6, 0xff, 0x9c, 0xa3, 0x92, 0x8f, 0xef, 0x5b, 0x9a, 0xe4,
0xe4, 0x18, 0xfc, 0x15, 0xe8, 0x3e, 0xbe, 0xa0, 0xf8, 0x7f, 0xa9, 0xff, 0x5e, 0xed, 0x70, 0x05,
0x0d, 0xed, 0x28, 0x49, 0xf4, 0x7b, 0xf9, 0x59, 0xd9, 0x56, 0x85, 0x0c, 0xe9, 0x29, 0x85, 0x1f,
0x0d, 0x81, 0x15, 0xf6, 0x35, 0xb1, 0x05, 0xee, 0x2e, 0x4e, 0x15, 0xd0, 0x4b, 0x24, 0x54, 0xbf,
0x6f, 0x4f, 0xad, 0xf0, 0x34, 0xb1, 0x04, 0x03, 0x11, 0x9c, 0xd8, 0xe3, 0xb9, 0x2f, 0xcc, 0x5b
    };
    static std::vector<uint8_t> verifiedPrime;
    if (inner.dhPrime.size() == 256 &&
        (std::memcmp(&inner.dhPrime[0], kKnownPrime, 256) == 0 || inner.dhPrime == verifiedPrime)) {
        return true;
    }

    uint32_t rngState = 0x9e3779b9U;
    if (isPrime(p.get(), deterministicPrimeRng, &rngState) != 0) {
        return false;
    }
    rngState ^= 0xa5a5a5a5U;
    if (isPrime(q.get(), deterministicPrimeRng, &rngState) != 0) {
        return false;
    }
    verifiedPrime = inner.dhPrime;
    return true;
}

std::vector<uint8_t> computeClientDh(const ServerDhInner& inner,
                                     const uint8_t randomB[256],
                                     uint8_t authKey[256],
                                     uint64_t& authKeyId)
{
    if (randomB == NULL || authKey == NULL) {
        throw std::runtime_error("Client DH input is null");
    }
    if (inner.dhPrime.empty() || inner.gA.empty()) {
        throw std::runtime_error("Client DH group is incomplete");
    }

    Mpi p;
    Mpi g;
    Mpi ga;
    Mpi b;
    Mpi gb;
    Mpi auth;

    readMpi(p, inner.dhPrime, "Invalid DH prime");
    readMpi(ga, inner.gA, "Invalid DH g_a");
    requireMpi(mbedtls_mpi_lset(
                   g.get(), static_cast<mbedtls_mpi_sint>(inner.g)),
               "Invalid DH generator");
    requireMpi(mbedtls_mpi_read_binary(b.get(), randomB, 256),
               "Invalid client DH exponent");

    requireMpi(mbedtls_mpi_exp_mod(
                   gb.get(), g.get(), b.get(), p.get(), NULL),
               "Could not compute g_b");
    requireMpi(mbedtls_mpi_exp_mod(
                   auth.get(), ga.get(), b.get(), p.get(), NULL),
               "Could not compute auth_key");

    const std::vector<uint8_t> gbBytes = writeMpiPadded256(gb.get());
    const std::vector<uint8_t> authBytes = writeMpiPadded256(auth.get());
    std::memcpy(authKey, &authBytes[0], 256);
    authKeyId = deriveAuthKeyId(authKey);
    return gbBytes;
}

uint64_t deriveAuthKeyId(const uint8_t authKey[256])
{
    if (authKey == NULL) {
        throw std::runtime_error("Auth key is null");
    }

    const std::vector<uint8_t> digest = sha1Internal(authKey, 256);
    uint64_t result = 0;
    unsigned int i;
    for (i = 0; i < 8; ++i) {
        result |= static_cast<uint64_t>(digest[12 + i]) << (i * 8);
    }
    return result;
}

std::vector<uint8_t> deriveServerSalt(const uint8_t newNonce[32],
                                      const uint8_t serverNonce[16])
{
    if (newNonce == NULL || serverNonce == NULL) {
        throw std::runtime_error("Server salt input is null");
    }

    std::vector<uint8_t> salt(8);
    size_t i;
    for (i = 0; i < salt.size(); ++i) {
        salt[i] = static_cast<uint8_t>(newNonce[i] ^ serverNonce[i]);
    }
    return salt;
}

std::vector<uint8_t> buildClientDhInnerEncrypted(
    const uint8_t nonce[16],
    const uint8_t serverNonce[16],
    const uint8_t newNonce[32],
    const std::vector<uint8_t>& gB,
    const uint8_t* randomPadding,
    size_t randomPaddingLength)
{
    if (newNonce == NULL) {
        throw std::runtime_error("Client DH new_nonce is null");
    }

    std::vector<uint8_t> inner =
        serializeClientDhInner(nonce, serverNonce, gB);
    const std::vector<uint8_t> digest =
        sha1Internal(&inner[0], inner.size());

    std::vector<uint8_t> plain;
    plain.reserve(20 + inner.size() + 15);
    plain.insert(plain.end(), digest.begin(), digest.end());
    plain.insert(plain.end(), inner.begin(), inner.end());

    const size_t paddingLength = (16 - (plain.size() & 15U)) & 15U;
    if (randomPaddingLength < paddingLength ||
        (paddingLength != 0 && randomPadding == NULL)) {
        throw std::runtime_error("Not enough client DH random padding");
    }
    if (paddingLength != 0) {
        plain.insert(plain.end(), randomPadding,
                     randomPadding + paddingLength);
    }

    uint8_t key[32];
    uint8_t iv[32];
    deriveTmpAes(newNonce, serverNonce, key, iv);
    return aesIgeEncryptBytes(plain, key, iv);
}

std::vector<uint8_t> buildClientDhInnerEncrypted(
    const ServerDhInner& inner,
    const uint8_t newNonce[32],
    const std::vector<uint8_t>& gB,
    const uint8_t* randomPadding,
    size_t randomPaddingLength)
{
    if (inner.nonce.size() != 16 || inner.serverNonce.size() != 16) {
        throw std::runtime_error(
            "Client DH inner nonces have invalid lengths");
    }

    return buildClientDhInnerEncrypted(&inner.nonce[0],
                                       &inner.serverNonce[0],
                                       newNonce,
                                       gB,
                                       randomPadding,
                                       randomPaddingLength);
}

std::vector<uint8_t> computeDhGenNewNonceHash1(const uint8_t newNonce[32],
                                                const uint8_t authKey[256])
{
    if (newNonce == NULL || authKey == NULL) {
        throw std::runtime_error("dh_gen_ok hash input is null");
    }

    const std::vector<uint8_t> authHash = sha1Internal(authKey, 256);
    std::vector<uint8_t> data;
    data.insert(data.end(), newNonce, newNonce + 32);
    data.push_back(1);
    data.insert(data.end(), authHash.begin(), authHash.begin() + 8);

    const std::vector<uint8_t> digest =
        sha1Internal(&data[0], data.size());
    return std::vector<uint8_t>(digest.begin() + 4, digest.begin() + 20);
}

void checkDhGenOk(const uint8_t newNonce[32],
                  const uint8_t authKey[256],
                  const uint8_t serverNewNonceHash1[16])
{
    if (serverNewNonceHash1 == NULL) {
        throw std::runtime_error("Server new_nonce_hash1 is null");
    }

    const std::vector<uint8_t> expected =
        computeDhGenNewNonceHash1(newNonce, authKey);
    if (!equalsBytes(&expected[0], serverNewNonceHash1, 16)) {
        throw std::runtime_error("dh_gen_ok new_nonce_hash1 mismatch");
    }
}

} // namespace tg
