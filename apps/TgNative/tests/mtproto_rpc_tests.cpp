#include "../core/mtproto_rpc.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

class Tester {
public:
    Tester() : checks_(0), failures_(0) {}

    void check(const std::string& name,
               bool passed,
               const std::string& detail)
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
            std::cout << "  FAIL MTProto RPC suite: no checks were run\n";
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
    if ((hex.size() & 1u) != 0) {
        return result;
    }

    std::size_t i;
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

void checkTruncationPrefixes(Tester& tester,
                             const std::string& name,
                             const std::vector<uint8_t>& message)
{
    std::size_t prefixLength;
    bool passed = true;
    for (prefixLength = 0; prefixLength < message.size(); ++prefixLength) {
        const std::vector<uint8_t> prefix(
            message.begin(), message.begin() + prefixLength);
        tg::RpcMessage parsed;
        if (tg::parseServerMessage(prefix, parsed)
            || parsed.kind != tg::RpcMessage::Other) {
            passed = false;
            break;
        }
    }
    tester.check(name, passed, "a truncated prefix was accepted or failed parsing");
}

} // namespace

int runMtprotoRpcTests()
{
    Tester tester;

    const std::vector<uint8_t> pingExpected =
        fromHex("ec77be7a0807060504030201");
    tester.check("ping builder is byte-exact",
                 tg::buildPing(0x0102030405060708ULL) == pingExpected,
                 "constructor or ping_id encoding differs from the TL specification");

    const std::vector<uint8_t> futureSaltsExpected =
        fromHex("04bd21b902000000");
    tester.check("getFutureSalts builder is byte-exact",
                 tg::buildGetFutureSalts(2) == futureSaltsExpected,
                 "constructor or num encoding differs from the TL specification");

    const std::vector<uint64_t> ackIds;
    std::vector<uint64_t> twoAckIds;
    twoAckIds.push_back(0x0102030405060708ULL);
    twoAckIds.push_back(0x1112131415161718ULL);
    const std::vector<uint8_t> ackExpected =
        fromHex("59b4d66215c4b51c0200000008070605040302011817161514131211");
    tester.check("msgs_ack builder is byte-exact",
                 tg::buildMsgsAck(twoAckIds) == ackExpected,
                 "vector encoding differs from the TL specification");

    std::vector<tg::ContainerMessage> messages;
    messages.push_back(tg::ContainerMessage(
        0x0102030405060708ULL,
        7,
        fromHex("aabbccdd")));
    const std::vector<uint8_t> containerExpected =
        fromHex("64ebf37315c4b51c0100000008070605040302010700000004aabbccdd000000");
    tester.check("message container builder is byte-exact",
                 tg::buildContainer(messages) == containerExpected,
                 "container fields or message body encoding differs from the TL specification");

    const std::vector<uint8_t> rpcResult =
        fromHex("016d5cf3080706050403020178563412");
    const std::vector<uint8_t> rpcError =
        fromHex("19ca442190010000024e4f00");
    const std::vector<uint8_t> pong =
        fromHex("c573773408070605040302011817161514131211");
    const std::vector<uint8_t> badServerSalt =
        fromHex("7b44abed08070605040302010700000090010000efcdab8967452301");
    const std::vector<uint8_t> badMessage =
        fromHex("11f8efa708070605040302010700000090010000");
    const std::vector<uint8_t> newSession =
        fromHex("0809c29e080706050403020118171615141312112827262524232221");
    const std::vector<uint8_t> parsedContainer =
        fromHex("64ebf37315c4b51c0100000008070605040302010700000004aabbccdd000000");
    const std::vector<uint8_t> gzipPacked =
        fromHex("a1cf72301a1f8b08000000000000ff010300fcff616263c24124350300000000");

    tg::RpcMessage parsed;

    bool passed = tg::parseServerMessage(rpcResult, parsed)
        && parsed.kind == tg::RpcMessage::RpcResult
        && parsed.reqMsgId == 0x0102030405060708ULL
        && parsed.rawResult == fromHex("78563412");
    tester.check("rpc_result parses request id and raw result",
                 passed, "parsed fields differ from the hand-built message");

    passed = tg::parseServerMessage(rpcError, parsed)
        && parsed.kind == tg::RpcMessage::RpcError
        && parsed.errorCode == 400
        && parsed.errorMessage == "NO";
    tester.check("rpc_error parses code and message",
                 passed, "parsed fields differ from the hand-built message");

    passed = tg::parseServerMessage(pong, parsed)
        && parsed.kind == tg::RpcMessage::Pong
        && parsed.reqMsgId == 0x0102030405060708ULL
        && parsed.pongMsgId == 0x1112131415161718ULL;
    tester.check("pong parses both message ids",
                 passed, "parsed fields differ from the hand-built message");

    passed = tg::parseServerMessage(badServerSalt, parsed)
        && parsed.kind == tg::RpcMessage::BadServerSalt
        && parsed.badMsgId == 0x0102030405060708ULL
        && parsed.errorCode == 400
        && parsed.newSalt == 0x0123456789abcdefULL;
    tester.check("bad_server_salt parses message fields",
                 passed, "parsed fields differ from the hand-built message");

    passed = tg::parseServerMessage(badMessage, parsed)
        && parsed.kind == tg::RpcMessage::BadMsgNotification
        && parsed.badMsgId == 0x0102030405060708ULL
        && parsed.errorCode == 400;
    tester.check("bad_msg_notification parses message fields",
                 passed, "parsed fields differ from the hand-built message");

    passed = tg::parseServerMessage(newSession, parsed)
        && parsed.kind == tg::RpcMessage::NewSessionCreated
        && parsed.firstMsgId == 0x0102030405060708ULL
        && parsed.firstUniqueId == 0x1112131415161718ULL
        && parsed.serverSalt == 0x2122232425262728ULL;
    tester.check("new_session_created parses session fields",
                 passed, "parsed fields differ from the hand-built message");

    passed = tg::parseServerMessage(parsedContainer, parsed)
        && parsed.kind == tg::RpcMessage::Container
        && parsed.children.size() == 1
        && parsed.children[0].msgId == 0x0102030405060708ULL
        && parsed.children[0].seqNo == 7
        && parsed.children[0].body == fromHex("aabbccdd");
    tester.check("msg_container parses child messages",
                 passed, "parsed fields differ from the hand-built message");

    passed = tg::parseServerMessage(gzipPacked, parsed)
        && parsed.kind == tg::RpcMessage::GzipPacked
        && parsed.rawResult == fromHex("616263");
    tester.check("gzip_packed inflates a stored DEFLATE gzip stream",
                 passed, "inflated bytes differ from the hand-built gzip payload");

    const std::vector<std::vector<uint8_t> > messagesToTruncate = {
        rpcResult,
        rpcError,
        pong,
        badServerSalt,
        badMessage,
        newSession,
        parsedContainer,
        gzipPacked
    };
    const char* truncationNames[] = {
        "rpc_result prefixes reject truncation",
        "rpc_error prefixes reject truncation",
        "pong prefixes reject truncation",
        "bad_server_salt prefixes reject truncation",
        "bad_msg_notification prefixes reject truncation",
        "new_session_created prefixes reject truncation",
        "msg_container prefixes reject truncation",
        "gzip_packed prefixes reject truncation"
    };

    std::size_t i;
    for (i = 0; i < messagesToTruncate.size(); ++i) {
        checkTruncationPrefixes(
            tester, truncationNames[i], messagesToTruncate[i]);
    }

    {
        const std::vector<uint8_t> unknown = fromHex("01020304");
        parsed = tg::parseServerMessage(unknown);
        tester.check("unknown constructor returns Other",
                     parsed.kind == tg::RpcMessage::Other,
                     "unknown constructor was not classified as Other");
    }

    {
        parsed = tg::parseServerMessage(fromHex("016d5cf30807060504030201"));
        tester.check("truncated rpc_result returns Other",
                     parsed.kind == tg::RpcMessage::Other,
                     "truncated rpc_result was not rejected");
    }

    return tester.exitCode();
}
