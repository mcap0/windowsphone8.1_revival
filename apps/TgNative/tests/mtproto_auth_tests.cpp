#include "../core/crypto.h"
#include "../core/mtproto_auth.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {

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
                   const uint8_t* value,
                   size_t len)
{
    const size_t headerLength = len <= 253 ? 1 : 4;
    if (len <= 253) {
        out.push_back(static_cast<uint8_t>(len));
    } else {
        out.push_back(254);
        out.push_back(static_cast<uint8_t>(len));
        out.push_back(static_cast<uint8_t>(len >> 8));
        out.push_back(static_cast<uint8_t>(len >> 16));
    }

    if (len != 0) {
        out.insert(out.end(), value, value + len);
    }

    const size_t padding = (4 - ((headerLength + len) & 3)) & 3;
    for (size_t i = 0; i != padding; ++i) {
        out.push_back(0);
    }
}

uint32_t readUint32LE(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

std::vector<uint8_t> makeResPQ()
{
    std::vector<uint8_t> wire;
    appendUint32LE(wire, 0x05162463U);

    for (size_t i = 0; i != 16; ++i) {
        wire.push_back(static_cast<uint8_t>(0x10 + i));
    }
    for (size_t i = 0; i != 16; ++i) {
        wire.push_back(static_cast<uint8_t>(0x40 + i));
    }

    const uint8_t pq[] = { 0x17, 0x23, 0x42 };
    appendTlBytes(wire, pq, sizeof(pq));
    appendUint32LE(wire, 0x1cb5c415U);
    appendUint32LE(wire, 2);
    appendUint64LE(wire, 0x1122334455667788ULL);
    appendUint64LE(wire, 0x8877665544332211ULL);
    return wire;
}

void testConstructorIds()
{
    uint8_t nonce[16] = {};
    const std::vector<uint8_t> request = tg::buildReqPqMulti(nonce);
    if (request.size() < 4 ||
        readUint32LE(&request[0]) != 0xbe7e8ef1U) {
        throw std::runtime_error("req_pq_multi constructor id mismatch");
    }

    const std::vector<uint8_t> resPQ = makeResPQ();
    if (readUint32LE(&resPQ[0]) != 0x05162463U) {
        throw std::runtime_error("resPQ literal constructor id mismatch");
    }

    const std::vector<uint8_t> inner = tg::buildPqInnerDataDc(
        std::vector<uint8_t>(), std::vector<uint8_t>(),
        std::vector<uint8_t>(), nonce, nonce, nonce, 1);
    if (readUint32LE(&inner[0]) != 0xa9f55f95U) {
        throw std::runtime_error("p_q_inner_data_dc constructor id mismatch");
    }
}

void testBuildReqPqMulti()
{
    uint8_t nonce[16];
    for (size_t i = 0; i != sizeof(nonce); ++i) {
        nonce[i] = static_cast<uint8_t>(i);
    }

    const std::vector<uint8_t> request = tg::buildReqPqMulti(nonce);
    if (request.size() != 20 ||
        readUint32LE(&request[0]) != 0xbe7e8ef1U ||
        std::memcmp(&request[4], nonce, sizeof(nonce)) != 0) {
        throw std::runtime_error("req_pq_multi layout mismatch");
    }
}

void testParseResPQ()
{
    const std::vector<uint8_t> wire = makeResPQ();
    const tg::ResPQ parsed = tg::parseResPQ(&wire[0], wire.size());

    if (std::memcmp(parsed.nonce, &wire[4], 16) != 0 ||
        std::memcmp(parsed.serverNonce, &wire[20], 16) != 0 ||
        parsed.pq.size() != 3 ||
        parsed.pq[0] != 0x17 ||
        parsed.pq[1] != 0x23 ||
        parsed.pq[2] != 0x42 ||
        parsed.fingerprints.size() != 2 ||
        parsed.fingerprints[0] != 0x1122334455667788ULL ||
        parsed.fingerprints[1] != 0x8877665544332211ULL) {
        throw std::runtime_error("ResPQ round trip mismatch");
    }

    bool rejected = false;
    try {
        tg::parseResPQ(&wire[0], wire.size() - 1);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected) {
        throw std::runtime_error("truncated ResPQ was accepted");
    }
}

void testBuildPqInnerDataDc()
{
    const std::vector<uint8_t> pq(3, 0x11);
    const std::vector<uint8_t> p(2, 0x22);
    const std::vector<uint8_t> q(1, 0x33);
    uint8_t nonce[16] = {};
    uint8_t serverNonce[16] = {};
    uint8_t newNonce[32] = {};

    const std::vector<uint8_t> inner = tg::buildPqInnerDataDc(
        pq, p, q, nonce, serverNonce, newNonce, -2);

    if (inner.size() != 84 ||
        readUint32LE(&inner[0]) != 0xa9f55f95U) {
        throw std::runtime_error("p_q_inner_data_dc layout mismatch");
    }
}

void testRsaFingerprint()
{
    const uint8_t modulus[] = {
        0x00, 0xd3, 0x41, 0x92, 0x77, 0x18
    };
    const uint64_t first =
        tg::rsaFingerprint(modulus, sizeof(modulus), 65537);
    const uint64_t second =
        tg::rsaFingerprint(modulus, sizeof(modulus), 65537);

    static_assert(sizeof(uint64_t) == 8,
                  "RSA fingerprint must be 64 bits");
    if (first != second) {
        throw std::runtime_error("RSA fingerprint is not deterministic");
    }
}

void testRsaPadAndEncrypt()
{
    const uint8_t data[] = { 0x12, 0x34, 0x56, 0x78, 0x9a };
    const size_t paddingLength = 192 - sizeof(data);
    std::vector<uint8_t> randomBytes(32 + paddingLength);
    for (size_t i = 0; i != randomBytes.size(); ++i) {
        randomBytes[i] = static_cast<uint8_t>(i * 13 + 7);
    }

    std::vector<uint8_t> paddedData(192, 0);
    std::memcpy(&paddedData[0], data, sizeof(data));
    std::memcpy(&paddedData[sizeof(data)],
                &randomBytes[32],
                paddingLength);

    const std::vector<uint8_t> block =
        tg::makeRsaPadBlock(&paddedData[0], &randomBytes[0]);
    if (block.size() != 256) {
        throw std::runtime_error("RSA_PAD block size mismatch");
    }

    const std::vector<uint8_t> encryptedHash =
        tg::sha256(&block[32], block.size() - 32);
    for (size_t i = 0; i != 32; ++i) {
        if (static_cast<uint8_t>(block[i] ^ encryptedHash[i]) !=
            randomBytes[i]) {
            throw std::runtime_error("RSA_PAD AES key XOR check failed");
        }
    }

    std::vector<uint8_t> decrypted(
        block.begin() + 32, block.end());
    uint8_t aesIv[32] = {};
    tg::aesIgeDecrypt(&randomBytes[0], aesIv,
                      &decrypted[0], decrypted.size());
    std::reverse(decrypted.begin(), decrypted.begin() + 192);

    if (!std::equal(paddedData.begin(), paddedData.end(),
                    decrypted.begin())) {
        throw std::runtime_error("RSA_PAD padded data decode mismatch");
    }

    std::vector<uint8_t> keyAndData(
        randomBytes.begin(), randomBytes.begin() + 32);
    keyAndData.insert(keyAndData.end(),
                      paddedData.begin(), paddedData.end());
    const std::vector<uint8_t> expectedDataHash =
        tg::sha256(&keyAndData[0], keyAndData.size());
    if (!std::equal(expectedDataHash.begin(), expectedDataHash.end(),
                    decrypted.begin() + 192)) {
        throw std::runtime_error("RSA_PAD data hash decode mismatch");
    }

    std::vector<uint8_t> modulus(256, 0xff);
    const std::vector<uint8_t> encrypted = tg::rsaPadAndEncrypt(
        data, sizeof(data), &modulus[0], modulus.size(), 1,
        &randomBytes[0], randomBytes.size());
    const std::vector<uint8_t> encryptedAgain = tg::rsaPadAndEncrypt(
        data, sizeof(data), &modulus[0], modulus.size(), 1,
        &randomBytes[0], randomBytes.size());

    if (encrypted.size() != 256 || encrypted != encryptedAgain ||
        encrypted != block) {
        throw std::runtime_error(
            "RSA_PAD output size, content, or determinism mismatch");
    }

    std::vector<uint8_t> tooLong(145, 0);
    bool rejected = false;
    try {
        tg::rsaPadAndEncrypt(
            &tooLong[0], tooLong.size(), &modulus[0], modulus.size(),
            1, &randomBytes[0], randomBytes.size());
    } catch (const std::length_error&) {
        rejected = true;
    }
    if (!rejected) {
        throw std::runtime_error("oversized RSA input was accepted");
    }

    rejected = false;
    try {
        tg::rsaPadAndEncrypt(
            data, sizeof(data), &modulus[0], modulus.size(), 1,
            &randomBytes[0], randomBytes.size() - 1);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    if (!rejected) {
        throw std::runtime_error("insufficient RSA randomness was accepted");
    }
}

typedef void (*TestFunction)();

int runTest(TestFunction test)
{
    try {
        test();
        return 0;
    } catch (...) {
        return 1;
    }
}

} // namespace

int runMtprotoAuthTests()
{
    int failures = 0;
    failures += runTest(testConstructorIds);
    failures += runTest(testBuildReqPqMulti);
    failures += runTest(testParseResPQ);
    failures += runTest(testBuildPqInnerDataDc);
    failures += runTest(testRsaFingerprint);
    failures += runTest(testRsaPadAndEncrypt);
    return failures;
}
