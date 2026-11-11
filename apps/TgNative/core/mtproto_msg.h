#ifndef TG_MTPROTO_MSG_H
#define TG_MTPROTO_MSG_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tg {

// authKeyId is the little-endian integer represented by the final eight bytes
// of SHA-1(auth_key).
struct Session {
    uint8_t auth_key[256];
    uint64_t authKeyId;
    uint64_t salt;
    uint64_t sessionId;
    uint32_t seqCounter;
    uint64_t lastMsgId;

    Session();

    uint64_t nextMsgId(uint32_t unixTime);
    uint32_t nextSeqNo(bool contentRelated);
};

struct DecryptedMsg {
    uint64_t salt;
    uint64_t sessionId;
    uint64_t msgId;
    uint32_t seqNo;
    std::vector<uint8_t> body;

    DecryptedMsg();
};

uint64_t nextMsgId(Session& session, uint32_t unixTime);
uint32_t nextSeqNo(Session& session, bool contentRelated);

std::vector<uint8_t> encryptMessage(const Session& session,
                                    uint64_t msgId,
                                    uint32_t seqNo,
                                    const std::vector<uint8_t>& body,
                                    const uint8_t* padding,
                                    std::size_t paddingLen);

// Decrypts a server-to-client packet (MTProto 2.0 direction offset x=8).
bool decryptMessage(const Session& session,
                    const std::vector<uint8_t>& packet,
                    DecryptedMsg& out);

// Decrypts a client-to-server packet (MTProto 2.0 direction offset x=0).
bool decryptClientPacket(const Session& session,
                         const std::vector<uint8_t>& packet,
                         DecryptedMsg& out);

} // namespace tg

#endif

