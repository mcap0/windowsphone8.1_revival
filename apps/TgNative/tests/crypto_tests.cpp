#include "../core/crypto.h"

extern "C" {
#include "mbedtls/aes.h"
}

#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& detail)
{
    if (!condition) {
        throw std::runtime_error(detail);
    }
}

std::string toHex(const std::vector<uint8_t>& bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 15]);
    }
    return result;
}

void testSha1()
{
    const uint8_t input[] = { 'a', 'b', 'c' };
    const std::vector<uint8_t> actual = tg::sha1(input, sizeof(input));
    require(toHex(actual) == "a9993e364706816aba3e25717850c26c9cd0d89d",
            "SHA-1 abc vector mismatch: " + toHex(actual));
}

void testSha256()
{
    const uint8_t input[] = { 'a', 'b', 'c' };
    const std::vector<uint8_t> actual = tg::sha256(input, sizeof(input));
    require(toHex(actual) ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "SHA-256 abc vector mismatch: " + toHex(actual));
}

std::vector<uint8_t> independentlyEncryptIge(
    const uint8_t key[32], const uint8_t iv[32],
    const std::vector<uint8_t>& plaintext)
{
    require((plaintext.size() & 15u) == 0,
            "independent AES-IGE input length is not block aligned");

    mbedtls_aes_context context;
    mbedtls_aes_init(&context);

    int status = mbedtls_aes_setkey_enc(&context, key, 256);
    if (status != 0) {
        mbedtls_aes_free(&context);
        throw std::runtime_error("independent AES-IGE key setup failed");
    }

    std::vector<uint8_t> ciphertext(plaintext.size());
    uint8_t previousCiphertext[16];
    uint8_t previousPlaintext[16];
    uint8_t blockInput[16];
    uint8_t blockOutput[16];

    // IGE IV convention: y_prev = iv[0..16), x_prev = iv[16..32).
    std::memcpy(previousCiphertext, iv, 16);
    std::memcpy(previousPlaintext, iv + 16, 16);

    for (std::size_t offset = 0; offset < plaintext.size(); offset += 16) {
        // AES input is x_i XOR y_(i-1).
        for (std::size_t i = 0; i < 16; ++i) {
            blockInput[i] = static_cast<uint8_t>(
                plaintext[offset + i] ^ previousCiphertext[i]);
        }

        status = mbedtls_aes_crypt_ecb(
            &context, MBEDTLS_AES_ENCRYPT, blockInput, blockOutput);
        if (status != 0) {
            mbedtls_aes_free(&context);
            throw std::runtime_error("independent AES-IGE block encryption failed");
        }

        // y_i = E(x_i XOR y_(i-1)) XOR x_(i-1).
        for (std::size_t i = 0; i < 16; ++i) {
            ciphertext[offset + i] = static_cast<uint8_t>(
                blockOutput[i] ^ previousPlaintext[i]);
        }

        // Advance both chains using the original plaintext and new ciphertext.
        std::memcpy(previousCiphertext, &ciphertext[offset], 16);
        std::memcpy(previousPlaintext, &plaintext[offset], 16);
    }

    mbedtls_aes_free(&context);
    return ciphertext;
}

void testAesIgeKnownConvention()
{
    uint8_t key[32];
    uint8_t iv[32];
    std::vector<uint8_t> plaintext(64);

    for (std::size_t i = 0; i < sizeof(key); ++i) {
        key[i] = static_cast<uint8_t>(i);
    }
    for (std::size_t i = 0; i < sizeof(iv); ++i) {
        iv[i] = static_cast<uint8_t>(100 + i);
    }
    for (std::size_t i = 0; i < plaintext.size(); ++i) {
        plaintext[i] = static_cast<uint8_t>(i * 3 + 1);
    }

    const std::vector<uint8_t> expected =
        independentlyEncryptIge(key, iv, plaintext);

    std::vector<uint8_t> actual = plaintext;
    tg::aesIgeEncrypt(key, iv, &actual[0], actual.size());
    require(actual == expected,
            "AES-IGE encryption does not match the independent IV convention");

    std::vector<uint8_t> decrypted = expected;
    tg::aesIgeDecrypt(key, iv, &decrypted[0], decrypted.size());
    require(decrypted == plaintext,
            "AES-IGE decryption failed for the known IV convention");
}

