#include "core/crypto.h"
#include "core/mtproto_auth.h"
#include "core/mtproto_dh.h"
#include "core/mtproto_msg.h"
#include "core/mtproto_rpc.h"
#include "core/tl.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>

namespace {

const uint32_t kMaximumPacketLength = 16u * 1024u * 1024u;
const uint32_t kReqDhParamsId = 0xd712e4beu;
const uint32_t kGzipPackedId = 0x3072cfa1u;
const uint32_t kVectorId = 0x1cb5c415u;

const uint32_t kRpcResultId = 0xf35c6d01u;
const uint32_t kRpcErrorId = 0x2144ca19u;
const uint32_t kPongId = 0x347773c5u;
const uint32_t kMsgsAckId = 0x62d6b459u;
const uint32_t kNewSessionCreatedId = 0x9ec20908u;
const uint32_t kBadServerSaltId = 0xedab447bu;
const uint32_t kBadMsgNotificationId = 0xa7eff811u;
const uint32_t kMsgContainerId = 0x73f1f8dcu;

void appendLe32(std::vector<uint8_t>& out, uint32_t value)
{
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

void appendLe64(std::vector<uint8_t>& out, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
}

uint32_t readLe32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
           (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) |
           (static_cast<uint32_t>(data[3]) << 24);
}

uint64_t readLe64(const uint8_t* data)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

class TlCursor {
public:
    explicit TlCursor(const std::vector<uint8_t>& data)
        : data_(data), position_(0)
    {
    }

    std::size_t remaining() const
    {
        return data_.size() - position_;
    }

    uint32_t readUInt32()
    {
        require(4);
        const uint32_t value = readLe32(&data_[position_]);
        position_ += 4;
        return value;
    }

    int32_t readInt32()
    {
        return static_cast<int32_t>(readUInt32());
    }

    uint64_t readUInt64()
    {
        require(8);
        const uint64_t value = readLe64(&data_[position_]);
        position_ += 8;
        return value;
    }

    std::vector<uint8_t> readBytes()
    {
        require(1);
        const uint8_t first = data_[position_++];
        std::size_t length = 0;
        std::size_t headerLength = 1;

        if (first < 254) {
            length = first;
        } else if (first == 254) {
            require(3);
            length = static_cast<std::size_t>(data_[position_]) |
                     (static_cast<std::size_t>(data_[position_ + 1]) << 8) |
                     (static_cast<std::size_t>(data_[position_ + 2]) << 16);
            position_ += 3;
            headerLength = 4;
        } else {
            throw std::runtime_error("invalid TL bytes length marker");
        }

        if (length > remaining()) {
            throw std::runtime_error("truncated TL bytes value");
        }

        std::vector<uint8_t> result(data_.begin() + position_,
                                    data_.begin() + position_ + length);
        position_ += length;

        const std::size_t padding =
            (4 - ((headerLength + length) & 3u)) & 3u;
        require(padding);
        position_ += padding;
        return result;
    }

    std::vector<uint8_t> readMessageBody()
    {
        const int32_t length = readInt32();
        if (length < 0 || static_cast<uint32_t>(length) > remaining()) {
            throw std::runtime_error("invalid message length in msg_container");
        }

        const std::size_t bodyLength = static_cast<std::size_t>(length);
        std::vector<uint8_t> result(data_.begin() + position_,
                                    data_.begin() + position_ + bodyLength);
        position_ += bodyLength;
        return result;
    }

    std::string readString()
    {
        const std::vector<uint8_t> bytes = readBytes();
        return std::string(bytes.begin(), bytes.end());
    }

    std::size_t readVectorCount()
    {
        if (readUInt32() != kVectorId) {
            throw std::runtime_error("invalid TL vector constructor");
        }
        const int32_t count = readInt32();
        if (count < 0 || count > 1000000) {
            throw std::runtime_error("invalid TL vector length");
        }
        return static_cast<std::size_t>(count);
    }

    std::vector<uint8_t> readRemaining()
    {
        std::vector<uint8_t> result(data_.begin() + position_, data_.end());
        position_ = data_.size();
        return result;
    }

private:
    void require(std::size_t count) const
    {
        if (count > data_.size() - position_) {
            throw std::runtime_error("truncated TL object");
        }
    }

    const std::vector<uint8_t>& data_;
    std::size_t position_;
};

void readRandom(uint8_t* output, std::size_t length)
{
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error(std::string("opening /dev/urandom: ") +
                                 std::strerror(errno));
    }

    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t count = read(fd, output + offset, length - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            const std::string error =
                count == 0 ? "unexpected end of /dev/urandom" :
                std::string("reading /dev/urandom: ") + std::strerror(errno);
            close(fd);
            throw std::runtime_error(error);
        }
    }
    close(fd);
}

void sendAll(int fd, const uint8_t* data, std::size_t length,
             const std::string& operation)
{
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t count = send(fd, data + offset, length - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error(operation + ": " +
                (count == 0 ? std::string("socket closed") :
                 std::string(std::strerror(errno))));
        }
    }
}

void readExact(int fd, uint8_t* data, std::size_t length,
               const std::string& operation)
{
    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t count = recv(fd, data + offset, length - offset, 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            throw std::runtime_error(operation + ": " +
                (count == 0 ? std::string("connection closed") :
                 std::string(std::strerror(errno))));
        }
    }
}

class SocketHandle {
public:
    explicit SocketHandle(int fd) : fd_(fd) {}

