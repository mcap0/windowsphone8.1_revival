#include "mtproto_auth.h"

#include "crypto.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace tg {
namespace {

const uint32_t kReqPqMultiConstructor = 0xbe7e8ef1U;
const uint32_t kResPQConstructor = 0x05162463U;
const uint32_t kPqInnerDataDcConstructor = 0xa9f55f95U;
const uint32_t kVectorConstructor = 0x1cb5c415U;

uint32_t readUint32LE(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t readUint64LE(const uint8_t* data)
{
    uint64_t value = 0;
    for (size_t i = 0; i != 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

void appendUint32LE(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

void appendUint64LE(std::vector<uint8_t>& out, uint64_t value)
{
    for (size_t i = 0; i != 8; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

void appendTlBytes(std::vector<uint8_t>& out,
                   const uint8_t* data,
                   size_t len)
{
    if (len != 0 && data == NULL) {
        throw std::invalid_argument("TL bytes data is null");
    }
    if (len > 0x00ffffffU) {
        throw std::length_error("TL bytes are too long");
    }

    size_t headerLength;
    if (len <= 253) {
        out.push_back(static_cast<uint8_t>(len));
        headerLength = 1;
    } else {
        out.push_back(254);
        out.push_back(static_cast<uint8_t>(len));
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(len >> 16));
        headerLength = 4;
    }

    if (len != 0) {
        out.insert(out.end(), data, data + len);
    }

    const size_t padding = (4 - ((headerLength + len) & 3)) & 3;
    for (size_t i = 0; i != padding; ++i) {
        out.push_back(0);
    }
}

struct ParsedTlBytes {
    std::vector<uint8_t> value;
    size_t next;
};

ParsedTlBytes readTlBytes(const uint8_t* data, size_t len, size_t offset)
{
    if (offset >= len) {
        throw std::runtime_error("Truncated TL bytes");
    }

    size_t headerLength = 1;
    size_t valueLength;
    const uint8_t first = data[offset];

    if (first < 254) {
        valueLength = first;
    } else if (first == 254) {
        if (len - offset < 4) {
            throw std::runtime_error("Truncated TL bytes length");
        }
        headerLength = 4;
        valueLength = static_cast<size_t>(data[offset + 1]) |
            (static_cast<size_t>(data[offset + 2]) << 8) |
            (static_cast<size_t>(data[offset + 3]) << 16);
        if (valueLength <= 253) {
            throw std::runtime_error("Non-canonical TL bytes length");
        }
    } else {
        throw std::runtime_error("Invalid TL bytes length marker");
    }

    if (valueLength > len - offset - headerLength) {
        throw std::runtime_error("Truncated TL bytes value");
    }

    const size_t padding = (4 - ((headerLength + valueLength) & 3)) & 3;
    const size_t consumed = headerLength + valueLength + padding;
    if (consumed > len - offset) {
        throw std::runtime_error("Truncated TL bytes padding");
    }

    ParsedTlBytes result;
    if (valueLength != 0) {
        result.value.assign(data + offset + headerLength,
                            data + offset + headerLength + valueLength);
    }

    const size_t paddingOffset = offset + headerLength + valueLength;
    for (size_t i = 0; i != padding; ++i) {
        if (data[paddingOffset + i] != 0) {
            throw std::runtime_error("Non-zero TL bytes padding");
        }
    }

    result.next = offset + consumed;
    return result;
}

uint32_t rotateLeft32(uint32_t value, unsigned count)
{
    return (value << count) | (value >> (32 - count));
}

std::vector<uint8_t> sha1Internal(const uint8_t* data, size_t len)
{
    if (len != 0 && data == NULL) {
        throw std::invalid_argument("SHA-1 input is null");
    }
    if (len > (std::numeric_limits<uint64_t>::max)() / 8U) {
        throw std::length_error("SHA-1 input is too long");
    }

    std::vector<uint8_t> message;
    if (len != 0) {
        message.assign(data, data + len);
    }

    const uint64_t bitLength = static_cast<uint64_t>(len) * 8U;
    message.push_back(0x80);
    while ((message.size() & 63U) != 56U) {
        message.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<uint8_t>(bitLength >> (i * 8)));
    }

    uint32_t h0 = 0x67452301U;
    uint32_t h1 = 0xefcdab89U;
    uint32_t h2 = 0x98badcfeU;
    uint32_t h3 = 0x10325476U;
    uint32_t h4 = 0xc3d2e1f0U;

    for (size_t block = 0; block < message.size(); block += 64) {
        uint32_t w[80];
        for (size_t i = 0; i != 16; ++i) {
            const size_t p = block + i * 4;
            w[i] = (static_cast<uint32_t>(message[p]) << 24) |
                (static_cast<uint32_t>(message[p + 1]) << 16) |
                (static_cast<uint32_t>(message[p + 2]) << 8) |
                static_cast<uint32_t>(message[p + 3]);
        }
        for (size_t i = 16; i != 80; ++i) {
            w[i] = rotateLeft32(
                w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (size_t i = 0; i != 80; ++i) {
            uint32_t f;
            uint32_t k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5a827999U;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ed9eba1U;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8f1bbcdcU;
            } else {
                f = b ^ c ^ d;
                k = 0xca62c1d6U;
            }
            const uint32_t temp =
                rotateLeft32(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotateLeft32(b, 30);
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
    for (size_t i = 0; i != 5; ++i) {
        digest[i * 4] = static_cast<uint8_t>(words[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(words[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(words[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(words[i]);
    }
    return digest;
}

uint32_t rotateRight32(uint32_t value, unsigned count)
{
    return (value >> count) | (value << (32 - count));
}

std::vector<uint8_t> sha256Internal(const uint8_t* data, size_t len)
{
    static const uint32_t k[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };

    if (len != 0 && data == NULL) {
        throw std::invalid_argument("SHA-256 input is null");
    }
    if (len > (std::numeric_limits<uint64_t>::max)() / 8U) {
        throw std::length_error("SHA-256 input is too long");
    }

    std::vector<uint8_t> message;
    if (len != 0) {
        message.assign(data, data + len);
    }

    const uint64_t bitLength = static_cast<uint64_t>(len) * 8U;
    message.push_back(0x80);
    while ((message.size() & 63U) != 56U) {
        message.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        message.push_back(static_cast<uint8_t>(bitLength >> (i * 8)));
    }

    uint32_t h[8] = {
        0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
        0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U
    };

    for (size_t block = 0; block < message.size(); block += 64) {
        uint32_t w[64];
        for (size_t i = 0; i != 16; ++i) {
            const size_t p = block + i * 4;
            w[i] = (static_cast<uint32_t>(message[p]) << 24) |
                (static_cast<uint32_t>(message[p + 1]) << 16) |
                (static_cast<uint32_t>(message[p + 2]) << 8) |
                static_cast<uint32_t>(message[p + 3]);
        }

        for (size_t i = 16; i != 64; ++i) {
            const uint32_t s0 = rotateRight32(w[i - 15], 7) ^
                rotateRight32(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotateRight32(w[i - 2], 17) ^
                rotateRight32(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = h[0];
        uint32_t b = h[1];
        uint32_t c = h[2];
        uint32_t d = h[3];
        uint32_t e = h[4];
        uint32_t f = h[5];
        uint32_t g = h[6];
        uint32_t hh = h[7];

        for (size_t i = 0; i != 64; ++i) {
            const uint32_t s1 = rotateRight32(e, 6) ^
                rotateRight32(e, 11) ^ rotateRight32(e, 25);
            const uint32_t choice = (e & f) ^ ((~e) & g);
            const uint32_t temp1 = hh + s1 + choice + k[i] + w[i];
            const uint32_t s0 = rotateRight32(a, 2) ^
                rotateRight32(a, 13) ^ rotateRight32(a, 22);
            const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t temp2 = s0 + majority;

            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::vector<uint8_t> digest(32);
    for (size_t i = 0; i != 8; ++i) {
        digest[i * 4] = static_cast<uint8_t>(h[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8_t>(h[i]);
    }
    return digest;
}

const size_t kRsaBlockSize = 256;
const size_t kRsaHashSize = 32;
const size_t kRsaPayloadSize = kRsaBlockSize - kRsaHashSize;
const size_t kRsaPaddedDataSize = 192;
const size_t kRsaDataHashSize = 32;
const size_t kBigIntLimbs = kRsaBlockSize / 4;

struct BigInt {
    uint32_t limb[kBigIntLimbs];

    BigInt()
    {
        std::memset(limb, 0, sizeof(limb));
    }
};

int compareBigInt(const BigInt& a, const BigInt& b)
{
    for (size_t i = kBigIntLimbs; i != 0; --i) {
        if (a.limb[i - 1] < b.limb[i - 1]) {
            return -1;
        }
        if (a.limb[i - 1] > b.limb[i - 1]) {
            return 1;
        }
    }
    return 0;
}

BigInt subtractBigInt(const BigInt& a, const BigInt& b)
{
    BigInt result;
    uint64_t borrow = 0;
    for (size_t i = 0; i != kBigIntLimbs; ++i) {
        const uint64_t av = a.limb[i];
        const uint64_t bv = static_cast<uint64_t>(b.limb[i]) + borrow;
        result.limb[i] = static_cast<uint32_t>(av - bv);
        borrow = av < bv ? 1U : 0U;
    }
    return result;
}

BigInt addBigInt(const BigInt& a, const BigInt& b)
{
    BigInt result;
    uint64_t carry = 0;
    for (size_t i = 0; i != kBigIntLimbs; ++i) {
        const uint64_t sum = static_cast<uint64_t>(a.limb[i]) +
            static_cast<uint64_t>(b.limb[i]) + carry;
        result.limb[i] = static_cast<uint32_t>(sum);
        carry = sum >> 32;
    }
    return result;
}

BigInt addModulo(const BigInt& a, const BigInt& b, const BigInt& modulus)
{
    const BigInt modulusMinusB = subtractBigInt(modulus, b);
    if (compareBigInt(a, modulusMinusB) >= 0) {
        return subtractBigInt(a, modulusMinusB);
    }
    return addBigInt(a, b);
}

BigInt doubleModulo(const BigInt& value, const BigInt& modulus)
{
    return addModulo(value, value, modulus);
}

bool getBigIntBit(const BigInt& value, size_t bit)
{
    return ((value.limb[bit / 32] >> (bit & 31)) & 1U) != 0;
}

BigInt multiplyModulo(const BigInt& a,
                      const BigInt& b,
                      const BigInt& modulus)
{
    BigInt result;
    BigInt current = b;
    for (size_t bit = 0; bit != kRsaBlockSize * 8; ++bit) {
        if (getBigIntBit(a, bit)) {
            result = addModulo(result, current, modulus);
        }
        if (bit + 1 != kRsaBlockSize * 8) {
            current = doubleModulo(current, modulus);
        }
    }
    return result;
}

BigInt reduceBytesModulo(const uint8_t* bytes,
                         size_t len,
                         const BigInt& modulus)
{
    BigInt result;
    BigInt one;
    one.limb[0] = 1;

    for (size_t i = 0; i != len; ++i) {
        for (int bit = 7; bit >= 0; --bit) {
            result = doubleModulo(result, modulus);
            if ((bytes[i] & (1U << bit)) != 0) {
                result = addModulo(result, one, modulus);
            }
        }
    }
    return result;
}

BigInt exponentiateModulo(const BigInt& base,
                          uint32_t exponent,
                          const BigInt& modulus)
{
    BigInt result;
    BigInt one;
    one.limb[0] = 1;
    if (compareBigInt(modulus, one) != 0) {
        result = one;
    }

    BigInt power = base;
    while (exponent != 0) {
        if ((exponent & 1U) != 0) {
            result = multiplyModulo(result, power, modulus);
        }
        exponent >>= 1;
        if (exponent != 0) {
            power = multiplyModulo(power, power, modulus);
        }
    }
    return result;
}

BigInt bigIntFromBytes(const uint8_t* bytes, size_t len)
{
    BigInt result;
    for (size_t i = 0; i != len; ++i) {
        const size_t reverseIndex = len - 1 - i;
        result.limb[i / 4] |=
            static_cast<uint32_t>(bytes[reverseIndex]) << ((i & 3) * 8);
    }
    return result;
}

void bigintToBytes(const BigInt& value, uint8_t* output, size_t outputLen)
{
    for (size_t i = 0; i != outputLen; ++i) {
        const size_t reverseIndex = outputLen - 1 - i;
        output[reverseIndex] = static_cast<uint8_t>(
            value.limb[i / 4] >> ((i & 3) * 8));
    }
}

} // namespace

std::vector<uint8_t> makeRsaPadBlock(
    const uint8_t paddedData[192],
    const uint8_t aesKey[32])
{
    if (paddedData == NULL || aesKey == NULL) {
        throw std::invalid_argument("RSA_PAD data or AES key is null");
    }

    std::vector<uint8_t> keyAndData;
    keyAndData.reserve(32 + kRsaPaddedDataSize);
    keyAndData.insert(keyAndData.end(), aesKey, aesKey + 32);
    keyAndData.insert(
        keyAndData.end(), paddedData, paddedData + kRsaPaddedDataSize);

    const std::vector<uint8_t> dataHash =
        sha256Internal(&keyAndData[0], keyAndData.size());

    std::vector<uint8_t> dataWithHash;
    dataWithHash.reserve(kRsaPaddedDataSize + kRsaDataHashSize);
    dataWithHash.insert(
        dataWithHash.end(), paddedData, paddedData + kRsaPaddedDataSize);
    dataWithHash.insert(dataWithHash.end(), dataHash.begin(), dataHash.end());

    std::reverse(dataWithHash.begin(),
                 dataWithHash.begin() + kRsaPaddedDataSize);

    uint8_t aesIv[32] = {};
    aesIgeEncrypt(aesKey, aesIv, &dataWithHash[0], dataWithHash.size());

    const std::vector<uint8_t> encryptedHash =
        sha256Internal(&dataWithHash[0], dataWithHash.size());

    std::vector<uint8_t> block(kRsaBlockSize);
    for (size_t i = 0; i != kRsaHashSize; ++i) {
        block[i] = static_cast<uint8_t>(aesKey[i] ^ encryptedHash[i]);
    }
    std::memcpy(&block[kRsaHashSize],
                &dataWithHash[0],
                dataWithHash.size());
    return block;
}

std::vector<uint8_t> buildReqPqMulti(const uint8_t nonce[16])
{
    if (nonce == NULL) {
        throw std::invalid_argument("nonce is null");
    }

    std::vector<uint8_t> result;
    result.reserve(20);
    appendUint32LE(result, kReqPqMultiConstructor);
    result.insert(result.end(), nonce, nonce + 16);
    return result;
}

ResPQ parseResPQ(const uint8_t* data, size_t len)
{
    if (data == NULL) {
        throw std::invalid_argument("ResPQ data is null");
    }
    if (len < 4) {
        throw std::runtime_error("Truncated ResPQ constructor");
    }
    if (readUint32LE(data) != kResPQConstructor) {
        throw std::runtime_error("Wrong ResPQ constructor");
    }

    size_t offset = 4;
    if (len - offset < 32) {
        throw std::runtime_error("Truncated ResPQ nonces");
    }

    ResPQ result;
    std::memcpy(result.nonce, data + offset, 16);
    offset += 16;
    std::memcpy(result.serverNonce, data + offset, 16);
    offset += 16;

    ParsedTlBytes pq = readTlBytes(data, len, offset);
    result.pq.swap(pq.value);
    offset = pq.next;

    if (len - offset < 8) {
        throw std::runtime_error("Truncated ResPQ fingerprint vector");
    }
    if (readUint32LE(data + offset) != kVectorConstructor) {
        throw std::runtime_error("Wrong ResPQ fingerprint vector constructor");
    }
    offset += 4;

    const uint32_t count = readUint32LE(data + offset);
    offset += 4;
    if (count > (len - offset) / 8) {
        throw std::runtime_error("Truncated ResPQ fingerprints");
    }

    result.fingerprints.reserve(count);
    for (uint32_t i = 0; i != count; ++i) {
        result.fingerprints.push_back(readUint64LE(data + offset));
        offset += 8;
    }
    return result;
}

std::vector<uint8_t> buildPqInnerDataDc(
    const std::vector<uint8_t>& pq,
    const std::vector<uint8_t>& p,
    const std::vector<uint8_t>& q,
    const uint8_t nonce[16],
    const uint8_t serverNonce[16],
    const uint8_t newNonce[32],
    int32_t dc)
{
    if (nonce == NULL || serverNonce == NULL || newNonce == NULL) {
        throw std::invalid_argument("PqInnerDataDc nonce is null");
    }

    std::vector<uint8_t> result;
    appendUint32LE(result, kPqInnerDataDcConstructor);
    appendTlBytes(result, pq.empty() ? NULL : &pq[0], pq.size());
    appendTlBytes(result, p.empty() ? NULL : &p[0], p.size());
    appendTlBytes(result, q.empty() ? NULL : &q[0], q.size());
    result.insert(result.end(), nonce, nonce + 16);
    result.insert(result.end(), serverNonce, serverNonce + 16);
    result.insert(result.end(), newNonce, newNonce + 32);
    appendUint32LE(result, static_cast<uint32_t>(dc));
    return result;
}

uint64_t rsaFingerprint(const uint8_t* n, size_t nLen, uint32_t e)
{
    if (nLen != 0 && n == NULL) {
        throw std::invalid_argument("RSA modulus is null");
    }

    size_t exponentLength = 1;
    if (e > 0x00ffffffU) {
        exponentLength = 4;
    } else if (e > 0x0000ffffU) {
        exponentLength = 3;
    } else if (e > 0x000000ffU) {
        exponentLength = 2;
    }

    uint8_t exponentBytes[4];
    for (size_t i = 0; i != exponentLength; ++i) {
        exponentBytes[exponentLength - 1 - i] =
            static_cast<uint8_t>(e >> (i * 8));
    }

    std::vector<uint8_t> serialized;
    appendTlBytes(serialized, n, nLen);
    appendTlBytes(serialized, exponentBytes, exponentLength);

    const std::vector<uint8_t> digest = sha1Internal(
        serialized.empty() ? NULL : &serialized[0], serialized.size());
    return readUint64LE(&digest[12]);
}

std::vector<uint8_t> rsaPadAndEncrypt(
    const uint8_t* data,
    size_t len,
    const uint8_t* n,
    size_t nLen,
    uint32_t e,
    const uint8_t* randomBytes,
    size_t randomLen)
{
    if (len != 0 && data == NULL) {
        throw std::invalid_argument("RSA input data is null");
    }
    if (n == NULL || nLen == 0 || nLen > kRsaBlockSize) {
        throw std::invalid_argument("Invalid RSA modulus");
    }
    if (e == 0) {
        throw std::invalid_argument("RSA exponent must be nonzero");
    }
    if (len > 144) {
        throw std::length_error("RSA input is too long");
    }
    if (randomLen != 0 && randomBytes == NULL) {
        throw std::invalid_argument("RSA random bytes are null");
    }

    const size_t paddingLength = kRsaPaddedDataSize - len;
    const size_t requiredRandomLength = 32 + paddingLength;
    if (randomLen < requiredRandomLength) {
        throw std::runtime_error("Insufficient RSA random bytes");
    }

    BigInt modulus = bigIntFromBytes(n, nLen);
    BigInt zero;
    if (compareBigInt(modulus, zero) == 0) {
        throw std::invalid_argument("RSA modulus is zero");
    }

    std::vector<uint8_t> paddedData(kRsaPaddedDataSize, 0);
    if (len != 0) {
        std::memcpy(&paddedData[0], data, len);
    }
    if (paddingLength != 0) {
        std::memcpy(&paddedData[len],
                    randomBytes + 32,
                    paddingLength);
    }

    size_t keyOffset = 0;
    for (;;) {
        if (keyOffset > randomLen || randomLen - keyOffset < 32) {
            throw std::runtime_error(
                "Insufficient RSA random bytes for AES key retry");
        }

        const std::vector<uint8_t> block =
            makeRsaPadBlock(&paddedData[0], randomBytes + keyOffset);
        const BigInt plaintext = bigIntFromBytes(&block[0], block.size());

        if (compareBigInt(plaintext, modulus) < 0) {
            const BigInt encrypted =
                exponentiateModulo(plaintext, e, modulus);
            std::vector<uint8_t> result(kRsaBlockSize);
            bigintToBytes(encrypted, &result[0], result.size());
            return result;
        }

        keyOffset += 32;
    }
}

} // namespace tg
