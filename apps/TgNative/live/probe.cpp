#include "core/crypto.h"
#include "core/mtproto_auth.h"
#include "core/mtproto_dh.h"
#include "core/tl.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

const int kTimeoutMilliseconds = 10000;
const uint32_t kMaximumPacketLength = 16u * 1024u * 1024u;
const uint32_t kReqDhParamsId = 0xd712e4beu;
const int kTestDcField = 10002; // Test DC N uses field 10000 + N.

uint64_t lastMessageId = 0;
uint64_t minimumMessageId = 0;

void appendLe32(std::vector<uint8_t>& output, uint32_t value)
{
    output.push_back(static_cast<uint8_t>(value));
    output.push_back(static_cast<uint8_t>(value >> 8));
    output.push_back(static_cast<uint8_t>(value >> 16));
    output.push_back(static_cast<uint8_t>(value >> 24));
}

void appendLe64(std::vector<uint8_t>& output, uint64_t value)
{
    unsigned int i;
    for (i = 0; i < 8; ++i) {
        output.push_back(static_cast<uint8_t>(value >> (i * 8)));
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
    unsigned int i;
    for (i = 0; i < 8; ++i) {
        value |= static_cast<uint64_t>(data[i]) << (i * 8);
    }
    return value;
}

long long monotonicMilliseconds()
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        throw std::runtime_error(std::string("clock_gettime failed: ") +
                                 std::strerror(errno));
    }

    return static_cast<long long>(now.tv_sec) * 1000LL +
           static_cast<long long>(now.tv_nsec / 1000000L);
}

void waitForSocket(int socketFd, short events, long long deadline,
                   const std::string& operation)
{
    for (;;) {
        const long long remaining = deadline - monotonicMilliseconds();
        if (remaining <= 0) {
            throw std::runtime_error(operation + " timed out");
        }

        int timeout = remaining > 2147483647LL
                          ? 2147483647
                          : static_cast<int>(remaining);
        if (timeout == 0) {
            timeout = 1;
        }

        struct pollfd descriptor;
        descriptor.fd = socketFd;
        descriptor.events = events;
        descriptor.revents = 0;

        const int result = poll(&descriptor, 1, timeout);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(operation + " poll failed: " +
                                     std::strerror(errno));
        }
        if (result == 0) {
            throw std::runtime_error(operation + " timed out");
        }
        if (descriptor.revents & POLLNVAL) {
            throw std::runtime_error(operation + ": invalid socket");
        }

        if (descriptor.revents & POLLERR) {
            int socketError = 0;
            socklen_t errorLength = sizeof(socketError);
            if (getsockopt(socketFd, SOL_SOCKET, SO_ERROR, &socketError,
                           &errorLength) == 0 &&
                socketError != 0) {
                throw std::runtime_error(operation + " socket error: " +
                                         std::strerror(socketError));
            }
        }

        if ((descriptor.revents & events) ||
            (events == POLLIN && (descriptor.revents & POLLHUP))) {
            return;
        }
    }
}

class SocketHandle {
public:
    explicit SocketHandle(int value) : value_(value) {}
    SocketHandle(SocketHandle&& other) noexcept : value_(other.value_)
    {
        other.value_ = -1;
    }

    ~SocketHandle()
    {
        if (value_ >= 0) {
            close(value_);
        }
    }

    int get() const { return value_; }

private:
    SocketHandle(const SocketHandle&);
    SocketHandle& operator=(const SocketHandle&);

    int value_;
};