    ~SocketHandle()
    {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    SocketHandle(SocketHandle&& other) noexcept : fd_(other.fd_)
    {
        other.fd_ = -1;
    }

    int get() const
    {
        return fd_;
    }

private:
    SocketHandle(const SocketHandle&);
    SocketHandle& operator=(const SocketHandle&);
    SocketHandle& operator=(SocketHandle&&);
    int fd_;
};

SocketHandle connectToTestDc()
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }

    struct timeval timeout;
    timeout.tv_sec = 15;
    timeout.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        const std::string error = std::strerror(errno);
        close(fd);
        throw std::runtime_error("setting socket timeout: " + error);
    }

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(443);
    if (inet_pton(AF_INET, "149.154.167.40", &address.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("invalid Telegram test DC address");
    }
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                sizeof(address)) < 0) {
        const std::string error = std::strerror(errno);
        close(fd);
        throw std::runtime_error("connecting to Telegram test DC: " + error);
    }
    return SocketHandle(fd);
}

uint64_t makeHandshakeMessageId()
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        throw std::runtime_error(std::string("clock_gettime: ") +
                                 std::strerror(errno));
    }
    if (now.tv_sec <= 0 ||
        static_cast<uint64_t>(now.tv_sec) > UINT64_C(0xffffffff)) {
        throw std::runtime_error(
            "system time is outside the MTProto message-id range");
    }

    const uint64_t seconds = static_cast<uint64_t>(now.tv_sec);
    const uint64_t fraction =
        (static_cast<uint64_t>(now.tv_nsec) << 32) / UINT64_C(1000000000);
    return ((seconds << 32) | fraction) & ~UINT64_C(3);
}

void sendHandshakeMessage(int fd, const std::vector<uint8_t>& body)
{
    if (body.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("handshake body is too large");
    }

    std::vector<uint8_t> packet;
    packet.reserve(20 + body.size());
    appendLe64(packet, 0);
    appendLe64(packet, makeHandshakeMessageId());
    appendLe32(packet, static_cast<uint32_t>(body.size()));
    packet.insert(packet.end(), body.begin(), body.end());

    std::vector<uint8_t> length;
    appendLe32(length, static_cast<uint32_t>(packet.size()));
    sendAll(fd, &length[0], length.size(), "sending handshake length");
    sendAll(fd, &packet[0], packet.size(), "sending handshake message");
}

uint64_t readHandshakeMessage(int fd, std::vector<uint8_t>& body)
{
    uint8_t lengthBytes[4];
    readExact(fd, lengthBytes, sizeof(lengthBytes), "reading handshake length");
    const uint32_t length = readLe32(lengthBytes);
    if (length < 20 || length > kMaximumPacketLength) {
        throw std::runtime_error("invalid handshake packet length");
    }

    std::vector<uint8_t> packet(length);
    readExact(fd, &packet[0], packet.size(), "reading handshake packet");
    if (readLe64(&packet[0]) != 0) {
        throw std::runtime_error("handshake auth_key_id is not zero");
    }
    const uint32_t bodyLength = readLe32(&packet[16]);
    if (bodyLength != packet.size() - 20) {
        throw std::runtime_error(
            "handshake message length does not match packet");
    }
    body.assign(packet.begin() + 20, packet.end());
    return readLe64(&packet[8]);
}

std::vector<uint8_t> decodeHex(const std::string& hex)
{
    if ((hex.size() & 1u) != 0) {
        throw std::runtime_error("invalid RSA modulus hex length");
    }
    std::vector<uint8_t> result;
    result.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const char a = hex[i];
        const char b = hex[i + 1];
        const int high = a >= '0' && a <= '9' ? a - '0' :
                         a >= 'a' && a <= 'f' ? a - 'a' + 10 :
                         a >= 'A' && a <= 'F' ? a - 'A' + 10 : -1;
        const int low = b >= '0' && b <= '9' ? b - '0' :
                        b >= 'a' && b <= 'f' ? b - 'a' + 10 :
                        b >= 'A' && b <= 'F' ? b - 'A' + 10 : -1;
        if (high < 0 || low < 0) {
            throw std::runtime_error("invalid RSA modulus hex digit");
        }
        result.push_back(static_cast<uint8_t>((high << 4) | low));
    }
    return result;
}

std::vector<uint8_t> minimalBigEndian(uint64_t value)
{
    std::vector<uint8_t> result;
    do {
        result.push_back(static_cast<uint8_t>(value & 0xffu));
        value >>= 8;
    } while (value != 0);
    std::reverse(result.begin(), result.end());
    return result;
}

