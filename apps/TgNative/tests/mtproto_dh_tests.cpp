#include "../core/mtproto_dh.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int runMtprotoDhTests();

namespace {

void appendU32(std::vector<uint8_t>& bytes, uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

uint32_t readU32(const std::vector<uint8_t>& bytes, size_t& offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("test TL int truncated");
    }

    const uint32_t value =
        static_cast<uint32_t>(bytes[offset]) |
        (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
        (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
        (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    offset += 4;
    return value;
}

uint64_t readU64(const std::vector<uint8_t>& bytes, size_t& offset)
{
    if (offset > bytes.size() || bytes.size() - offset < 8) {
        throw std::runtime_error("test TL long truncated");
    }

    uint64_t value = 0;
    unsigned int i;
    for (i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(bytes[offset + i]) << (i * 8);
    }
    offset += 8;
    return value;
}

void appendTlBytes(std::vector<uint8_t>& bytes,
                   const std::vector<uint8_t>& value)
{
    if (value.size() < 254) {
        bytes.push_back(static_cast<uint8_t>(value.size()));
    } else {
        bytes.push_back(254);
        bytes.push_back(static_cast<uint8_t>(value.size()));
        bytes.push_back(static_cast<uint8_t>(value.size() >> 8));
        bytes.push_back(static_cast<uint8_t>(value.size() >> 16));
    }

    bytes.insert(bytes.end(), value.begin(), value.end());
    while ((bytes.size() & 3U) != 0) {
        bytes.push_back(0);
    }
}

std::vector<uint8_t> readTlBytes(const std::vector<uint8_t>& bytes,
                                 size_t& offset)
{
    if (offset >= bytes.size()) {
        throw std::runtime_error("test TL bytes truncated");
    }

    const uint8_t marker = bytes[offset++];
    size_t length;
    if (marker < 254) {
        length = marker;
    } else if (marker == 254) {
        if (bytes.size() - offset < 3) {
            throw std::runtime_error("test TL long bytes truncated");
        }
        length = static_cast<size_t>(bytes[offset]) |
                 (static_cast<size_t>(bytes[offset + 1]) << 8) |
                 (static_cast<size_t>(bytes[offset + 2]) << 16);
        offset += 3;
    } else {
        throw std::runtime_error("test TL marker invalid");
    }

    if (length > bytes.size() - offset) {
        throw std::runtime_error("test TL value truncated");
    }

    std::vector<uint8_t> value(bytes.begin() + offset,
                               bytes.begin() + offset + length);
    offset += length;

    const size_t header = marker == 254 ? 4 : 1;
    const size_t padding = (4 - ((header + length) & 3U)) & 3U;
    if (padding > bytes.size() - offset) {
        throw std::runtime_error("test TL padding truncated");
    }
    offset += padding;
    return value;
}

uint32_t crc32(const std::string& value)
{
    uint32_t crc = 0xffffffffU;
    size_t i;
    for (i = 0; i < value.size(); ++i) {
        crc ^= static_cast<uint8_t>(value[i]);
        unsigned int bit;
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
        }
    }
    return crc ^ 0xffffffffU;
}

std::vector<uint8_t> makeEncryptedServerAnswer(
    const uint8_t nonce[16],
    const uint8_t serverNonce[16],
    const uint8_t newNonce[32])
{
    std::vector<uint8_t> inner;
    appendU32(inner, tg::SERVER_DH_INNER_DATA_ID);
    inner.insert(inner.end(), nonce, nonce + 16);
    inner.insert(inner.end(), serverNonce, serverNonce + 16);
    appendU32(inner, 2);
    appendTlBytes(inner, std::vector<uint8_t>(1, 23));
    appendTlBytes(inner, std::vector<uint8_t>(1, 5));
    appendU32(inner, 123456);

    std::vector<uint8_t> plain;
    const std::vector<uint8_t> digest = tg::sha1Bytes(inner);
    plain.insert(plain.end(), digest.begin(), digest.end());
    plain.insert(plain.end(), inner.begin(), inner.end());
    while ((plain.size() & 15U) != 0) {
        plain.push_back(0xa5);
    }

    uint8_t key[32];
    uint8_t iv[32];
    tg::deriveTmpAes(newNonce, serverNonce, key, iv);
    return tg::aesIgeEncryptBytes(plain, key, iv);
}

void printResult(const char* name, bool passed, const std::string& detail)
{
    if (passed) {
        std::cout << "  ok " << name << std::endl;
    } else {
        std::cout << "  FAIL " << name << ": " << detail << std::endl;
    }
}

} // namespace

int runMtprotoDhTests()
{
    int failures = 0;

    const std::string declarations[5] = {
        "server_DH_params_ok nonce:int128 server_nonce:int128 encrypted_answer:string = Server_DH_Params",
        "server_DH_inner_data nonce:int128 server_nonce:int128 g:int dh_prime:string g_a:string server_time:int = Server_DH_inner_data",
        "client_DH_inner_data nonce:int128 server_nonce:int128 retry_id:long g_b:string = Client_DH_Inner_Data",
        "set_client_DH_params nonce:int128 server_nonce:int128 encrypted_data:string = Set_client_DH_params_answer",
        "dh_gen_ok nonce:int128 server_nonce:int128 new_nonce_hash1:int128 = Set_client_DH_params_answer"
    };
    const uint32_t ids[5] = {
        0xd0e8075cU, 0xb5890dbaU, 0x6643b654U,
        0xf5045f1fU, 0x3bcbf734U
    };
    const uint32_t actualIds[5] = {
        tg::SERVER_DH_PARAMS_OK_ID, tg::SERVER_DH_INNER_DATA_ID,
        tg::CLIENT_DH_INNER_DATA_ID, tg::SET_CLIENT_DH_PARAMS_ID,
        tg::DH_GEN_OK_ID
    };

    bool idsOkay = true;
    size_t i;
    for (i = 0; i < 5; ++i) {
        if (actualIds[i] != ids[i] || crc32(declarations[i]) != ids[i]) {
            idsOkay = false;
        }
    }
    printResult("constructor ids", idsOkay,
                "constructor literal or declaration CRC mismatch");
    if (!idsOkay) {
        ++failures;
    }

    uint8_t newNonce[32];
    uint8_t changedNewNonce[32];
    uint8_t serverNonce[16];
    size_t j;
    for (j = 0; j < 32; ++j) {
        newNonce[j] = static_cast<uint8_t>(j + 1);
        changedNewNonce[j] = newNonce[j];
    }
    for (j = 0; j < 16; ++j) {
        serverNonce[j] = static_cast<uint8_t>(0xf0U - j);
    }
    changedNewNonce[0] ^= 0x80;

    uint8_t key1[32];
    uint8_t iv1[32];
    uint8_t key2[32];
    uint8_t iv2[32];
    uint8_t changedKey[32];
    uint8_t changedIv[32];

    tg::deriveTmpAes(newNonce, serverNonce, key1, iv1);
    tg::deriveTmpAes(newNonce, serverNonce, key2, iv2);
    tg::deriveTmpAes(changedNewNonce, serverNonce, changedKey, changedIv);

    const bool aesDerivationOkay =
        std::memcmp(key1, key2, 32) == 0 &&
        std::memcmp(iv1, iv2, 32) == 0 &&
        std::memcmp(key1, changedKey, 32) != 0;
    printResult("temporary AES derivation", aesDerivationOkay,
                "key/IV were not deterministic 32-byte outputs or key did not change");
    if (!aesDerivationOkay) {
        ++failures;
    }

    uint8_t nonce[16];
    uint8_t wrongNonce[16];
    for (j = 0; j < 16; ++j) {
        nonce[j] = static_cast<uint8_t>(0x30 + j);
        wrongNonce[j] = nonce[j];
    }
    wrongNonce[4] ^= 1;

    bool parseOkay = false;
    std::vector<uint8_t> encrypted;
    try {
        encrypted = makeEncryptedServerAnswer(nonce, serverNonce, newNonce);
        const tg::ServerDhInner parsed =
            tg::parseServerDhParams(&encrypted[0], encrypted.size(),
                                    nonce, serverNonce, newNonce);
        parseOkay = parsed.nonce.size() == 16 &&
                    parsed.serverNonce.size() == 16 &&
                    parsed.g == 2 &&
                    parsed.dhPrime.size() == 1 && parsed.dhPrime[0] == 23 &&
                    parsed.gA.size() == 1 && parsed.gA[0] == 5 &&
                    parsed.serverTime == 123456;
    } catch (...) {
        parseOkay = false;
    }
    printResult("server DH answer parsing", parseOkay,
                "valid encrypted inner data was not parsed");
    if (!parseOkay) {
        ++failures;
    }

    bool wrongNonceThrows = false;
    try {
        tg::parseServerDhParams(&encrypted[0], encrypted.size(),
                                wrongNonce, serverNonce, newNonce);
    } catch (const std::runtime_error&) {
        wrongNonceThrows = true;
    } catch (...) {
        wrongNonceThrows = true;
    }
    printResult("server DH wrong nonce rejected", wrongNonceThrows,
                "wrong nonce was accepted");
    if (!wrongNonceThrows) {
        ++failures;
    }

    bool flippedByteThrows = false;
    try {
        std::vector<uint8_t> altered = encrypted;
        altered[0] ^= 0x01;
        tg::parseServerDhParams(&altered[0], altered.size(),
                                nonce, serverNonce, newNonce);
    } catch (...) {
        flippedByteThrows = true;
    }
    printResult("server DH altered ciphertext rejected", flippedByteThrows,
                "altered ciphertext was accepted");
    if (!flippedByteThrows) {
        ++failures;
    }

    bool clientRoundTripOkay = false;
    try {
        tg::ServerDhInner inner;
        inner.nonce.assign(nonce, nonce + 16);
        inner.serverNonce.assign(serverNonce, serverNonce + 16);

        std::vector<uint8_t> gB(256, 0);
        gB[254] = 0x12;
        gB[255] = 0x34;

        uint8_t randomPadding[16];
        for (j = 0; j < 16; ++j) {
            randomPadding[j] = static_cast<uint8_t>(0x80 + j);
        }

        const std::vector<uint8_t> encryptedClient =
            tg::buildClientDhInnerEncrypted(inner, newNonce, gB,
                                            randomPadding,
                                            sizeof(randomPadding));
        uint8_t key[32];
        uint8_t iv[32];
        tg::deriveTmpAes(newNonce, serverNonce, key, iv);

        const std::vector<uint8_t> plain =
            tg::aesIgeDecryptBytes(encryptedClient, key, iv);
        if (plain.size() >= 20) {
            std::vector<uint8_t> body(plain.begin() + 20, plain.end());
            size_t offset = 0;
            const uint32_t constructor = readU32(body, offset);
            const bool constructorMatches =
                constructor == tg::CLIENT_DH_INNER_DATA_ID;

            offset += 16 + 16;
            const uint64_t retryId = readU64(body, offset);
            const std::vector<uint8_t> decodedGB = readTlBytes(body, offset);
            const size_t trailing = body.size() - offset;

            const std::vector<uint8_t> expectedHash =
                tg::sha1Bytes(&body[0], offset);
            const bool prefixMatches =
                std::memcmp(&plain[0], &expectedHash[0], 20) == 0;

            clientRoundTripOkay =
                prefixMatches && constructorMatches &&
                retryId == 0 && decodedGB == gB && trailing <= 15;
        }
    } catch (...) {
        clientRoundTripOkay = false;
    }
    printResult("client DH encrypted round trip", clientRoundTripOkay,
                "encrypted inner data, SHA-1 prefix, or fields did not round trip");
    if (!clientRoundTripOkay) {
        ++failures;
    }

    bool derivedOutputsOkay = false;
    try {
        tg::ServerDhInner toy;
        toy.g = 2;
        toy.dhPrime.push_back(23);
        toy.gA.push_back(5);

        uint8_t exponent[256];
        std::memset(exponent, 0, sizeof(exponent));
        exponent[255] = 3;

        uint8_t authKey[256];
        uint64_t authKeyId = 0;
        const std::vector<uint8_t> gB =
            tg::computeClientDh(toy, exponent, authKey, authKeyId);
        const uint64_t expectedId = tg::deriveAuthKeyId(authKey);
        const std::vector<uint8_t> salt =
            tg::deriveServerSalt(newNonce, serverNonce);

        const uint8_t expectedSalt[8] = {
            static_cast<uint8_t>(newNonce[0] ^ serverNonce[0]),
            static_cast<uint8_t>(newNonce[1] ^ serverNonce[1]),
            static_cast<uint8_t>(newNonce[2] ^ serverNonce[2]),
            static_cast<uint8_t>(newNonce[3] ^ serverNonce[3]),
            static_cast<uint8_t>(newNonce[4] ^ serverNonce[4]),
            static_cast<uint8_t>(newNonce[5] ^ serverNonce[5]),
            static_cast<uint8_t>(newNonce[6] ^ serverNonce[6]),
            static_cast<uint8_t>(newNonce[7] ^ serverNonce[7])
        };

        uint8_t secondAuthKey[256];
        uint64_t secondAuthKeyId = 0;
        const std::vector<uint8_t> secondGb =
            tg::computeClientDh(toy, exponent, secondAuthKey,
                                secondAuthKeyId);

        derivedOutputsOkay =
            authKeyId == expectedId &&
            authKeyId == secondAuthKeyId &&
            gB.size() == 256 && gB == secondGb &&
            std::memcmp(authKey, secondAuthKey, 256) == 0 &&
            salt.size() == 8 &&
            std::memcmp(&salt[0], expectedSalt, 8) == 0;
    } catch (...) {
        derivedOutputsOkay = false;
    }
    printResult("auth key id and server salt", derivedOutputsOkay,
                "derived outputs had incorrect size or were not deterministic");
    if (!derivedOutputsOkay) {
        ++failures;
    }

    bool dhGenOkay = false;
    try {
        uint8_t authKey[256];
        for (j = 0; j < 256; ++j) {
            authKey[j] = static_cast<uint8_t>(j ^ 0x5a);
        }

        const std::vector<uint8_t> hash =
            tg::computeDhGenNewNonceHash1(newNonce, authKey);
        tg::checkDhGenOk(newNonce, authKey, &hash[0]);

        std::vector<uint8_t> alteredHash = hash;
        alteredHash[7] ^= 0x40;
        bool alteredRejected = false;
        try {
            tg::checkDhGenOk(newNonce, authKey, &alteredHash[0]);
        } catch (...) {
            alteredRejected = true;
        }

        dhGenOkay = hash.size() == 16 && alteredRejected;
    } catch (...) {
        dhGenOkay = false;
    }
    printResult("dh_gen_ok hash verification", dhGenOkay,
                "correct hash was rejected or altered hash was accepted");
    if (!dhGenOkay) {
        ++failures;
    }

    return failures;
}
