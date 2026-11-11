#include "crypto.h"

extern "C" {
#include "mbedtls/aes.h"
#include "mbedtls/bignum.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
}

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace tg {
namespace {

typedef uint64_t u64;

u64 addMod(u64 a, u64 b, u64 mod)
{
    return a >= mod - b ? a - (mod - b) : a + b;
}

u64 multiplyMod(u64 a, u64 b, u64 mod)
{
    u64 result = 0;

    a %= mod;
    b %= mod;
    while (b != 0) {
        if ((b & 1u) != 0) {
            result = addMod(result, a, mod);
        }
        b >>= 1;
        if (b != 0) {
            a = addMod(a, a, mod);
        }
    }
    return result;
}

u64 powerMod(u64 base, u64 exp, u64 mod)
{
    u64 result = 1 % mod;

    base %= mod;
    while (exp != 0) {
        if ((exp & 1u) != 0) {
            result = multiplyMod(result, base, mod);
        }
        exp >>= 1;
        if (exp != 0) {
            base = multiplyMod(base, base, mod);
        }
    }
    return result;
}

u64 gcd(u64 a, u64 b)
{
    while (b != 0) {
        const u64 remainder = a % b;
        a = b;
        b = remainder;
    }
    return a;
}

bool isPrime(u64 n)
{
    static const u64 smallPrimes[] = {
        2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37
    };
    static const u64 witnesses[] = {
        2, 325, 9375, 28178, 450775, 9780504, 1795265022
    };

    if (n < 2) {
        return false;
    }

    for (std::size_t i = 0;
         i < sizeof(smallPrimes) / sizeof(smallPrimes[0]); ++i) {
        if (n == smallPrimes[i]) {
            return true;
        }
        if (n % smallPrimes[i] == 0) {
            return false;
        }
    }

    u64 d = n - 1;
    unsigned int shifts = 0;
    while ((d & 1u) == 0) {
        d >>= 1;
        ++shifts;
    }

    for (std::size_t i = 0;
         i < sizeof(witnesses) / sizeof(witnesses[0]); ++i) {
        const u64 a = witnesses[i] % n;
        if (a == 0) {
            continue;
        }

        u64 x = powerMod(a, d, n);
        if (x == 1 || x == n - 1) {
            continue;
        }

        bool composite = true;
        for (unsigned int r = 1; r < shifts; ++r) {
            x = multiplyMod(x, x, n);
            if (x == n - 1) {
                composite = false;
                break;
            }
        }
        if (composite) {
            return false;
        }
    }
    return true;
}

u64 pollardRho(u64 n)
{
    if ((n & 1u) == 0) {
        return 2;
    }
    if (n % 3 == 0) {
        return 3;
    }

    for (u64 c = 1; ; ++c) {
        const u64 constant = c % n;
        u64 x = 2 % n;
        u64 y = x;
        u64 divisor = 1;
        unsigned int iterations = 0;

        while (divisor == 1 && iterations < 1000000) {
            x = addMod(multiplyMod(x, x, n), constant, n);
            y = addMod(multiplyMod(y, y, n), constant, n);
            y = addMod(multiplyMod(y, y, n), constant, n);
            const u64 difference = x > y ? x - y : y - x;
            divisor = gcd(difference, n);
            ++iterations;
        }

        if (divisor > 1 && divisor < n) {
            return divisor;
        }
    }
}

void collectPrimeFactors(u64 n, std::vector<u64>& factors)
{
    if (n == 1) {
        return;
    }
    if (isPrime(n)) {
        factors.push_back(n);
        return;
    }

    const u64 divisor = pollardRho(n);
    collectPrimeFactors(divisor, factors);
    collectPrimeFactors(n / divisor, factors);
}

void aesIge(const uint8_t key[32], const uint8_t iv[32],
            uint8_t* data, std::size_t len, bool encrypt)
{
    if (key == 0 || iv == 0 || (len != 0 && data == 0)) {
        throw std::invalid_argument("AES-IGE received a null pointer");
    }
    if ((len & 15u) != 0) {
        throw std::invalid_argument("AES-IGE length must be a multiple of 16");
    }
    if (len == 0) {
        return;
    }

    mbedtls_aes_context context;
    mbedtls_aes_init(&context);

    int status = encrypt
        ? mbedtls_aes_setkey_enc(&context, key, 256)
        : mbedtls_aes_setkey_dec(&context, key, 256);
    if (status != 0) {
        mbedtls_aes_free(&context);
        throw std::runtime_error("mbedTLS AES key setup failed");
    }

    uint8_t previousInput[16];
    uint8_t previousOutput[16];
    // IGE initializes the previous ciphertext from iv[0..16) and the
    // previous plaintext from iv[16..32).
    std::memcpy(previousOutput, iv, 16);
    std::memcpy(previousInput, iv + 16, 16);

    uint8_t inputBlock[16];
    uint8_t outputBlock[16];
    uint8_t savedBlock[16];

    for (std::size_t offset = 0; offset < len; offset += 16) {
        std::memcpy(savedBlock, data + offset, 16);

        for (std::size_t i = 0; i < 16; ++i) {
            inputBlock[i] = static_cast<uint8_t>(
                savedBlock[i] ^ (encrypt ? previousOutput[i] : previousInput[i]));
        }

        status = mbedtls_aes_crypt_ecb(
            &context,
            encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT,
            inputBlock,
            outputBlock);
        if (status != 0) {
            mbedtls_aes_free(&context);
            throw std::runtime_error("mbedTLS AES block operation failed");
        }

        if (encrypt) {
            for (std::size_t i = 0; i < 16; ++i) {
                data[offset + i] = static_cast<uint8_t>(
                    outputBlock[i] ^ previousInput[i]);
            }
            std::memcpy(previousInput, savedBlock, 16);
            std::memcpy(previousOutput, data + offset, 16);
        } else {
            for (std::size_t i = 0; i < 16; ++i) {
                data[offset + i] = static_cast<uint8_t>(
                    outputBlock[i] ^ previousOutput[i]);
            }
            std::memcpy(previousInput, data + offset, 16);
            std::memcpy(previousOutput, savedBlock, 16);
        }
    }

    mbedtls_aes_free(&context);
}

} // namespace

std::vector<uint8_t> sha1(const uint8_t* data, std::size_t len)
{
    if (len != 0 && data == 0) {
        throw std::invalid_argument("SHA-1 received a null input");
    }

    static const uint8_t emptyInput = 0;
    const uint8_t* input = data == 0 ? &emptyInput : data;
    std::vector<uint8_t> result(20);

    if (mbedtls_sha1(input, len, &result[0]) != 0) {
        throw std::runtime_error("mbedTLS SHA-1 failed");
    }
    return result;
}

std::vector<uint8_t> sha256(const uint8_t* data, std::size_t len)
{
    if (len != 0 && data == 0) {
        throw std::invalid_argument("SHA-256 received a null input");
    }

    static const uint8_t emptyInput = 0;
    const uint8_t* input = data == 0 ? &emptyInput : data;
    std::vector<uint8_t> result(32);

    if (mbedtls_sha256(input, len, &result[0], 0) != 0) {
        throw std::runtime_error("mbedTLS SHA-256 failed");
    }
    return result;
}

void aesIgeEncrypt(const uint8_t key[32], const uint8_t iv[32],
                   uint8_t* data, std::size_t len)
{
    aesIge(key, iv, data, len, true);
}

void aesIgeDecrypt(const uint8_t key[32], const uint8_t iv[32],
                   uint8_t* data, std::size_t len)
{
    aesIge(key, iv, data, len, false);
}

uint64_t mulMod(uint64_t a, uint64_t b, uint64_t mod)
{
    if (mod == 0) {
        throw std::invalid_argument("mulMod modulus must be nonzero");
    }
    return multiplyMod(a, b, mod);
}

uint64_t modPow(uint64_t base, uint64_t exp, uint64_t mod)
{
    if (mod == 0) {
        throw std::invalid_argument("modPow modulus must be nonzero");
    }
    return powerMod(base, exp, mod);
}

void factorPq(uint64_t pq, uint64_t& p, uint64_t& q)
{
    if (pq < 4) {
        throw std::invalid_argument("pq must be a product of two primes");
    }

    std::vector<u64> factors;
    collectPrimeFactors(pq, factors);
    if (factors.size() != 2) {
        throw std::invalid_argument("pq is not a product of exactly two primes");
    }

    std::sort(factors.begin(), factors.end());
    if (factors[0] == factors[1]) {
        throw std::invalid_argument("pq must have two distinct prime factors");
    }

    p = factors[0];
    q = factors[1];
}

std::vector<uint8_t> rsaRawEncrypt(const uint8_t* n, std::size_t nLen,
                                   uint32_t e, const uint8_t* m,
                                   std::size_t mLen)
{
    if (n == 0 || nLen == 0 || (mLen != 0 && m == 0)) {
        throw std::invalid_argument("RSA received an invalid input");
    }

    const uint8_t exponentBytes[4] = {
        static_cast<uint8_t>((e >> 24) & 0xffu),
        static_cast<uint8_t>((e >> 16) & 0xffu),
        static_cast<uint8_t>((e >> 8) & 0xffu),
        static_cast<uint8_t>(e & 0xffu)
    };
    static const uint8_t emptyInput = 0;
    const uint8_t* message = m == 0 ? &emptyInput : m;
    std::vector<uint8_t> result(nLen);

    mbedtls_mpi modulus;
    mbedtls_mpi exponent;
    mbedtls_mpi input;
    mbedtls_mpi output;
    mbedtls_mpi_init(&modulus);
    mbedtls_mpi_init(&exponent);
    mbedtls_mpi_init(&input);
    mbedtls_mpi_init(&output);

    int status = mbedtls_mpi_read_binary(&modulus, n, nLen);
    if (status == 0) {
        status = mbedtls_mpi_read_binary(&exponent, exponentBytes,
                                         sizeof(exponentBytes));
    }
    if (status == 0) {
        status = mbedtls_mpi_read_binary(&input, message, mLen);
    }
    if (status == 0) {
        status = mbedtls_mpi_exp_mod(
            &output, &input, &exponent, &modulus, 0);
    }
    if (status == 0) {
        status = mbedtls_mpi_write_binary(&output, &result[0], nLen);
    }

    mbedtls_mpi_free(&modulus);
    mbedtls_mpi_free(&exponent);
    mbedtls_mpi_free(&input);
    mbedtls_mpi_free(&output);

    if (status != 0) {
        throw std::runtime_error("mbedTLS RSA modular exponentiation failed");
    }
    return result;
}

} // namespace tg