uint64_t bigEndianToUint64(const std::vector<uint8_t>& bytes)
{
    if (bytes.empty() || bytes.size() > 8) {
        throw std::runtime_error("server pq does not fit in uint64");
    }
    uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

tg::Session establishAuthKey(int fd)
{
    uint8_t nonce[16];
    readRandom(nonce, sizeof(nonce));

    const uint8_t intermediateTransport[4] = {0xee, 0xee, 0xee, 0xee};
    sendAll(fd, intermediateTransport, sizeof(intermediateTransport),
            "sending intermediate transport marker");
    sendHandshakeMessage(fd, tg::buildReqPqMulti(nonce));

    std::vector<uint8_t> body;
    readHandshakeMessage(fd, body);
    const tg::ResPQ resPq =
        tg::parseResPQ(body.empty() ? NULL : &body[0], body.size());
    if (std::memcmp(resPq.nonce, nonce, sizeof(nonce)) != 0) {
        throw std::runtime_error("resPQ nonce mismatch");
    }

    const std::string modulusHex =
        "c8c11d635691fac091dd9489aedced2932aa8a0bcefef05fa800892d9b52ed03"
        "200865c9e97211cb2ee6c7ae96d3fb0e15aeffd66019b44a08a240cfdd2868a"
        "85e1f54d6fa5deaa041f6941ddf302690d61dc476385c2fa655142353cb4e4b"
        "59f6e5b6584db76fe8b1370263246c010c93d011014113ebdf987d093f9d37c"
        "2be48352d69a1683f8f6e6c2167983c761e3ab169fde5daaa12123fa1beab621"
        "e4da5935e9c198f82f35eae583a99386d8110ea6bd1abb0f568759f62694419"
        "ea5f69847c43462abef858b4cb5edc84e7b9226cd7bd7e183aa974a712c079"
        "dde85b9dc063b8a5c08e8f859c0ee5dcd824c7807f20153361a7f63cfd2a433"
        "a1be7f5";
    const std::vector<uint8_t> modulus = decodeHex(modulusHex);
    if (modulus.size() != 256) {
        throw std::runtime_error("test DC RSA modulus is not 256 bytes");
    }
    const uint64_t fingerprint =
        tg::rsaFingerprint(&modulus[0], modulus.size(), 65537);
    if (std::find(resPq.fingerprints.begin(), resPq.fingerprints.end(),
                  fingerprint) == resPq.fingerprints.end()) {
        throw std::runtime_error("test DC RSA fingerprint is not advertised");
    }

    const uint64_t pq = bigEndianToUint64(resPq.pq);
    uint64_t pValue = 0;
    uint64_t qValue = 0;
    tg::factorPq(pq, pValue, qValue);
    if (pValue == 0 || qValue == 0) {
        throw std::runtime_error("pq factorization failed");
    }
    if (pValue > qValue) {
        std::swap(pValue, qValue);
    }
    if (pValue > UINT64_MAX / qValue || pValue * qValue != pq) {
        throw std::runtime_error("pq factors do not multiply to pq");
    }

    const std::vector<uint8_t> p = minimalBigEndian(pValue);
    const std::vector<uint8_t> q = minimalBigEndian(qValue);
    uint8_t newNonce[32];
    readRandom(newNonce, sizeof(newNonce));
    const std::vector<uint8_t> inner =
        tg::buildPqInnerDataDc(resPq.pq, p, q, nonce, resPq.serverNonce,
                               newNonce, 10002);
    if (inner.size() > 235) {
        throw std::runtime_error(
            "p_q_inner_data_dc is too large for RSA padding");
    }

    uint8_t rsaRandom[256];
    readRandom(rsaRandom, sizeof(rsaRandom));
    const std::vector<uint8_t> encryptedData =
        tg::rsaPadAndEncrypt(&inner[0], inner.size(), &modulus[0],
                             modulus.size(), 65537, rsaRandom,
                             sizeof(rsaRandom));
    if (encryptedData.size() != 256) {
        throw std::runtime_error("RSA encrypted data is not 256 bytes");
    }

    tg::TlWriter reqDh;
    reqDh.writeConstructor(kReqDhParamsId);
    reqDh.writeRaw(nonce, sizeof(nonce));
    reqDh.writeRaw(resPq.serverNonce, sizeof(resPq.serverNonce));
    reqDh.writeBytes(p.empty() ? NULL : &p[0], p.size());
    reqDh.writeBytes(q.empty() ? NULL : &q[0], q.size());
    int64_t signedFingerprint = 0;
    std::memcpy(&signedFingerprint, &fingerprint, sizeof(fingerprint));
    reqDh.writeInt64(signedFingerprint);
    reqDh.writeBytes(&encryptedData[0], encryptedData.size());
    sendHandshakeMessage(fd, reqDh.data());

    readHandshakeMessage(fd, body);
    tg::TlReader dhReader(body.empty() ? NULL : &body[0], body.size());
    if (dhReader.readConstructor() != tg::SERVER_DH_PARAMS_OK_ID) {
        throw std::runtime_error("server did not return server_DH_params_ok");
    }
    uint8_t responseNonce[16];
    uint8_t responseServerNonce[16];
    dhReader.readRaw(responseNonce, sizeof(responseNonce));
    dhReader.readRaw(responseServerNonce, sizeof(responseServerNonce));
    if (std::memcmp(responseNonce, nonce, sizeof(nonce)) != 0 ||
        std::memcmp(responseServerNonce, resPq.serverNonce,
                    sizeof(resPq.serverNonce)) != 0) {
        throw std::runtime_error("server_DH_params_ok nonce mismatch");
    }
    const std::string encryptedAnswer = dhReader.readBytes();
    if (encryptedAnswer.empty()) {
        throw std::runtime_error(
            "server_DH_params_ok has no encrypted answer");
    }

    const tg::ServerDhInner dhInner =
        tg::parseServerDhParams(
            reinterpret_cast<const uint8_t*>(encryptedAnswer.data()),
            encryptedAnswer.size(), nonce, resPq.serverNonce, newNonce);
    if (!tg::checkDhParams(dhInner)) {
        throw std::runtime_error("server DH parameters failed validation");
    }

    uint8_t randomB[256];
    uint8_t authKey[256];
    uint64_t authKeyId = 0;
    readRandom(randomB, sizeof(randomB));
    const std::vector<uint8_t> gB =
        tg::computeClientDh(dhInner, randomB, authKey, authKeyId);
    uint8_t padding[16];
    readRandom(padding, sizeof(padding));
    const std::vector<uint8_t> encryptedClientDh =
        tg::buildClientDhInnerEncrypted(dhInner, newNonce, gB,
                                        padding, sizeof(padding));

    tg::TlWriter setClientDh;
    setClientDh.writeConstructor(tg::SET_CLIENT_DH_PARAMS_ID);
    setClientDh.writeRaw(nonce, sizeof(nonce));
    setClientDh.writeRaw(resPq.serverNonce, sizeof(resPq.serverNonce));
    setClientDh.writeBytes(&encryptedClientDh[0], encryptedClientDh.size());
    sendHandshakeMessage(fd, setClientDh.data());

    readHandshakeMessage(fd, body);
    tg::TlReader genReader(body.empty() ? NULL : &body[0], body.size());
    if (genReader.readConstructor() != tg::DH_GEN_OK_ID) {
        throw std::runtime_error("server did not return dh_gen_ok");
    }
    uint8_t genNonce[16];
    uint8_t genServerNonce[16];
    uint8_t newNonceHash[16];
    genReader.readRaw(genNonce, sizeof(genNonce));
    genReader.readRaw(genServerNonce, sizeof(genServerNonce));
    genReader.readRaw(newNonceHash, sizeof(newNonceHash));
    if (std::memcmp(genNonce, nonce, sizeof(nonce)) != 0 ||
        std::memcmp(genServerNonce, resPq.serverNonce,
                    sizeof(resPq.serverNonce)) != 0) {
        throw std::runtime_error("dh_gen_ok nonce mismatch");
    }
    tg::checkDhGenOk(newNonce, authKey, newNonceHash);

    tg::Session session;
    std::memcpy(session.auth_key, authKey, sizeof(authKey));
    session.authKeyId = authKeyId;
    const std::vector<uint8_t> salt =
        tg::deriveServerSalt(newNonce, resPq.serverNonce);
    if (salt.size() != 8) {
        throw std::runtime_error("derived server salt is not 8 bytes");
    }
    session.salt = 0;
    for (std::size_t i = 0; i < salt.size(); ++i) {
        session.salt |= static_cast<uint64_t>(salt[i]) << (i * 8);
    }
    uint8_t sessionId[8];
    readRandom(sessionId, sizeof(sessionId));
    session.sessionId = readLe64(sessionId);
    if (session.sessionId == 0) {
        session.sessionId = 1;
    }
    return session;
}

void sendEncryptedPacket(int fd, const std::vector<uint8_t>& packet)
{
    if (packet.empty() || packet.size() > kMaximumPacketLength) {
        throw std::runtime_error("invalid encrypted packet size");
    }
    std::vector<uint8_t> length;
    appendLe32(length, static_cast<uint32_t>(packet.size()));
    sendAll(fd, &length[0], length.size(), "sending encrypted packet length");
    sendAll(fd, &packet[0], packet.size(), "sending encrypted packet");
}

std::vector<uint8_t> readEncryptedPacket(int fd)
{
    uint8_t lengthBytes[4];
    readExact(fd, lengthBytes, sizeof(lengthBytes),
              "reading encrypted packet length");
    const uint32_t length = readLe32(lengthBytes);
    if (length < 24 || length > kMaximumPacketLength) {
        throw std::runtime_error("invalid encrypted server packet length");
    }
    std::vector<uint8_t> packet(length);
    readExact(fd, &packet[0], packet.size(), "reading encrypted server packet");
    return packet;
}

uint64_t sendEncryptedMessage(int fd, tg::Session& session,
                              const std::vector<uint8_t>& body,
                              bool contentRelated,
                              const std::string& operation)
{
    const uint64_t msgId =
        tg::nextMsgId(session, static_cast<uint32_t>(std::time(NULL)));
    const uint32_t seqNo = tg::nextSeqNo(session, contentRelated);
    const std::size_t unpadded = 32 + body.size();
    std::size_t paddingLength = (16 - (unpadded & 15u)) & 15u;
    if (paddingLength < 12) {
        paddingLength += 16;
    }
    std::vector<uint8_t> padding(paddingLength);
    if (!padding.empty()) {
        readRandom(&padding[0], padding.size());
    }
    const std::vector<uint8_t> packet =
        tg::encryptMessage(session, msgId, seqNo, body,
                           padding.empty() ? NULL : &padding[0],
                           padding.size());
    try {
        sendEncryptedPacket(fd, packet);
    } catch (const std::exception& error) {
        throw std::runtime_error(operation + ": " + error.what());
    }
    return msgId;
}

std::vector<uint8_t> gunzip(const std::vector<uint8_t>& compressed)
{
    char inputPath[] = "/tmp/tg-login-gzip-in-XXXXXX";
    char outputPath[] = "/tmp/tg-login-gzip-out-XXXXXX";
    const int inputFd = mkstemp(inputPath);
    if (inputFd < 0) {
        throw std::runtime_error(std::string("creating gzip input file: ") +
                                 std::strerror(errno));
    }
    const int outputFd = mkstemp(outputPath);
    if (outputFd < 0) {
        const std::string error = std::strerror(errno);
        close(inputFd);
        unlink(inputPath);
        throw std::runtime_error("creating gzip output file: " + error);
    }

    std::size_t offset = 0;
    while (offset < compressed.size()) {
        const ssize_t count =
            write(inputFd, &compressed[offset], compressed.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            const std::string error = std::strerror(errno);
            close(inputFd);
            close(outputFd);
            unlink(inputPath);
            unlink(outputPath);
            throw std::runtime_error("writing gzip input: " + error);
        }
    }
    close(inputFd);
    close(outputFd);

    const pid_t child = fork();
    if (child < 0) {
        const std::string error = std::strerror(errno);
        unlink(inputPath);
        unlink(outputPath);
        throw std::runtime_error("starting gzip decoder: " + error);
    }
    if (child == 0) {
        const int in = open(inputPath, O_RDONLY);
        const int out = open(outputPath, O_WRONLY | O_TRUNC);
        if (in < 0 || out < 0 || dup2(in, STDIN_FILENO) < 0 ||
            dup2(out, STDOUT_FILENO) < 0) {
            _exit(126);
        }
        close(in);
        close(out);
        execlp("gzip", "gzip", "-dc", static_cast<char*>(NULL));
        _exit(127);
    }

    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            const std::string error = std::strerror(errno);
            unlink(inputPath);
            unlink(outputPath);
            throw std::runtime_error("waiting for gzip decoder: " + error);
        }
    }
    unlink(inputPath);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        unlink(outputPath);
        throw std::runtime_error(
            "gzip_packed payload could not be decompressed");
    }

    const int resultFd = open(outputPath, O_RDONLY);
    unlink(outputPath);
    if (resultFd < 0) {
        throw std::runtime_error(std::string("opening gzip result: ") +
                                 std::strerror(errno));
    }

    std::vector<uint8_t> result;
    uint8_t buffer[4096];
    for (;;) {
        const ssize_t count = read(resultFd, buffer, sizeof(buffer));
        if (count > 0) {
            if (result.size() + static_cast<std::size_t>(count) >
                kMaximumPacketLength) {
                close(resultFd);
                throw std::runtime_error("inflated gzip result is too large");
            }
            result.insert(result.end(), buffer, buffer + count);
        } else if (count == 0) {
            break;
        } else if (errno != EINTR) {
            const std::string error = std::strerror(errno);
            close(resultFd);
            throw std::runtime_error("reading gzip result: " + error);
        }
    }
    close(resultFd);
    return result;
}