SocketHandle connectToTestDc()
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error(std::string("socket failed: ") +
                                 std::strerror(errno));
    }

    SocketHandle socketHandle(fd);
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        throw std::runtime_error(std::string("could not set nonblocking socket: ") +
                                 std::strerror(errno));
    }

    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(443);
    if (inet_pton(AF_INET, "149.154.167.40", &address.sin_addr) != 1) {
        throw std::runtime_error("could not parse the test DC address");
    }

    const long long deadline =
        monotonicMilliseconds() + kTimeoutMilliseconds;
    const int result = connect(fd, reinterpret_cast<struct sockaddr*>(&address),
                               sizeof(address));
    if (result < 0 && errno != EINPROGRESS && errno != EINTR) {
        throw std::runtime_error(std::string("connect failed: ") +
                                 std::strerror(errno));
    }

    if (result < 0) {
        waitForSocket(fd, POLLOUT, deadline, "connect");
        int socketError = 0;
        socklen_t errorLength = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError,
                       &errorLength) < 0) {
            throw std::runtime_error(std::string("connect status failed: ") +
                                     std::strerror(errno));
        }
        if (socketError != 0) {
            throw std::runtime_error(std::string("connect failed: ") +
                                     std::strerror(socketError));
        }
    }

    return socketHandle;
}

void sendAll(int socketFd, const uint8_t* data, std::size_t length,
             const std::string& operation)
{
    std::size_t sent = 0;
    const long long deadline =
        monotonicMilliseconds() + kTimeoutMilliseconds;

    while (sent < length) {
        waitForSocket(socketFd, POLLOUT, deadline, operation);
        const ssize_t result =
            send(socketFd, data + sent, length - sent, 0);
        if (result > 0) {
            sent += static_cast<std::size_t>(result);
        } else if (result == 0) {
            throw std::runtime_error(operation + ": socket closed while sending");
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            throw std::runtime_error(operation + " failed: " +
                                     std::strerror(errno));
        }
    }
}

void readExactAppend(int socketFd, std::vector<uint8_t>& output,
                     std::size_t length, const std::string& operation)
{
    std::size_t received = 0;
    const long long deadline =
        monotonicMilliseconds() + kTimeoutMilliseconds;
    uint8_t buffer[4096];

    while (received < length) {
        waitForSocket(socketFd, POLLIN, deadline, operation);
        std::size_t wanted = length - received;
        if (wanted > sizeof(buffer)) {
            wanted = sizeof(buffer);
        }

        const ssize_t result = recv(socketFd, buffer, wanted, 0);
        if (result > 0) {
            const std::size_t count = static_cast<std::size_t>(result);
            output.insert(output.end(), buffer, buffer + count);
            received += count;
        } else if (result == 0) {
            throw std::runtime_error(operation + ": socket closed before the answer was complete");
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            throw std::runtime_error(operation + " failed: " +
                                     std::strerror(errno));
        }
    }
}

void readRandomBytes(uint8_t* output, std::size_t length)
{
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        throw std::runtime_error(std::string("could not open /dev/urandom: ") +
                                 std::strerror(errno));
    }

    std::size_t offset = 0;
    while (offset < length) {
        const ssize_t result = read(fd, output + offset, length - offset);
        if (result > 0) {
            offset += static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
            continue;
        } else {
            const std::string reason =
                result == 0
                    ? "unexpected end of /dev/urandom"
                    : std::string("could not read /dev/urandom: ") +
                          std::strerror(errno);
            close(fd);
            throw std::runtime_error(reason);
        }
    }

    close(fd);
}

void observeServerMessageId(uint64_t serverMessageId)
{
    const uint64_t alignedServerId = serverMessageId & ~UINT64_C(3);
    if (alignedServerId > UINT64_MAX - 4) {
        throw std::runtime_error("server message id overflow");
    }

    const uint64_t nextServerTimeId = alignedServerId + 4;
    if (nextServerTimeId > minimumMessageId) {
        minimumMessageId = nextServerTimeId;
    }
}

