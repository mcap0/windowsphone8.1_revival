#include "../core/mtproto_msg.h"

#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

class Tester {
public:
    Tester() : checks_(0), failures_(0) {}

    void check(const std::string& name, bool passed, const std::string& detail)
    {
        ++checks_;
        if (passed) {
            std::cout << "  ok " << name << '\n';
        } else {
            ++failures_;
            std::cout << "  FAIL " << name << ": " << detail << '\n';
        }
    }

    int exitCode()
    {
        if (checks_ == 0) {
            ++failures_;
            std::cout << "  FAIL MTProto message suite: no checks were run\n";
        }
        return failures_ == 0 ? 0 : 1;
    }

private:
    int checks_;
    int failures_;
};

std::vector<uint8_t> fromHex(const std::string& hex)
{
    std::vector<uint8_t> result;
    std::size_t i;

    if ((hex.size() & 1u) != 0) {
        return result;
    }

    result.reserve(hex.size() / 2);
    for (i = 0; i < hex.size(); i += 2) {
        unsigned int value = 0;
        std::istringstream parser(hex.substr(i, 2));
        parser >> std::hex >> value;
        if (parser.fail() || value > 255) {
            return std::vector<uint8_t>();
        }
        result.push_back(static_cast<uint8_t>(value));
    }
    return result;
}

std::string toHex(const std::vector<uint8_t>& bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string result;
    std::size_t i;

    result.reserve(bytes.size() * 2);
    for (i = 0; i < bytes.size(); ++i) {
        result.push_back(digits[bytes[i] >> 4]);
        result.push_back(digits[bytes[i] & 0x0f]);
    }
    return result;
}

tg::Session makeVectorSession()
{
    tg::Session session;
    unsigned int i;

    for (i = 0; i < 256; ++i) {
        session.auth_key[i] = static_cast<uint8_t>((i * 7) % 256);
    }

    session.authKeyId = 0x4acffb6c6fde18ceULL;
    session.salt = 0x0123456789abcdefULL;
    session.sessionId = 0xffeeddccbbaa9988ULL;
    return session;
}

std::vector<uint8_t> makePadding(std::size_t length)
{
    std::vector<uint8_t> padding;
    std::size_t i;

    padding.reserve(length);
    for (i = 0; i < length; ++i) {
        padding.push_back(static_cast<uint8_t>(0xa5u + i * 17u));
    }
    return padding;
}

std::vector<uint8_t> makeShortBody()
{
    return fromHex("4d5450726f746f20322e3020656e637279707465642d6c6179657220766563746f72");
}

std::vector<uint8_t> makeBody100()
{
    std::vector<uint8_t> body;
    unsigned int i;

    for (i = 0; i < 100; ++i) {
        body.push_back(static_cast<uint8_t>(i));
    }
    return body;
}

void checkDecryptedVector(Tester& tester,
                          const std::string& name,
                          const std::string& packetHex,
                          const std::vector<uint8_t>& expectedBody)
{
    const tg::Session session = makeVectorSession();
    const std::vector<uint8_t> packet = fromHex(packetHex);
    tg::DecryptedMsg message;
    const bool decrypted = tg::decryptMessage(session, packet, message);
    const bool fieldsMatch = decrypted
        && message.salt == 0x0123456789abcdefULL
        && message.sessionId == 0xffeeddccbbaa9988ULL
        && message.msgId == 0x1122334455667788ULL
        && message.seqNo == 7
        && message.body == expectedBody;

    tester.check(name, fieldsMatch, "packet did not decrypt to the expected fields and body");
}

} // namespace