std::vector<uint8_t> unwrapGzip(const std::vector<uint8_t>& input)
{
    std::vector<uint8_t> current = input;
    for (int depth = 0; depth < 4; ++depth) {
        if (current.size() < 4 || readLe32(&current[0]) != kGzipPackedId) {
            return current;
        }

        TlCursor reader(current);
        reader.readUInt32();
        current = gunzip(reader.readBytes());
    }
    if (current.size() >= 4 && readLe32(&current[0]) == kGzipPackedId) {
        throw std::runtime_error("gzip_packed nesting is too deep");
    }
    return current;
}

std::string readJsonString(const std::string& json, const std::string& key)
{
    const std::string quoted = "\"" + key + "\"";
    std::size_t pos = json.find(quoted);
    if (pos == std::string::npos) {
        throw std::runtime_error("telegram.json is missing " + quoted);
    }
    pos = json.find(':', pos + quoted.size());
    if (pos == std::string::npos) {
        throw std::runtime_error("invalid JSON value for " + quoted);
    }
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' ||
            json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        throw std::runtime_error(quoted + " must be a JSON string");
    }
    ++pos;

    std::string value;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') {
            return value;
        }
        if (c == '\\') {
            if (pos >= json.size()) {
                break;
            }
            const char escaped = json[pos++];
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default:
                throw std::runtime_error(
                    "unsupported JSON escape in " + quoted);
            }
        } else {
            value.push_back(c);
        }
    }
    throw std::runtime_error("unterminated JSON string for " + quoted);
}