uint64_t makeMessageId()
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        throw std::runtime_error(std::string("clock_gettime failed: ") +
                                 std::strerror(errno));
    }
    if (now.tv_sec <= 0 ||
        static_cast<uint64_t>(now.tv_sec) > UINT64_C(0xffffffff)) {
        throw std::runtime_error("current Unix time is outside the MTProto message-id range");
    }

    const uint64_t seconds = static_cast<uint64_t>(now.tv_sec);
    const uint64_t fractional =
        (static_cast<uint64_t>(now.tv_nsec) << 32) / UINT64_C(1000000000);
    uint64_t messageId = (seconds << 32) | fractional;
    messageId &= ~UINT64_C(3);

    if (messageId < minimumMessageId) {
        messageId = minimumMessageId;
    }
    if (messageId <= lastMessageId) {
        if (lastMessageId > UINT64_MAX - 4) {
            throw std::runtime_error("message id overflow");
        }
        messageId = (lastMessageId + 4) & ~UINT64_C(3);
    }

    lastMessageId = messageId;
    return messageId;
}

std::vector<uint8_t> decodeHex(const std::string& hex)
{
    if ((hex.size() & 1u) != 0) {
        throw std::runtime_error("test RSA modulus has an odd hex length");
    }

    std::vector<uint8_t> result;
    result.reserve(hex.size() / 2);

    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const char high = hex[i];
        const char low = hex[i + 1];
        const int highValue =
            high >= '0' && high <= '9' ? high - '0' :
            high >= 'a' && high <= 'f' ? high - 'a' + 10 :
            high >= 'A' && high <= 'F' ? high - 'A' + 10 : -1;
        const int lowValue =
            low >= '0' && low <= '9' ? low - '0' :
            low >= 'a' && low <= 'f' ? low - 'a' + 10 :
            low >= 'A' && low <= 'F' ? low - 'A' + 10 : -1;

        if (highValue < 0 || lowValue < 0) {
            throw std::runtime_error("test RSA modulus contains invalid hex");
        }
        result.push_back(
            static_cast<uint8_t>((highValue << 4) | lowValue));
    }

    return result;
}

std::string toDecimalBigEndian(const std::vector<uint8_t>& bytes)
{
    const uint32_t base = 1000000000u;
    std::vector<uint32_t> digits(1, 0);

    for (std::size_t byteIndex = 0; byteIndex < bytes.size(); ++byteIndex) {
        uint64_t carry = bytes[byteIndex];
        for (std::size_t digitIndex = 0; digitIndex < digits.size();
             ++digitIndex) {
            const uint64_t value =
                static_cast<uint64_t>(digits[digitIndex]) * 256u + carry;
            digits[digitIndex] = static_cast<uint32_t>(value % base);
            carry = value / base;
        }
        while (carry != 0) {
            digits.push_back(static_cast<uint32_t>(carry % base));
            carry /= base;
        }
    }

    std::string result;
    char buffer[16];
    std::size_t index = digits.size();
    --index;
    const int firstLength = std::snprintf(
        buffer, sizeof(buffer), "%u", static_cast<unsigned int>(digits[index]));
    if (firstLength > 0) {
        result.append(buffer, static_cast<std::size_t>(firstLength));
    }

    while (index > 0) {
        --index;
        const int partLength = std::snprintf(
            buffer, sizeof(buffer), "%09u",
            static_cast<unsigned int>(digits[index]));
        if (partLength > 0) {
            result.append(buffer, static_cast<std::size_t>(partLength));
        }
    }

    return result;
}

uint64_t bigEndianToUint64(const std::vector<uint8_t>& bytes,
                           const std::string& name)
{
    if (bytes.empty() || bytes.size() > 8) {
        throw std::runtime_error(name + " does not fit in an unsigned 64-bit integer");
    }

    uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        value = (value << 8) | bytes[i];
    }
    return value;
}

std::vector<uint8_t> minimalBigEndian(uint64_t value)
{
    std::vector<uint8_t> bytes;
    while (value != 0) {
        bytes.push_back(static_cast<uint8_t>(value & 0xffu));
        value >>= 8;
    }
    if (bytes.empty()) {
        bytes.push_back(0);
    }
    std::reverse(bytes.begin(), bytes.end());
    return bytes;
}

