#ifndef TG_CORE_MTPROTO_RPC_H
#define TG_CORE_MTPROTO_RPC_H

#include <cstdint>
#include <string>
#include <vector>

namespace tg {

struct ContainerMessage {
    uint64_t msgId;
    int32_t seqNo;
    std::vector<uint8_t> body;

    ContainerMessage();
    ContainerMessage(uint64_t messageId,
                     int32_t sequenceNumber,
                     const std::vector<uint8_t>& messageBody);
};

struct RpcMessage {
    enum Kind {
        RpcResult,
        RpcError,
        Pong,
        MsgsAck,
        NewSessionCreated,
        BadServerSalt,
        BadMsgNotification,
        Container,
        GzipPacked,
        FutureSalts,
        Other
    };

    Kind kind;
    uint64_t reqMsgId;
    int32_t errorCode;
    std::string errorMessage;
    uint64_t newSalt;
    uint64_t badMsgId;
    uint64_t pongMsgId;
    std::vector<uint8_t> rawResult;
    std::vector<ContainerMessage> children;
    uint64_t firstMsgId;
    uint64_t firstUniqueId;
    uint64_t serverSalt;
    std::vector<uint64_t> msgIds;

    RpcMessage();
};

std::vector<uint8_t> buildPing(uint64_t pingId);
std::vector<uint8_t> buildInvokeWithLayer(
    int32_t layer,
    const std::vector<uint8_t>& query);
std::vector<uint8_t> buildInitConnection(
    int32_t apiId,
    const std::string& deviceModel,
    const std::string& systemVersion,
    const std::string& appVersion,
    const std::string& systemLangCode,
    const std::string& langPack,
    const std::string& langCode,
    const std::vector<uint8_t>& query);
std::vector<uint8_t> buildHelpGetConfig();
std::vector<uint8_t> buildAuthSendCode(
    const std::string& phoneNumber,
    int32_t apiId,
    const std::string& apiHash);
std::vector<uint8_t> buildAuthSignIn(
    const std::string& phoneNumber,
    const std::string& phoneCodeHash,
    const std::string& phoneCode);
std::vector<uint8_t> buildGetFutureSalts(int32_t num);
std::vector<uint8_t> buildMsgsAck(const std::vector<uint64_t>& msgIds);
std::vector<uint8_t> buildContainer(
    const std::vector<ContainerMessage>& messages);

RpcMessage parseServerMessage(const std::vector<uint8_t>& body);
bool parseServerMessage(const std::vector<uint8_t>& body, RpcMessage& out);

} // namespace tg

#endif