int32_t readJsonApiId(const std::string& json)
{
    const std::string quoted = "\"api_id\"";
    std::size_t pos = json.find(quoted);
    if (pos == std::string::npos) {
        throw std::runtime_error("telegram.json is missing \"api_id\"");
    }
    pos = json.find(':', pos + quoted.size());
    if (pos == std::string::npos) {
        throw std::runtime_error("invalid api_id JSON value");
    }
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' ||
            json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') {
        throw std::runtime_error("api_id must be a positive integer");
    }

    uint64_t value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        const unsigned int digit =
            static_cast<unsigned int>(json[pos++] - '0');
        if (value >
            (static_cast<uint64_t>(std::numeric_limits<int32_t>::max()) -
             digit) / 10) {
            throw std::runtime_error("api_id is outside the int32 range");
        }
        value = value * 10 + digit;
    }
    if (value == 0) {
        throw std::runtime_error("api_id must be a positive integer");
    }
    return static_cast<int32_t>(value);
}

void loadSecrets(int32_t& apiId, std::string& apiHash)
{
    const char* paths[] = {
        "secrets/telegram.json",
        "apps/TgNative/secrets/telegram.json"
    };
    std::string contents;
    for (std::size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        FILE* file = std::fopen(paths[i], "rb");
        if (file == NULL) {
            continue;
        }
        char buffer[2048];
        for (;;) {
            const std::size_t count = std::fread(buffer, 1, sizeof(buffer), file);
            contents.append(buffer, count);
            if (count < sizeof(buffer)) {
                break;
            }
        }
        const bool failed = std::ferror(file) != 0;
        std::fclose(file);
        if (failed) {
            throw std::runtime_error("reading secrets/telegram.json failed");
        }
        break;
    }
    if (contents.empty()) {
        throw std::runtime_error(
            "cannot open secrets/telegram.json or "
            "apps/TgNative/secrets/telegram.json");
    }

    apiId = readJsonApiId(contents);
    apiHash = readJsonString(contents, "api_hash");
    if (apiHash.empty()) {
        throw std::runtime_error("api_hash in telegram.json is empty");
    }
}