int runMtprotoMsgTests()
{
    Tester tester;
    const std::string shortClientPacketHex =
        "ce18de6f6cfbcf4af2842a26ab5e3e14bbb013cd7ee2a431b656a862cff0c5f50"
        "c207d6ee69e045de4a5fc6bdd096bfda332fcfe01ac1246e0ceec91af104bd5e"
        "03407ceb721d45c73cc18cbc13bcd129ef036d1d4476343cec064cc00f37fbde"
        "2c91c23dd5e1e62";
    const std::string shortServerPacketHex =
        "ce18de6f6cfbcf4a1f590e1d0a4d5cd1a0dbf3f805595ae79cbf5fea91e4bbf1"
        "5d686f33fbbe81c2b4b34076685b1197183144d259bcec9a745990bd5c56c277"
        "9a15466c2cde2f71fe5724a67e4e599d28f3c8d775ff3517bd2f8d74b860ac70"
        "e7679a846d70dccc";
    const std::string body100ClientPacketHex =
        "ce18de6f6cfbcf4af70bd9c03b2b27c9eeae30e0ef1dbbcc112c7bf802b73df"
        "b3d8979b4a57aabb4d7a01b0995c3a2ae7a12d59587bec846a18b8b2962d032"
        "4bb9d730a0425626aba69a546a2d47cace5304d8941b1ec6d87ac0d81622559"
        "b2bf5795c62820f0bfde6b4f1ff1315e5417aa2f675f58ecf11f2e1e78ff5b"
        "19df0100933a319b232bd4c1ae408656f6ca2adaaaccd1e2fc5519e292c4575"
        "910560c3b92e59172051a4";
    const std::string body100ServerPacketHex =
        "ce18de6f6cfbcf4a7f5a164f7aaf740565dace71a74a3b88cba980b9cf997f"
        "33c5c2ecea460220d7d90f710a4d68ee633175012c45ad9d6f2e51bdd666a0f"
        "cdea32a2a5c6f13c7d3f647065bdddc6ed03869137449234f7e30fe4aef836d"
        "f856e797d00b9baa29aba1cd9d98bdaf8b45f5346a7299d7a50c05535381cc"
        "1b3b8106451d4a7ac252fc7d04d73794f42351b10dbd00849b1f1dbd16a97f"
        "370435305590b7cc472d293a";
    const std::vector<uint8_t> shortBody = makeShortBody();
    const std::vector<uint8_t> body100 = makeBody100();
    const std::vector<uint8_t> shortPadding = makePadding(14);
    const std::vector<uint8_t> body100Padding = makePadding(12);
    const tg::Session session = makeVectorSession();

    const std::vector<uint8_t> encryptedShort = tg::encryptMessage(
        session, 0x1122334455667788ULL, 7, shortBody,
        shortPadding.data(), shortPadding.size());
    tester.check("client-to-server short known-answer packet",
                 toHex(encryptedShort) == shortClientPacketHex,
                 "encrypted bytes differ from the independent oracle vector");

    const std::vector<uint8_t> encrypted100 = tg::encryptMessage(
        session, 0x1122334455667788ULL, 7, body100,
        body100Padding.data(), body100Padding.size());
    tester.check("client-to-server 100-byte known-answer packet",
                 toHex(encrypted100) == body100ClientPacketHex,
                 "encrypted bytes differ from the independent oracle vector");

    checkDecryptedVector(tester, "server-to-client short known-answer packet",
                         shortServerPacketHex, shortBody);
    checkDecryptedVector(tester, "server-to-client 100-byte known-answer packet",
                         body100ServerPacketHex, body100);

    {
        const std::vector<uint8_t> body = fromHex("726f756e642d74726970");
        const std::vector<uint8_t> padding = makePadding(22);
        const std::vector<uint8_t> packet = tg::encryptMessage(
            session, 0x1020304050607080ULL, 9, body,
            padding.data(), padding.size());
        tg::DecryptedMsg decrypted;
        const bool passed = tg::decryptClientPacket(session, packet, decrypted)
            && decrypted.salt == session.salt
            && decrypted.sessionId == session.sessionId
            && decrypted.msgId == 0x1020304050607080ULL
            && decrypted.seqNo == 9
            && decrypted.body == body;
        tester.check("client-to-server encrypt/decrypt round trip", passed,
                     "decrypted message differs from the original");
    }

    {
        std::vector<uint8_t> packet = fromHex(shortServerPacketHex);
        tg::DecryptedMsg message;
        if (packet.size() > 24) {
            packet[24] ^= 0x01;
        }
        tester.check("tampered ciphertext rejected",
                     !tg::decryptMessage(session, packet, message),
                     "tampered packet was accepted");
    }

    {
        std::vector<uint8_t> packet = fromHex(shortServerPacketHex);
        tg::DecryptedMsg message;
        packet[0] ^= 0x01;
        tester.check("wrong auth_key_id rejected",
                     !tg::decryptMessage(session, packet, message),
                     "packet with an incorrect auth_key_id was accepted");
    }

    {
        tg::Session ids;
        const uint64_t first = ids.nextMsgId(1700000000u);
        const uint64_t second = ids.nextMsgId(1700000000u);
        const uint64_t afterClockWentBack = ids.nextMsgId(1699999999u);
        const bool passed = (first & 3u) == 0
            && (second & 3u) == 0
            && (afterClockWentBack & 3u) == 0
            && first < second
            && second < afterClockWentBack;
        tester.check("message IDs are divisible by four and strictly increasing",
                     passed, "generated message IDs are not monotonic/aligned");
    }

    {
        tg::Session sequences;
        const uint32_t firstContent = sequences.nextSeqNo(true);
        const uint32_t nonContent = sequences.nextSeqNo(false);
        const uint32_t secondContent = sequences.nextSeqNo(true);
        tester.check("content-related sequence numbers are odd",
                     (firstContent & 1u) == 1
                         && (secondContent & 1u) == 1
                         && (nonContent & 1u) == 0,
                     "sequence-number parity is incorrect");
    }

    {
        const std::vector<uint8_t> body = makeShortBody();
        const std::vector<uint8_t> badPadding = makePadding(13);
        const std::vector<uint8_t> packet = tg::encryptMessage(
            session, 0x1122334455667788ULL, 7, body,
            badPadding.data(), badPadding.size());
        const std::vector<uint8_t> tooShortPadding = makePadding(11);
        const std::vector<uint8_t> shortPacket = tg::encryptMessage(
            session, 0x1122334455667788ULL, 7, body,
            tooShortPadding.data(), tooShortPadding.size());
        tester.check("invalid padding length or alignment returns empty packet",
                     packet.empty() && shortPacket.empty(),
                     "encryption accepted invalid padding");
    }

    return tester.exitCode();
}