uint64_t readMtprotoMessage(int socketFd, std::vector<uint8_t>& answer,
                            std::vector<uint8_t>& body,
                            const std::string& operation)
{
    answer.clear();
    body.clear();

    std::vector<uint8_t> lengthBytes;
    readExactAppend(socketFd, lengthBytes, 4, operation + ": reading answer length");
    const uint32_t answerLength = readLe32(&lengthBytes[0]);
    if (answerLength < 20) {
        throw std::runtime_error("answer payload is shorter than the MTProto envelope");
    }
    if (answerLength > kMaximumPacketLength) {
        throw std::runtime_error("answer payload exceeds the allowed size");
    }

    readExactAppend(socketFd, answer, answerLength,
                    operation + ": reading answer payload");

    const uint64_t authKeyId = readLe64(&answer[0]);
    if (authKeyId != 0) {
        throw std::runtime_error("answer auth_key_id is not zero");
    }

    const uint32_t bodyLength = readLe32(&answer[16]);
    if (bodyLength != answer.size() - 20) {
        throw std::runtime_error("answer message_length does not match its payload");
    }

    body.assign(answer.begin() + 20, answer.end());
    return readLe64(&answer[8]);
}

void sendMtprotoMessage(int socketFd, const std::vector<uint8_t>& body,
                        const std::string& operation,
                        bool hasDhParameterLengths = false,
                        std::size_t pLength = 0,
                        std::size_t qLength = 0,
                        std::size_t encryptedDataLength = 0)
{
    if (body.size() > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("MTProto message body is too large");
    }

    const uint64_t messageId = makeMessageId();
    std::vector<uint8_t> payload;
    payload.reserve(20 + body.size());
    appendLe64(payload, 0);
    appendLe64(payload, messageId);
    appendLe32(payload, static_cast<uint32_t>(body.size()));
    payload.insert(payload.end(), body.begin(), body.end());

    if (payload.size() > kMaximumPacketLength) {
        throw std::runtime_error("MTProto packet exceeds the allowed size");
    }

    std::cout << operation << " request header:"
              << " auth_key_id=0x0000000000000000"
              << " message_id=0x" << std::hex << std::setw(16)
              << std::setfill('0') << messageId << std::dec
              << std::setfill(' ')
              << " message_length=" << body.size()
              << " total_packet_length=" << (payload.size() + 4)
              << std::endl;

    const std::size_t shown = body.size() < 32 ? body.size() : 32;
    std::cout << operation << " body first " << shown << " bytes:";
    for (std::size_t i = 0; i < shown; ++i) {
        std::cout << ' ' << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(body[i]);
    }
    std::cout << std::dec << std::setfill(' ') << std::endl;

    if (hasDhParameterLengths) {
        std::cout << "req_DH_params lengths: p=" << pLength
                  << " q=" << qLength
                  << " encrypted_data=" << encryptedDataLength << std::endl;
    }

    std::vector<uint8_t> length;
    length.reserve(4);
    appendLe32(length, static_cast<uint32_t>(payload.size()));
    sendAll(socketFd, &length[0], length.size(),
            operation + ": sending intermediate transport length");
    sendAll(socketFd, &payload[0], payload.size(),
            operation + ": sending MTProto message");
}

std::string constructorDescription(uint32_t constructor)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08x",
                  static_cast<unsigned int>(constructor));
    return buffer;
}

void expectNonce(const uint8_t* value, const uint8_t* expected,
                 std::size_t expectedLength, const std::string& name)
{
    if (std::memcmp(value, expected, expectedLength) != 0) {
        throw std::runtime_error(name + " does not match the request");
    }
}

int fail(const std::string& step, const std::string& reason,
         const std::vector<uint8_t>& answer)
{
    std::cerr << "PROBE FAIL: " << step << ": " << reason << std::endl;

    const std::size_t shown = answer.size() < 64 ? answer.size() : 64;
    std::cerr << "raw answer (first " << shown << " bytes, total "
              << answer.size() << " bytes):";
    for (std::size_t i = 0; i < shown; ++i) {
        std::cerr << ' ' << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(answer[i]);
    }
    std::cerr << std::dec << std::setfill(' ') << std::endl;
    return 1;
}

} // namespace