std::vector<uint8_t> buildCodeSettings()
{
    tg::TlWriter writer;
    writer.writeConstructor(0xdebebe83u);
    writer.writeInt32(0);
    return writer.data();
}

std::vector<uint8_t> buildLayer214SendCode(const std::string& phone,
                                           int32_t apiId,
                                           const std::string& apiHash)
{
    tg::TlWriter writer;
    writer.writeConstructor(0xa677244fu);
    writer.writeString(phone);
    writer.writeInt32(apiId);
    writer.writeString(apiHash);
    const std::vector<uint8_t> settings = buildCodeSettings();
    writer.writeRaw(&settings[0], settings.size());
    return writer.data();
}

uint32_t authSignInConstructor()
{
    const char* schema =
        "auth.signIn flags:# phone_number:string phone_code_hash:string "
        "phone_code:flags.0?string email_verification:flags.1?EmailVerification "
        "= auth.Authorization";
    return tg::crc32(schema);
}

std::vector<uint8_t> buildLayer214SignIn(const std::string& phone,
                                         const std::string& phoneCodeHash,
                                         const std::string& phoneCode)
{
    tg::TlWriter writer;
    writer.writeConstructor(authSignInConstructor());
    writer.writeInt32(1);
    writer.writeString(phone);
    writer.writeString(phoneCodeHash);
    writer.writeString(phoneCode);
    return writer.data();
}

std::vector<uint8_t> buildLayer214Query(int32_t apiId,
                                        const std::vector<uint8_t>& query)
{
    const std::vector<uint8_t> init =
        tg::buildInitConnection(apiId, "PC probe", "Linux", "0.1",
                                "en", "tdesktop", "en", query);
    return tg::buildInvokeWithLayer(214, init);
}

uint32_t constructorOf(const std::vector<uint8_t>& body)
{
    if (body.size() < 4) {
        throw std::runtime_error("TL object is shorter than a constructor");
    }
    return readLe32(&body[0]);
}

void printConstructor(uint32_t id)
{
    std::cout << "constructor 0x" << std::hex << id << std::dec << std::endl;
}

void printHexDump(const char* label, const std::vector<uint8_t>& bytes,
                  std::ostream& output)
{
    output << label << " (" << bytes.size() << " bytes):";
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if ((i & 15u) == 0) {
            output << std::endl << "  ";
        } else {
            output << ' ';
        }
        output << std::hex;
        const unsigned int value = bytes[i];
        if (value < 16) {
            output << '0';
        }
        output << value << std::dec;
    }
    output << std::endl;
}

int32_t parseConfigThisDc(const std::vector<uint8_t>& config)
{
    TlCursor reader(config);
    if (reader.readUInt32() != 0xcc1a241eu) {
        throw std::runtime_error("help.getConfig did not return config#cc1a241e");
    }

    reader.readInt32();
    reader.readInt32();
    reader.readInt32();

    const uint32_t testMode = reader.readUInt32();
    if (testMode != tg::kBoolTrue && testMode != tg::kBoolFalse) {
        throw std::runtime_error("config contains an invalid test_mode Bool");
    }
    if (testMode != tg::kBoolTrue) {
        throw std::runtime_error("Telegram endpoint did not report test mode");
    }

    const int32_t thisDc = reader.readInt32();
    const std::size_t dcOptionCount = reader.readVectorCount();
    std::cout << "help.getConfig this_dc=" << thisDc
              << ", dc_options=" << dcOptionCount << std::endl;
    return thisDc;
}

struct SentCodeInfo {
    std::string phoneCodeHash;
    uint32_t typeId;
    int32_t length;
};

SentCodeInfo parseSentCode(const std::vector<uint8_t>& result)
{
    TlCursor reader(result);
    if (reader.readUInt32() != 0x5e002502u) {
        throw std::runtime_error("auth.sendCode did not return auth.sentCode");
    }

    const int32_t flags = reader.readInt32();
    SentCodeInfo info;
    info.typeId = reader.readUInt32();
    info.length = -1;

    switch (info.typeId) {
    case 0x3dbb5986u:
    case 0xc000bba2u:
    case 0x5353e5a7u:
        info.length = reader.readInt32();
        break;
    case 0x82006484u:
        reader.readString();
        info.length = reader.readInt32();
        break;
    default:
        std::cout << "unknown type 0x" << std::hex << info.typeId
                  << std::dec << std::endl;
        throw std::runtime_error(
            "auth.sentCode contains an unsupported SentCodeType");
    }

    info.phoneCodeHash = reader.readString();
    if (info.phoneCodeHash.empty()) {
        throw std::runtime_error(
            "auth.sentCode returned an empty phone_code_hash");
    }

    if (flags & (1 << 1)) {
        reader.readUInt32();
    }
    if (flags & (1 << 2)) {
        reader.readInt32();
    }
    if (reader.remaining() != 0) {
        throw std::runtime_error(
            "auth.sentCode has unexpected trailing data after optional fields");
    }
    return info;
}