void testAesIge()
{
    uint8_t key[32];
    uint8_t iv[32];

    for (std::size_t i = 0; i < sizeof(key); ++i) {
        key[i] = static_cast<uint8_t>(i * 29 + 7);
    }
    for (std::size_t i = 0; i < sizeof(iv); ++i) {
        iv[i] = static_cast<uint8_t>(i * 47 + 13);
    }

    const std::size_t lengths[] = { 16, 32, 64, 4096 };
    for (std::size_t lengthIndex = 0;
         lengthIndex < sizeof(lengths) / sizeof(lengths[0]);
         ++lengthIndex) {
        const std::size_t length = lengths[lengthIndex];
        std::vector<uint8_t> original(length);
        for (std::size_t i = 0; i < length; ++i) {
            original[i] = static_cast<uint8_t>(
                i * 73 + length * 11 + (i >> 1));
        }

        std::vector<uint8_t> encrypted = original;
        tg::aesIgeEncrypt(key, iv, &encrypted[0], encrypted.size());
        require(encrypted != original, "AES-IGE ciphertext equals its input");

        tg::aesIgeDecrypt(key, iv, &encrypted[0], encrypted.size());
        require(encrypted == original,
                "AES-IGE round trip failed for length " +
                    std::to_string(static_cast<unsigned long long>(length)));
    }
}

void checkFactors(uint64_t value, uint64_t expectedP, uint64_t expectedQ)
{
    uint64_t p = 0;
    uint64_t q = 0;
    tg::factorPq(value, p, q);

    require(p == expectedP && q == expectedQ,
            "unexpected factors for " +
                std::to_string(static_cast<unsigned long long>(value)));
    require(p > 1 && p < q, "factors are not ordered primes");
    require(tg::mulMod(p, q, value + 1) == value,
            "factor product does not equal input");
}

void testFactorLarge()
{
    const uint64_t value = UINT64_C(1724114033281923457);
    uint64_t p = 0;
    uint64_t q = 0;
    tg::factorPq(value, p, q);

    require(p > 1 && p < q, "large factors are not ordered");
    require(tg::mulMod(p, q, value + 1) == value,
            "large factor product does not equal input");
}

void testFactorSmall()
{
    checkFactors(15, 3, 5);
    checkFactors(21, 3, 7);
    checkFactors(3233, 53, 61);
}

void testRsaRaw()
{
    const uint8_t modulus[] = { 0x0c, 0xa1 };
    const uint8_t message[] = { 0x41 };
    const uint8_t expected[] = { 0x0a, 0xe6 };

    const std::vector<uint8_t> actual =
        tg::rsaRawEncrypt(modulus, sizeof(modulus), 17, message, sizeof(message));
    require(actual.size() == sizeof(expected) &&
                actual[0] == expected[0] && actual[1] == expected[1],
            "RSA textbook vector mismatch: " + toHex(actual));
}

void testModPow()
{
    require(tg::modPow(2, 10, 1000) == 24, "modPow(2, 10, 1000) mismatch");

    const uint64_t modulus = UINT64_C(1000000007);
    uint64_t slow = 1;
    for (unsigned int i = 0; i < 200; ++i) {
        slow = (slow * 3) % modulus;
    }

    const uint64_t fast = tg::modPow(3, 200, modulus);
    const uint64_t half = tg::modPow(3, 100, modulus);
    require(fast == slow, "modPow disagrees with repeated multiplication");
    require(fast == (half * half) % modulus,
            "modPow violates the exponent multiplication identity");
}

typedef void (*TestFunction)();

void runTest(const char* name, TestFunction test, int& failures)
{
    try {
        test();
        std::cout << "  ok " << name << std::endl;
    } catch (const std::exception& error) {
        ++failures;
        std::cout << "  FAIL " << name << ": " << error.what() << std::endl;
    } catch (...) {
        ++failures;
        std::cout << "  FAIL " << name << ": unknown error" << std::endl;
    }
}

} // namespace

int runCryptoTests()
{
    int failures = 0;

    runTest("sha1", testSha1, failures);
    runTest("sha256", testSha256, failures);
    runTest("aes-256-ige-known-convention", testAesIgeKnownConvention, failures);
    runTest("aes-256-ige", testAesIge, failures);
    runTest("factor-pq-large", testFactorLarge, failures);
    runTest("factor-pq-small", testFactorSmall, failures);
    runTest("rsa-raw", testRsaRaw, failures);
    runTest("mod-pow", testModPow, failures);

    return failures;
}
