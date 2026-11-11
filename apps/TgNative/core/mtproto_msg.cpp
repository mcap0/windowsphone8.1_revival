#include "mtproto_msg.h"

#include "crypto.h"

#include <cstring>
#include <limits>

namespace tg {
namespace {

void appendUInt32(std::vector<uint8_t>& output, uint32_t value)
{
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value >> 16));
    output.push_back(static_cast<uint8_t>(value >> 24));
}

void appendUInt64(std::vector<uint8_t>& output, uint64_t value)
{
    unsigned int i;
    for (i = 0; i < 8; ++i) {
        output.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

uint32_t readUInt32(const uint8_t* input)
{
    return static_cast<uint32_t>(input[0])
        | (static_cast<uint32_t>(input[1]) << 8)
        | (static_cast<uint32_t>(input[2]) << 16)
        | (static_cast<uint32_t>(input[3]) << 24);
}

uint64_t readUInt64(const uint8_t* input)
{
    uint64_t value = 0;
    unsigned int i;
    for (i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(input[i]) << (i * 8);
    }
    return value;
}

uint64_t authKeyIdFromKey(const uint8_t authKey[256])
{
    const std::vector<uint8_t> digest = sha1(authKey, 256);
    uint64_t value = 0;
    unsigned int i;

    if (digest.size() < 20) {
        return 0;
    }

    for (i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(digest[12 + i]) << (i * 8);
    }
    return value;
}

bool equalConstantTime(const uint8_t* left,
                       const uint8_t* right,
                       std::size_t length)
{
    uint8_t difference = 0;
    std::size_t i;

    for (i = 0; i < length; ++i) {
        difference = static_cast<uint8_t>(difference | (left[i] ^ right[i]));
    }
    return difference == 0;
}

void deriveAesKeyAndIv(const uint8_t authKey[256],
                       const uint8_t msgKey[16],
                       std::size_t directionOffset,
                       uint8_t aesKey[32],
                       uint8_t aesIv[32])
{
    std::vector<uint8_t> inputA;
    std::vector<uint8_t> inputB;
    std::vector<uint8_t> hashA;
    std::vector<uint8_t> hashB;

    inputA.reserve(48);
    inputA.insert(inputA.end(), msgKey, msgKey + 16);
    inputA.insert(inputA.end(), authKey + directionOffset,
                  authKey + directionOffset + 36);

    inputB.reserve(52);
    inputB.insert(inputB.end(), authKey + directionOffset + 40,
                  authKey + directionOffset + 76);
    inputB.insert(inputB.end(), msgKey, msgKey + 16);

    hashA = sha256(inputA.data(), inputA.size());
    hashB = sha256(inputB.data(), inputB.size());

    if (hashA.size() < 32 || hashB.size() < 32) {
        std::memset(aesKey, 0, 32);
        std::memset(aesIv, 0, 32);
        return;
    }

    std::memcpy(aesKey, hashA.data(), 8);
    std::memcpy(aesKey + 8, hashB.data() + 8, 16);
    std::memcpy(aesKey + 24, hashA.data() + 24, 8);

    std::memcpy(aesIv, hashB.data(), 8);
    std::memcpy(aesIv + 8, hashA.data() + 8, 16);
    std::memcpy(aesIv + 24, hashB.data() + 24, 8);
}

void calculateMessageKey(const uint8_t authKey[256],
                         const std::vector<uint8_t>& plaintext,
                         std::size_t directionOffset,
                         uint8_t msgKey[16])
{
    std::vector<uint8_t> input;
    std::vector<uint8_t> digest;
    const std::size_t start = 88 + directionOffset;

    input.reserve(32 + plaintext.size());
    input.insert(input.end(), authKey + start, authKey + start + 32);
    input.insert(input.end(), plaintext.begin(), plaintext.end());

    digest = sha256(input.data(), input.size());
    if (digest.size() < 24) {
        std::memset(msgKey, 0, 16);
        return;
    }
    std::memcpy(msgKey, digest.data() + 8, 16);
}

std::vector<uint8_t> encryptWithDirection(const Session& session,
                                          uint64_t msgId,
                                          uint32_t seqNo,
                                          const std::vector<uint8_t>& body,
                                          const uint8_t* padding,
                                          std::size_t paddingLen,
                                          std::size_t directionOffset)
{
    std::vector<uint8_t> plaintext;
    std::vector<uint8_t> packet;
    uint8_t msgKey[16];
    uint8_t aesKey[32];
    uint8_t aesIv[32];

    if (paddingLen < 12 || paddingLen > 1024
        || (paddingLen != 0 && padding == NULL)
        || body.size() > (std::numeric_limits<uint32_t>::max)()
        || (32 + body.size() + paddingLen) % 16 != 0
        || authKeyIdFromKey(session.auth_key) != session.authKeyId) {
        return packet;
    }

    plaintext.reserve(32 + body.size() + paddingLen);
    appendUInt64(plaintext, session.salt);
    appendUInt64(plaintext, session.sessionId);
    appendUInt64(plaintext, msgId);
    appendUInt32(plaintext, seqNo);
    appendUInt32(plaintext, static_cast<uint32_t>(body.size()));
    plaintext.insert(plaintext.end(), body.begin(), body.end());
    plaintext.insert(plaintext.end(), padding, padding + paddingLen);

    calculateMessageKey(session.auth_key, plaintext, directionOffset, msgKey);
    deriveAesKeyAndIv(session.auth_key, msgKey, directionOffset, aesKey, aesIv);

    packet.reserve(24 + plaintext.size());
    appendUInt64(packet, session.authKeyId);
    packet.insert(packet.end(), msgKey, msgKey + 16);
    packet.insert(packet.end(), plaintext.begin(), plaintext.end());
    aesIgeEncrypt(aesKey, aesIv, packet.data() + 24, plaintext.size());

    return packet;
}

bool decryptWithDirection(const Session& session,
                          const std::vector<uint8_t>& packet,
                          DecryptedMsg& out,
                          std::size_t directionOffset)
{
    std::vector<uint8_t> plaintext;
    uint8_t aesKey[32];
    uint8_t aesIv[32];
    uint8_t computedMsgKey[16];
    std::size_t bodyLength;
    std::size_t paddingLength;
    DecryptedMsg result;

    if (session.authKeyId != authKeyIdFromKey(session.auth_key)
        || packet.size() < 24 + 16
        || (packet.size() - 24) % 16 != 0
        || readUInt64(packet.data()) != session.authKeyId) {
        return false;
    }

    deriveAesKeyAndIv(session.auth_key, packet.data() + 8,
                      directionOffset, aesKey, aesIv);

    plaintext.assign(packet.begin() + 24, packet.end());
    aesIgeDecrypt(aesKey, aesIv, plaintext.data(), plaintext.size());

    calculateMessageKey(session.auth_key, plaintext, directionOffset,
                        computedMsgKey);
    if (!equalConstantTime(packet.data() + 8, computedMsgKey, 16)) {
        return false;
    }

    if (plaintext.size() < 32 + 12) {
        return false;
    }

    bodyLength = readUInt32(plaintext.data() + 28);
    if (bodyLength > plaintext.size() - 32) {
        return false;
    }

    paddingLength = plaintext.size() - 32 - bodyLength;
    if (paddingLength < 12 || paddingLength > 1024) {
        return false;
    }

    result.salt = readUInt64(plaintext.data());
    result.sessionId = readUInt64(plaintext.data() + 8);
    result.msgId = readUInt64(plaintext.data() + 16);
    result.seqNo = readUInt32(plaintext.data() + 24);
    result.body.assign(plaintext.begin() + 32,
                       plaintext.begin() + 32 + bodyLength);
    out = result;
    return true;
}

} // namespace

Session::Session()
    : authKeyId(0),
      salt(0),
      sessionId(0),
      seqCounter(0),
      lastMsgId(0)
{
    std::memset(auth_key, 0, sizeof(auth_key));
}

uint64_t Session::nextMsgId(uint32_t unixTime)
{
    const uint64_t candidate = static_cast<uint64_t>(unixTime) << 32;
    uint64_t result = candidate;

    if (result <= lastMsgId) {
        if (lastMsgId > (std::numeric_limits<uint64_t>::max)() - 4) {
            return lastMsgId;
        }
        result = lastMsgId + 4;
    }

    // Both the time-derived candidate and each increment are divisible by 4.
    lastMsgId = result;
    return result;
}

uint32_t Session::nextSeqNo(bool contentRelated)
{
    const uint32_t result = seqCounter * 2 + (contentRelated ? 1u : 0u);
    if (contentRelated) {
        ++seqCounter;
    }
    return result;
}

DecryptedMsg::DecryptedMsg()
    : salt(0),
      sessionId(0),
      msgId(0),
      seqNo(0)
{
}

uint64_t nextMsgId(Session& session, uint32_t unixTime)
{
    return session.nextMsgId(unixTime);
}

uint32_t nextSeqNo(Session& session, bool contentRelated)
{
    return session.nextSeqNo(contentRelated);
}

std::vector<uint8_t> encryptMessage(const Session& session,
                                    uint64_t msgId,
                                    uint32_t seqNo,
                                    const std::vector<uint8_t>& body,
                                    const uint8_t* padding,
                                    std::size_t paddingLen)
{
    return encryptWithDirection(session, msgId, seqNo, body, padding,
                                paddingLen, 0);
}

bool decryptMessage(const Session& session,
                    const std::vector<uint8_t>& packet,
                    DecryptedMsg& out)
{
    return decryptWithDirection(session, packet, out, 8);
}

bool decryptClientPacket(const Session& session,
                         const std::vector<uint8_t>& packet,
                         DecryptedMsg& out)
{
    return decryptWithDirection(session, packet, out, 0);
}

} // namespace tg