struct WaitResult {
    bool received;
    bool badServerSalt;
    std::vector<uint8_t> result;

    WaitResult() : received(false), badServerSalt(false) {}
};

void processBody(int fd, tg::Session& session,
                 const std::vector<uint8_t>& body,
                 uint64_t requestId, uint64_t pingId, bool expectPong,
                 WaitResult& result, int depth);

void processMessage(int fd, tg::Session& session, uint64_t msgId,
                    uint32_t seqNo, const std::vector<uint8_t>& body,
                    uint64_t requestId, uint64_t pingId, bool expectPong,
                    WaitResult& result, int depth)
{
    if (depth > 8) {
        throw std::runtime_error("server message nesting is too deep");
    }
    if (seqNo & 1u) {
        const std::vector<uint64_t> ids(1, msgId);
        sendEncryptedMessage(fd, session, tg::buildMsgsAck(ids), false,
                             "sending msgs_ack");
    }
    processBody(fd, session, body, requestId, pingId, expectPong, result,
                depth);
}

void throwRpcError(int32_t code, const std::string& message)
{
    throw std::runtime_error("RPC error " + std::to_string(code) + ": " +
                             message);
}

void processBody(int fd, tg::Session& session,
                 const std::vector<uint8_t>& body,
                 uint64_t requestId, uint64_t pingId, bool expectPong,
                 WaitResult& result, int depth)
{
    if (depth > 8) {
        throw std::runtime_error("server message nesting is too deep");
    }
    if (body.size() < 4) {
        throw std::runtime_error(
            "server TL object is shorter than a constructor");
    }

    TlCursor reader(body);
    const uint32_t constructor = reader.readUInt32();

    if (constructor == kMsgContainerId) {
        const int32_t signedCount = reader.readInt32();
        if (signedCount < 0 || signedCount > 1000000) {
            throw std::runtime_error("invalid msg_container message count");
        }
        const std::size_t count = static_cast<std::size_t>(signedCount);
        for (std::size_t i = 0; i < count; ++i) {
            const uint64_t childMsgId = reader.readUInt64();
            const uint32_t childSeqNo = reader.readUInt32();
            const std::vector<uint8_t> childBody = reader.readMessageBody();
            processMessage(fd, session, childMsgId, childSeqNo, childBody,
                           requestId, pingId, expectPong, result, depth + 1);
        }
        if (reader.remaining() != 0) {
            throw std::runtime_error("trailing data in msg_container");
        }
        return;
    }

    if (constructor == kGzipPackedId) {
        const std::vector<uint8_t> expanded = gunzip(reader.readBytes());
        processBody(fd, session, expanded, requestId, pingId, expectPong,
                    result, depth + 1);
        return;
    }

    if (constructor == kRpcResultId) {
        const uint64_t responseTo = reader.readUInt64();
        if (responseTo != requestId) {
            return;
        }

        result.result = unwrapGzip(reader.readRemaining());
        if (result.result.size() < 4) {
            throw std::runtime_error("rpc_result contains an empty TL object");
        }
        if (constructorOf(result.result) == kRpcErrorId) {
            TlCursor errorReader(result.result);
            errorReader.readUInt32();
            const int32_t code = errorReader.readInt32();
            const std::string message = errorReader.readString();
            throwRpcError(code, message);
        }
        result.received = true;
        return;
    }

    if (constructor == kRpcErrorId) {
        const int32_t code = reader.readInt32();
        const std::string message = reader.readString();
        throwRpcError(code, message);
    }

    if (constructor == kPongId) {
        const uint64_t responseTo = reader.readUInt64();
        const uint64_t receivedPingId = reader.readUInt64();
        if (expectPong && responseTo == requestId &&
            receivedPingId == pingId) {
            result.received = true;
        }
        return;
    }

    if (constructor == kMsgsAckId) {
        const std::size_t count = reader.readVectorCount();
        for (std::size_t i = 0; i < count; ++i) {
            reader.readUInt64();
        }
        return;
    }

    if (constructor == kNewSessionCreatedId) {
        reader.readUInt64();
        reader.readUInt64();
        session.salt = reader.readUInt64();
        return;
    }

    if (constructor == kBadServerSaltId) {
        const uint64_t badMsgId = reader.readUInt64();
        reader.readInt32();
        reader.readInt32();
        session.salt = reader.readUInt64();
        if (badMsgId == requestId) {
            result.badServerSalt = true;
        }
        return;
    }

    if (constructor == kBadMsgNotificationId) {
        const uint64_t badMsgId = reader.readUInt64();
        reader.readInt32();
        const int32_t errorCode = reader.readInt32();
        std::cerr << "bad_msg_notification code " << errorCode
                  << " for message " << badMsgId << std::endl;
        throw std::runtime_error("server rejected the MTProto message");
    }

    if (constructor == 0xae500895u) {
        return;
    }
}

WaitResult waitForResponse(int fd, tg::Session& session, uint64_t requestId,
                           uint64_t pingId, bool expectPong)
{
    WaitResult result;
    while (!result.received && !result.badServerSalt) {
        const std::vector<uint8_t> packet = readEncryptedPacket(fd);
        tg::DecryptedMsg message;
        if (!tg::decryptMessage(session, packet, message)) {
            throw std::runtime_error(
                "server packet failed MTProto authentication");
        }
        processMessage(fd, session, message.msgId, message.seqNo, message.body,
                       requestId, pingId, expectPong, result, 0);
    }
    return result;
}