int main()
{
    std::signal(SIGPIPE, SIG_IGN);

    std::vector<uint8_t> answer;
    std::string currentStep = "initialization";

    try {
        uint8_t nonce[16];
        readRandomBytes(nonce, sizeof(nonce));

        currentStep = "req_pq_multi";
        const std::vector<uint8_t> reqPqBody = tg::buildReqPqMulti(nonce);
        SocketHandle socketHandle = connectToTestDc();

        const uint8_t transportInit[4] = {0xee, 0xee, 0xee, 0xee};
        sendAll(socketHandle.get(), transportInit, sizeof(transportInit),
                "sending intermediate transport init");
        sendMtprotoMessage(socketHandle.get(), reqPqBody, "req_pq_multi");

        std::vector<uint8_t> body;
        const uint64_t resPqMessageId =
            readMtprotoMessage(socketHandle.get(), answer, body, "resPQ");

        currentStep = "resPQ";
        const tg::ResPQ response =
            tg::parseResPQ(body.empty() ? NULL : &body[0], body.size());

        const bool nonceMatches =
            std::memcmp(response.nonce, nonce, sizeof(nonce)) == 0;

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
            throw std::runtime_error("test RSA modulus is not 256 bytes");
        }

        const uint64_t testFingerprint =
            tg::rsaFingerprint(&modulus[0], modulus.size(), 65537);
        bool fingerprintFound = false;
        for (std::size_t i = 0; i < response.fingerprints.size(); ++i) {
            if (response.fingerprints[i] == testFingerprint) {
                fingerprintFound = true;
            }
        }

        std::cout << "pq: " << toDecimalBigEndian(response.pq) << std::endl;
        std::cout << "fingerprints: " << response.fingerprints.size()
                  << std::endl;
        for (std::size_t i = 0; i < response.fingerprints.size(); ++i) {
            std::cout << "  0x" << std::hex << std::setw(16)
                      << std::setfill('0') << response.fingerprints[i]
                      << std::dec << std::setfill(' ') << std::endl;
        }
        std::cout << "test-key fingerprint: "
                  << (fingerprintFound ? "present" : "absent")
                  << " (0x" << std::hex << std::setw(16) << std::setfill('0')
                  << testFingerprint << std::dec << std::setfill(' ') << ')'
                  << std::endl;

        if (!nonceMatches || !fingerprintFound) {
            std::string reason;
            if (!nonceMatches) {
                reason = "nonce does not match the request";
            }
            if (!fingerprintFound) {
                if (!reason.empty()) {
                    reason += "; ";
                }
                reason += "test-key fingerprint is not in the server list";
            }
            throw std::runtime_error(reason);
        }

        observeServerMessageId(resPqMessageId);

        currentStep = "factor pq";
        const uint64_t pqValue = bigEndianToUint64(response.pq, "pq");
        uint64_t pValue = 0;
        uint64_t qValue = 0;
        tg::factorPq(pqValue, pValue, qValue);
        if (pValue == 0 || qValue == 0) {
            throw std::runtime_error("factorization returned a zero factor");
        }
        if (pValue > qValue) {
            const uint64_t temporary = pValue;
            pValue = qValue;
            qValue = temporary;
        }
        if (pValue > std::numeric_limits<uint64_t>::max() / qValue ||
            pValue * qValue != pqValue) {
            throw std::runtime_error("p*q does not equal pq");
        }

        const std::vector<uint8_t> p = minimalBigEndian(pValue);
        const std::vector<uint8_t> q = minimalBigEndian(qValue);
        uint8_t newNonce[32];
        readRandomBytes(newNonce, sizeof(newNonce));

        currentStep = "p_q_inner_data_dc";
        const std::vector<uint8_t> innerData =
            tg::buildPqInnerDataDc(response.pq, p, q, nonce,
                                   response.serverNonce, newNonce, kTestDcField);
        if (innerData.size() > 255u - 20u) {
            throw std::runtime_error("p_q_inner_data_dc is too large for the test RSA key");
        }

        std::cout << "OURS_PQ_INNER ";
        for (std::size_t i = 0; i < innerData.size(); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(innerData[i]);
        }
        std::cout << " OURS_P ";
        for (std::size_t i = 0; i < p.size(); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(p[i]);
        }
        std::cout << " OURS_Q ";
        for (std::size_t i = 0; i < q.size(); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(q[i]);
        }
        std::cout << std::dec << std::setfill(' ') << std::endl;

        std::vector<uint8_t> rsaRandom(256);
        readRandomBytes(&rsaRandom[0], rsaRandom.size());
        const std::vector<uint8_t> encryptedData =
            tg::rsaPadAndEncrypt(&innerData[0], innerData.size(),
                                 &modulus[0], modulus.size(), 65537,
                                 &rsaRandom[0], rsaRandom.size());
        if (encryptedData.size() != 256) {
            throw std::runtime_error("encrypted_data is not exactly one 256-byte RSA block");
        }

        currentStep = "req_DH_params";
        tg::TlWriter reqDhWriter;
        reqDhWriter.writeConstructor(kReqDhParamsId);
        reqDhWriter.writeRaw(nonce, sizeof(nonce));
        reqDhWriter.writeRaw(response.serverNonce, sizeof(response.serverNonce));
        reqDhWriter.writeBytes(p.empty() ? NULL : &p[0], p.size());
        reqDhWriter.writeBytes(q.empty() ? NULL : &q[0], q.size());

        int64_t signedFingerprint = 0;
        std::memcpy(&signedFingerprint, &testFingerprint,
                    sizeof(signedFingerprint));
        reqDhWriter.writeInt64(signedFingerprint);
        reqDhWriter.writeBytes(encryptedData.empty() ? NULL : &encryptedData[0],
                               encryptedData.size());
        sendMtprotoMessage(socketHandle.get(), reqDhWriter.data(),
                           "req_DH_params", true, p.size(), q.size(),
                           encryptedData.size());

        readMtprotoMessage(socketHandle.get(), answer, body,
                           "server_DH_params_ok");

        currentStep = "server_DH_params_ok";
        tg::TlReader dhParamsReader(body.empty() ? NULL : &body[0],
                                    body.size());
        const uint32_t dhParamsConstructor = dhParamsReader.readConstructor();
        if (dhParamsConstructor != tg::SERVER_DH_PARAMS_OK_ID) {
            throw std::runtime_error(
                "expected server_DH_params_ok, received " +
                constructorDescription(dhParamsConstructor));
        }

        uint8_t dhNonce[16];
        uint8_t dhServerNonce[16];
        dhParamsReader.readRaw(dhNonce, sizeof(dhNonce));
        dhParamsReader.readRaw(dhServerNonce, sizeof(dhServerNonce));
        const std::string encryptedAnswer = dhParamsReader.readBytes();
        expectNonce(dhNonce, nonce, sizeof(nonce),
                    "server_DH_params_ok nonce");
        expectNonce(dhServerNonce, response.serverNonce,
                    sizeof(response.serverNonce),
                    "server_DH_params_ok server_nonce");
        if (encryptedAnswer.empty()) {
            throw std::runtime_error("encrypted_answer is empty");
        }

        std::cout << "DBG_NONCE ";
        for (std::size_t i = 0; i < sizeof(nonce); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(nonce[i]);
        }
        std::cout << " DBG_SERVER_NONCE ";
        for (std::size_t i = 0; i < sizeof(response.serverNonce); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(response.serverNonce[i]);
        }
        std::cout << " DBG_NEW_NONCE ";
        for (std::size_t i = 0; i < sizeof(newNonce); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(newNonce[i]);
        }
        std::cout << " DBG_ENCRYPTED ";
        for (std::size_t i = 0; i < encryptedAnswer.size(); ++i) {
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned int>(
                             static_cast<uint8_t>(encryptedAnswer[i]));
        }
        std::cout << std::dec << std::setfill(' ') << std::endl;

        tg::ServerDhInner dhInner =
            tg::parseServerDhParams(
                reinterpret_cast<const uint8_t*>(encryptedAnswer.data()),
                encryptedAnswer.size(), nonce, response.serverNonce, newNonce);

        currentStep = "checkDhParams";
        if (!tg::checkDhParams(dhInner)) {
            throw std::runtime_error("server DH parameters failed validation");
        }
        std::cout << "DH params OK" << std::endl;
        std::cout << "server_time: " << dhInner.serverTime << std::endl;

        currentStep = "compute client DH";
        uint8_t randomB[256];
        uint8_t authKey[256];
        uint64_t authKeyId = 0;
        readRandomBytes(randomB, sizeof(randomB));
        const std::vector<uint8_t> gB =
            tg::computeClientDh(dhInner, randomB, authKey, authKeyId);

        currentStep = "set_client_DH_params";
        std::vector<uint8_t> clientPadding(16);
        readRandomBytes(&clientPadding[0], clientPadding.size());
        const std::vector<uint8_t> encryptedClientDh =
            tg::buildClientDhInnerEncrypted(
                dhInner, newNonce, gB,
                clientPadding.empty() ? NULL : &clientPadding[0],
                clientPadding.size());

        tg::TlWriter setClientDhWriter;
        setClientDhWriter.writeConstructor(tg::SET_CLIENT_DH_PARAMS_ID);
        setClientDhWriter.writeRaw(nonce, sizeof(nonce));
        setClientDhWriter.writeRaw(response.serverNonce, sizeof(response.serverNonce));
        setClientDhWriter.writeBytes(
            encryptedClientDh.empty() ? NULL : &encryptedClientDh[0],
            encryptedClientDh.size());
        sendMtprotoMessage(socketHandle.get(), setClientDhWriter.data(),
                           "set_client_DH_params");

        readMtprotoMessage(socketHandle.get(), answer, body, "dh_gen_ok");

        currentStep = "dh_gen_ok";
        tg::TlReader dhGenReader(body.empty() ? NULL : &body[0], body.size());
        const uint32_t dhGenConstructor = dhGenReader.readConstructor();
        if (dhGenConstructor != tg::DH_GEN_OK_ID) {
            throw std::runtime_error(
                "expected dh_gen_ok, received " +
                constructorDescription(dhGenConstructor));
        }

        uint8_t dhGenNonce[16];
        uint8_t dhGenServerNonce[16];
        uint8_t newNonceHash1[16];
        dhGenReader.readRaw(dhGenNonce, sizeof(dhGenNonce));
        dhGenReader.readRaw(dhGenServerNonce, sizeof(dhGenServerNonce));
        dhGenReader.readRaw(newNonceHash1, sizeof(newNonceHash1));
        expectNonce(dhGenNonce, nonce, sizeof(nonce), "dh_gen_ok nonce");
        expectNonce(dhGenServerNonce, response.serverNonce,
                    sizeof(response.serverNonce), "dh_gen_ok server_nonce");
        tg::checkDhGenOk(newNonce, authKey, newNonceHash1);

        std::cout << "key fingerprint: 0x" << std::hex << std::setw(16)
                  << std::setfill('0') << authKeyId << std::dec
                  << std::setfill(' ') << std::endl;
        std::cout << "AUTH KEY OK" << std::endl;
        return 0;
    } catch (const std::exception& error) {
        return fail(currentStep, error.what(), answer);
    } catch (...) {
        return fail(currentStep, "unknown error", answer);
    }
}