std::vector<uint8_t> performRpc(int fd, tg::Session& session,
                                const std::vector<uint8_t>& body,
                                uint64_t pingId, bool expectPong,
                                const std::string& operation)
{
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint64_t requestId =
            sendEncryptedMessage(fd, session, body, true, operation);
        const WaitResult response =
            waitForResponse(fd, session, requestId, pingId, expectPong);
        if (!response.badServerSalt) {
            if (!response.received) {
                throw std::runtime_error(operation +
                                         ": no matching server response");
            }
            return response.result;
        }
    }
    throw std::runtime_error(operation +
                             ": server repeatedly rejected the salt");
}

std::string testPhoneForDc(int32_t thisDc)
{
    if (thisDc < 0 || thisDc > 9) {
        throw std::runtime_error(
            "test DC id cannot be used as a single-digit test phone suffix");
    }
    return std::string("99966") + static_cast<char>('0' + thisDc) + "1234";
}

std::string testCodeForDc(int32_t thisDc)
{
    if (thisDc < 0 || thisDc > 9) {
        throw std::runtime_error(
            "test DC id cannot be used as a single-digit test code");
    }
    return std::string(5, static_cast<char>('0' + thisDc));
}

bool runProbe(int fd, tg::Session& session)
{
    try {
        uint8_t randomPing[8];
        readRandom(randomPing, sizeof(randomPing));
        const uint64_t pingId = readLe64(randomPing);
        performRpc(fd, session, tg::buildPing(pingId), pingId, true, "ping");
        std::cout << "STEP 1 OK: ping -> pong" << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "STEP 1 FAIL: " << error.what() << std::endl;
        return false;
    }

    int32_t apiId = 0;
    std::string apiHash;
    try {
        loadSecrets(apiId, apiHash);
    } catch (const std::exception& error) {
        std::cerr << "STEP 2 FAIL: loading API credentials: "
                  << error.what() << std::endl;
        return false;
    }

    std::vector<uint8_t> config;
    int32_t thisDc = -1;
    try {
        const std::vector<uint8_t> query =
            buildLayer214Query(apiId, tg::buildHelpGetConfig());
        config = performRpc(fd, session, query, 0, false,
                            "invokeWithLayer/initConnection/help.getConfig");
        thisDc = parseConfigThisDc(config);
        std::cout << "STEP 2 OK: initConnection/help.getConfig returned config"
                  << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "STEP 2 FAIL: " << error.what() << std::endl;
        return false;
    }

    std::string phone;
    std::string phoneCode;
    std::string phoneCodeHash;
    try {
        phone = testPhoneForDc(thisDc);
        phoneCode = testCodeForDc(thisDc);
        std::cout << "Using test DC " << thisDc
                  << " test phone/code derived from this_dc" << std::endl;

        const std::vector<uint8_t> sendCode =
            buildLayer214SendCode(phone, apiId, apiHash);
        const std::vector<uint8_t> query =
            buildLayer214Query(apiId, sendCode);
        const std::vector<uint8_t> response =
            performRpc(fd, session, query, 0, false,
                       "invokeWithLayer/initConnection/auth.sendCode");

        printConstructor(constructorOf(response));
        printHexDump("auth.sentCode", response, std::cerr);
        const SentCodeInfo sentCode = parseSentCode(response);
        phoneCodeHash = sentCode.phoneCodeHash;
        std::cout << "sentCode type 0x" << std::hex << sentCode.typeId
                  << std::dec << ", length " << sentCode.length << std::endl;
        std::cout << "STEP 3 OK: auth.sendCode returned auth.sentCode"
                  << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "STEP 3 FAIL: " << error.what() << std::endl;
        return false;
    }

    std::vector<uint8_t> signIn;
    try {
        signIn = buildLayer214SignIn(phone, phoneCodeHash, phoneCode);
        const uint32_t signInId = authSignInConstructor();
        std::cout << "auth.signIn constructor 0x" << std::hex << signInId
                  << std::dec << std::endl;

        const std::vector<uint8_t> response =
            performRpc(fd, session, signIn, 0, false, "auth.signIn");
        const uint32_t constructor = constructorOf(response);
        if (constructor == 0x2ea2c0d4u) {
            std::cout << "auth.authorization#2ea2c0d4" << std::endl;
        } else if (constructor == 0x44747e9au) {
            std::cout << "auth.authorizationSignUpRequired#44747e9a"
                      << std::endl;
        } else {
            printConstructor(constructor);
            throw std::runtime_error(
                "auth.signIn returned neither authorization nor signup-required");
        }
        std::cout << "STEP 4 OK: auth.signIn accepted the test code"
                  << std::endl;
    } catch (const std::exception& error) {
        std::cerr << "STEP 4 FAIL: " << error.what() << std::endl;
        if (!signIn.empty()) {
            printHexDump("auth.signIn body", signIn, std::cerr);
        }
        return false;
    }

    return true;
}

} // namespace

int main()
{
    try {
        SocketHandle socketHandle = connectToTestDc();
        tg::Session session = establishAuthKey(socketHandle.get());
        std::cout << "AUTH KEY OK" << std::endl;
        return runProbe(socketHandle.get(), session) ? 0 : 1;
    } catch (const std::exception& error) {
        std::cerr << "LOGIN PROBE FAIL: " << error.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "LOGIN PROBE FAIL: unknown error" << std::endl;
        return 1;
    }
}
