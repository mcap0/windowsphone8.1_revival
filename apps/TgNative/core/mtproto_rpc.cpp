#include "mtproto_rpc.h"

#include "tl.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace tg {

namespace {

static const uint32_t kPing = 0x7abe77ecu;
static const uint32_t kInvokeWithLayer = 0xda9b0d0du;
static const uint32_t kInitConnection = 0xc1cd5ea9u;
static const uint32_t kHelpGetConfig = 0xc4f9186bu;
static const uint32_t kAuthSendCode = 0xa677244fu;
static const uint32_t kCodeSettings = 0xad253d78u;
static const uint32_t kAuthSignIn = 0x8d52a951u;
static const uint32_t kGetFutureSalts = 0xb921bd04u;
static const uint32_t kMsgsAck = 0x62d6b459u;
static const uint32_t kVector = 0x1cb5c415u;
static const uint32_t kMsgContainer = 0x73f3eb64u;

static const uint32_t kRpcResult = 0xf35c6d01u;
static const uint32_t kRpcError = 0x2144ca19u;
static const uint32_t kPong = 0x347773c5u;
static const uint32_t kBadServerSalt = 0xedab447bu;
static const uint32_t kBadMsgNotification = 0xa7eff811u;
static const uint32_t kNewSessionCreated = 0x9ec20908u;
static const uint32_t kGzipPacked = 0x3072cfa1u;
static const uint32_t kFutureSalts = 0xae500895u;
static const uint32_t kFutureSalt = 0x0949d9dcu;

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& bytes)
        : bytes_(bytes), position_(0)
    {
    }

    std::size_t remaining() const
    {
        return bytes_.size() - position_;
    }

    bool readUInt32(uint32_t& value)
    {
        if (remaining() < 4) {
            return false;
        }
        value = static_cast<uint32_t>(bytes_[position_])
            | (static_cast<uint32_t>(bytes_[position_ + 1]) << 8)
            | (static_cast<uint32_t>(bytes_[position_ + 2]) << 16)
            | (static_cast<uint32_t>(bytes_[position_ + 3]) << 24);
        position_ += 4;
        return true;
    }

    bool readInt32(int32_t& value)
    {
        uint32_t raw;
        if (!readUInt32(raw)) {
            return false;
        }
        value = static_cast<int32_t>(raw);
        return true;
    }

    bool readUInt64(uint64_t& value)
    {
        uint32_t low;
        uint32_t high;
        if (!readUInt32(low) || !readUInt32(high)) {
            return false;
        }
        value = static_cast<uint64_t>(low)
            | (static_cast<uint64_t>(high) << 32);
        return true;
    }

    bool readBytes(std::vector<uint8_t>& value)
    {
        if (remaining() < 1) {
            return false;
        }

        const uint8_t first = bytes_[position_++];
        std::size_t length = 0;
        std::size_t headerLength = 1;

        if (first < 254) {
            length = first;
        } else if (first == 254) {
            if (remaining() < 3) {
                return false;
            }
            length = static_cast<std::size_t>(bytes_[position_])
                | (static_cast<std::size_t>(bytes_[position_ + 1]) << 8)
                | (static_cast<std::size_t>(bytes_[position_ + 2]) << 16);
            position_ += 3;
            headerLength = 4;
        } else {
            return false;
        }

        if (length > remaining()) {
            return false;
        }

        value.assign(bytes_.begin() + position_,
                     bytes_.begin() + position_ + length);
        position_ += length;

        const std::size_t padding =
            (4 - ((headerLength + length) & 3u)) & 3u;
        if (padding > remaining()) {
            return false;
        }
        position_ += padding;
        return true;
    }

    bool readString(std::string& value)
    {
        std::vector<uint8_t> bytes;
        if (!readBytes(bytes)) {
            return false;
        }
        value.assign(bytes.begin(), bytes.end());
        return true;
    }

    bool atEnd() const
    {
        return position_ == bytes_.size();
    }

    std::size_t position() const
    {
        return position_;
    }

private:
    const std::vector<uint8_t>& bytes_;
    std::size_t position_;
};

class BitReader {
public:
    BitReader(const uint8_t* data, std::size_t size)
        : data_(data), size_(size), position_(0), bitPosition_(0)
    {
    }

    bool readBits(unsigned int count, uint32_t& value)
    {
        value = 0;
        if (count > 24) {
            return false;
        }

        unsigned int i;
        for (i = 0; i < count; ++i) {
            if (position_ >= size_) {
                return false;
            }
            const uint32_t bit =
                (data_[position_] >> bitPosition_) & 1u;
            value |= bit << i;
            ++bitPosition_;
            if (bitPosition_ == 8) {
                bitPosition_ = 0;
                ++position_;
            }
        }
        return true;
    }

    void alignByte()
    {
        if (bitPosition_ != 0) {
            bitPosition_ = 0;
            ++position_;
        }
    }

    bool readAlignedByte(uint8_t& value)
    {
        if (bitPosition_ != 0 || position_ >= size_) {
            return false;
        }
        value = data_[position_++];
        return true;
    }

private:
    const uint8_t* data_;
    std::size_t size_;
    std::size_t position_;
    unsigned int bitPosition_;
};

struct Huffman {
    std::vector<uint16_t> codes[16];
    std::vector<int> symbols[16];
    unsigned int maximumLength;

    Huffman() : maximumLength(0)
    {
    }
};

unsigned int reverseBits(unsigned int value, unsigned int count)
{
    unsigned int reversed = 0;
    unsigned int i;
    for (i = 0; i < count; ++i) {
        reversed = (reversed << 1) | (value & 1u);
        value >>= 1;
    }
    return reversed;
}

bool buildHuffman(const std::vector<uint8_t>& lengths, Huffman& tree)
{
    unsigned int counts[16] = {};
    unsigned int nextCode[16] = {};
    std::size_t i;

    tree.maximumLength = 0;
    for (i = 0; i < lengths.size(); ++i) {
        if (lengths[i] > 15) {
            return false;
        }
        if (lengths[i] != 0) {
            ++counts[lengths[i]];
            if (lengths[i] > tree.maximumLength) {
                tree.maximumLength = lengths[i];
            }
        }
    }

    if (tree.maximumLength == 0) {
        return false;
    }

    unsigned int code = 0;
    unsigned int bits;
    for (bits = 1; bits <= 15; ++bits) {
        code = (code + counts[bits - 1]) << 1;
        if (code + counts[bits] > (1u << bits)) {
            return false;
        }
        nextCode[bits] = code;
    }

    for (i = 0; i < lengths.size(); ++i) {
        const unsigned int length = lengths[i];
        if (length != 0) {
            const unsigned int reversed =
                reverseBits(nextCode[length]++, length);
            tree.codes[length].push_back(
                static_cast<uint16_t>(reversed));
            tree.symbols[length].push_back(static_cast<int>(i));
        }
    }

    return true;
}

bool decodeSymbol(BitReader& input, const Huffman& tree, int& symbol)
{
    unsigned int code = 0;
    unsigned int length;

    for (length = 1; length <= tree.maximumLength; ++length) {
        uint32_t bit;
        if (!input.readBits(1, bit)) {
            return false;
        }
        code |= bit << (length - 1);

        std::size_t i;
        for (i = 0; i < tree.codes[length].size(); ++i) {
            if (tree.codes[length][i] == code) {
                symbol = tree.symbols[length][i];
                return true;
            }
        }
    }

    return false;
}

bool appendByte(std::vector<uint8_t>& output, uint8_t value)
{
    static const std::size_t kMaximumInflatedSize = 16u * 1024u * 1024u;
    if (output.size() >= kMaximumInflatedSize) {
        return false;
    }
    output.push_back(value);
    return true;
}

bool inflateDeflate(const uint8_t* data,
                    std::size_t size,
                    std::vector<uint8_t>& output)
{
    static const unsigned int lengthBase[29] = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
    };
    static const unsigned int lengthExtra[29] = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
    };
    static const unsigned int distanceBase[30] = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
        193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097,
        6145, 8193, 12289, 16385, 24577
    };
    static const unsigned int distanceExtra[30] = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
    };
    static const unsigned int codeLengthOrder[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };

    BitReader input(data, size);
    bool finalBlock = false;
    output.clear();

    while (!finalBlock) {
        uint32_t value;
        uint32_t blockType;

        if (!input.readBits(1, value)) {
            return false;
        }
        finalBlock = value != 0;
        if (!input.readBits(2, blockType)) {
            return false;
        }

        if (blockType == 0) {
            input.alignByte();
            uint8_t low;
            uint8_t high;
            uint8_t inverseLow;
            uint8_t inverseHigh;
            if (!input.readAlignedByte(low)
                || !input.readAlignedByte(high)
                || !input.readAlignedByte(inverseLow)
                || !input.readAlignedByte(inverseHigh)) {
                return false;
            }

            const unsigned int length =
                static_cast<unsigned int>(low)
                | (static_cast<unsigned int>(high) << 8);
            const unsigned int inverse =
                static_cast<unsigned int>(inverseLow)
                | (static_cast<unsigned int>(inverseHigh) << 8);
            if ((length ^ 0xffffu) != inverse) {
                return false;
            }

            unsigned int i;
            for (i = 0; i < length; ++i) {
                uint8_t byte;
                if (!input.readAlignedByte(byte) || !appendByte(output, byte)) {
                    return false;
                }
            }
            continue;
        }

        if (blockType == 3) {
            return false;
        }

        Huffman literalTree;
        Huffman distanceTree;

        if (blockType == 1) {
            std::vector<uint8_t> literalLengths(288, 0);
            std::vector<uint8_t> distanceLengths(32, 5);
            std::size_t i;
            for (i = 0; i <= 143; ++i) {
                literalLengths[i] = 8;
            }
            for (i = 144; i <= 255; ++i) {
                literalLengths[i] = 9;
            }
            for (i = 256; i <= 279; ++i) {
                literalLengths[i] = 7;
            }
            for (i = 280; i <= 287; ++i) {
                literalLengths[i] = 8;
            }
            if (!buildHuffman(literalLengths, literalTree)
                || !buildHuffman(distanceLengths, distanceTree)) {
                return false;
            }
        } else {
            uint32_t rawHlit;
            uint32_t rawHdist;
            uint32_t rawHclen;
            if (!input.readBits(5, rawHlit)
                || !input.readBits(5, rawHdist)
                || !input.readBits(4, rawHclen)) {
                return false;
            }

            const unsigned int hlit = rawHlit + 257;
            const unsigned int hdist = rawHdist + 1;
            const unsigned int hclen = rawHclen + 4;
            std::vector<uint8_t> codeLengths(19, 0);
            unsigned int i;
            for (i = 0; i < hclen; ++i) {
                uint32_t length;
                if (!input.readBits(3, length)) {
                    return false;
                }
                codeLengths[codeLengthOrder[i]] =
                    static_cast<uint8_t>(length);
            }

            Huffman codeLengthTree;
            if (!buildHuffman(codeLengths, codeLengthTree)) {
                return false;
            }

            const std::size_t total = hlit + hdist;
            std::vector<uint8_t> combined;
            combined.reserve(total);
            while (combined.size() < total) {
                int symbol;
                if (!decodeSymbol(input, codeLengthTree, symbol)) {
                    return false;
                }

                if (symbol >= 0 && symbol <= 15) {
                    combined.push_back(static_cast<uint8_t>(symbol));
                } else if (symbol == 16) {
                    if (combined.empty()) {
                        return false;
                    }
                    uint32_t extra;
                    if (!input.readBits(2, extra)) {
                        return false;
                    }
                    const std::size_t repeat = extra + 3;
                    if (repeat > total - combined.size()) {
                        return false;
                    }
                    combined.insert(combined.end(), repeat, combined.back());
                } else if (symbol == 17) {
                    uint32_t extra;
                    if (!input.readBits(3, extra)) {
                        return false;
                    }
                    const std::size_t repeat = extra + 3;
                    if (repeat > total - combined.size()) {
                        return false;
                    }
                    combined.insert(combined.end(), repeat, 0);
                } else if (symbol == 18) {
                    uint32_t extra;
                    if (!input.readBits(7, extra)) {
                        return false;
                    }
                    const std::size_t repeat = extra + 11;
                    if (repeat > total - combined.size()) {
                        return false;
                    }
                    combined.insert(combined.end(), repeat, 0);
                } else {
                    return false;
                }
            }

            std::vector<uint8_t> literalLengths(
                combined.begin(), combined.begin() + hlit);
            std::vector<uint8_t> distanceLengths(
                combined.begin() + hlit, combined.end());
            if (literalLengths.size() <= 256
                || literalLengths[256] == 0
                || !buildHuffman(literalLengths, literalTree)) {
                return false;
            }

            bool hasDistanceCode = false;
            for (i = 0; i < distanceLengths.size(); ++i) {
                if (distanceLengths[i] != 0) {
                    hasDistanceCode = true;
                    break;
                }
            }
            if (hasDistanceCode
                && !buildHuffman(distanceLengths, distanceTree)) {
                return false;
            }
            if (!hasDistanceCode) {
                distanceTree.maximumLength = 0;
            }
        }

        for (;;) {
            int symbol;
            if (!decodeSymbol(input, literalTree, symbol)) {
                return false;
            }

            if (symbol < 256) {
                if (!appendByte(output, static_cast<uint8_t>(symbol))) {
                    return false;
                }
                continue;
            }

            if (symbol == 256) {
                break;
            }

            if (symbol < 257 || symbol > 285) {
                return false;
            }

            const unsigned int lengthIndex =
                static_cast<unsigned int>(symbol - 257);
            unsigned int matchLength = lengthBase[lengthIndex];
            if (lengthExtra[lengthIndex] != 0) {
                uint32_t extra;
                if (!input.readBits(lengthExtra[lengthIndex], extra)) {
                    return false;
                }
                matchLength += extra;
            }

            if (distanceTree.maximumLength == 0) {
                return false;
            }
            int distanceSymbol;
            if (!decodeSymbol(input, distanceTree, distanceSymbol)
                || distanceSymbol < 0 || distanceSymbol > 29) {
                return false;
            }

            unsigned int distance = distanceBase[distanceSymbol];
            if (distanceExtra[distanceSymbol] != 0) {
                uint32_t extra;
                if (!input.readBits(distanceExtra[distanceSymbol], extra)) {
                    return false;
                }
                distance += extra;
            }
            if (distance == 0 || distance > output.size()) {
                return false;
            }

            unsigned int i;
            for (i = 0; i < matchLength; ++i) {
                const uint8_t byte = output[output.size() - distance];
                if (!appendByte(output, byte)) {
                    return false;
                }
            }
        }
    }

    input.alignByte();
    return true;
}

uint32_t gzipCrc32(const uint8_t* data, std::size_t size)
{
    uint32_t crc = 0xffffffffu;
    std::size_t i;
    for (i = 0; i < size; ++i) {
        unsigned int bit;
        crc ^= data[i];
        for (bit = 0; bit < 8; ++bit) {
            const uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xedb88320u & mask);
        }
    }
    return crc ^ 0xffffffffu;
}

uint32_t readLittleEndian32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0])
        | (static_cast<uint32_t>(data[1]) << 8)
        | (static_cast<uint32_t>(data[2]) << 16)
        | (static_cast<uint32_t>(data[3]) << 24);
}

bool inflateGzip(const std::vector<uint8_t>& gzip,
                 std::vector<uint8_t>& output)
{
    if (gzip.size() < 18
        || gzip[0] != 0x1f
        || gzip[1] != 0x8b
        || gzip[2] != 8) {
        return false;
    }

    const uint8_t flags = gzip[3];
    if ((flags & 0xe0u) != 0) {
        return false;
    }

    std::size_t position = 10;
    const std::size_t trailerPosition = gzip.size() - 8;

    if ((flags & 0x04u) != 0) {
        if (position + 2 > trailerPosition) {
            return false;
        }
        const std::size_t extraLength =
            static_cast<std::size_t>(gzip[position])
            | (static_cast<std::size_t>(gzip[position + 1]) << 8);
        position += 2;
        if (extraLength > trailerPosition - position) {
            return false;
        }
        position += extraLength;
    }

    if ((flags & 0x08u) != 0) {
        while (position < trailerPosition && gzip[position] != 0) {
            ++position;
        }
        if (position >= trailerPosition) {
            return false;
        }
        ++position;
    }

    if ((flags & 0x10u) != 0) {
        while (position < trailerPosition && gzip[position] != 0) {
            ++position;
        }
        if (position >= trailerPosition) {
            return false;
        }
        ++position;
    }

    if ((flags & 0x02u) != 0) {
        if (position + 2 > trailerPosition) {
            return false;
        }
        position += 2;
    }

    if (position > trailerPosition) {
        return false;
    }

    const std::size_t deflateLength = trailerPosition - position;
    if (!inflateDeflate(&gzip[position], deflateLength, output)) {
        return false;
    }

    const uint32_t expectedCrc =
        readLittleEndian32(&gzip[trailerPosition]);
    const uint32_t expectedSize =
        readLittleEndian32(&gzip[trailerPosition + 4]);
    return expectedCrc == gzipCrc32(output.data(), output.size())
        && expectedSize == static_cast<uint32_t>(output.size());
}

bool readVectorHeader(Reader& reader, std::size_t& count)
{
    uint32_t constructor;
    int32_t signedCount;
    if (!reader.readUInt32(constructor)
        || constructor != kVector
        || !reader.readInt32(signedCount)
        || signedCount < 0) {
        return false;
    }
    count = static_cast<std::size_t>(signedCount);
    return true;
}

} // namespace

ContainerMessage::ContainerMessage()
    : msgId(0), seqNo(0)
{
}

ContainerMessage::ContainerMessage(
    uint64_t messageId,
    int32_t sequenceNumber,
    const std::vector<uint8_t>& messageBody)
    : msgId(messageId), seqNo(sequenceNumber), body(messageBody)
{
}

RpcMessage::RpcMessage()
    : kind(Other),
      reqMsgId(0),
      errorCode(0),
      newSalt(0),
      badMsgId(0),
      pongMsgId(0),
      firstMsgId(0),
      firstUniqueId(0),
      serverSalt(0)
{
}

std::vector<uint8_t> buildPing(uint64_t pingId)
{
    TlWriter writer;
    writer.writeConstructor(kPing);
    writer.writeInt64(static_cast<int64_t>(pingId));
    return writer.data();
}

std::vector<uint8_t> buildInvokeWithLayer(
    int32_t layer,
    const std::vector<uint8_t>& query)
{
    TlWriter writer;
    writer.writeConstructor(kInvokeWithLayer);
    writer.writeInt32(layer);
    writer.writeRaw(query.empty() ? NULL : &query[0], query.size());
    return writer.data();
}

std::vector<uint8_t> buildInitConnection(
    int32_t apiId,
    const std::string& deviceModel,
    const std::string& systemVersion,
    const std::string& appVersion,
    const std::string& systemLangCode,
    const std::string& langPack,
    const std::string& langCode,
    const std::vector<uint8_t>& query)
{
    TlWriter writer;
    writer.writeConstructor(kInitConnection);
    writer.writeInt32(0);
    writer.writeInt32(apiId);
    writer.writeString(deviceModel);
    writer.writeString(systemVersion);
    writer.writeString(appVersion);
    writer.writeString(systemLangCode);
    writer.writeString(langPack);
    writer.writeString(langCode);
    writer.writeRaw(query.empty() ? NULL : &query[0], query.size());
    return writer.data();
}

std::vector<uint8_t> buildHelpGetConfig()
{
    TlWriter writer;
    writer.writeConstructor(kHelpGetConfig);
    return writer.data();
}

std::vector<uint8_t> buildAuthSendCode(
    const std::string& phoneNumber,
    int32_t apiId,
    const std::string& apiHash)
{
    TlWriter writer;
    writer.writeConstructor(kAuthSendCode);
    writer.writeString(phoneNumber);
    writer.writeInt32(apiId);
    writer.writeString(apiHash);
    writer.writeConstructor(kCodeSettings);
    writer.writeInt32(0);
    return writer.data();
}

std::vector<uint8_t> buildAuthSignIn(
    const std::string& phoneNumber,
    const std::string& phoneCodeHash,
    const std::string& phoneCode)
{
    TlWriter writer;
    writer.writeConstructor(kAuthSignIn);
    writer.writeInt32(1);
    writer.writeString(phoneNumber);
    writer.writeString(phoneCodeHash);
    writer.writeString(phoneCode);
    return writer.data();
}

std::vector<uint8_t> buildGetFutureSalts(int32_t num)
{
    TlWriter writer;
    writer.writeConstructor(kGetFutureSalts);
    writer.writeInt32(num);
    return writer.data();
}

std::vector<uint8_t> buildMsgsAck(const std::vector<uint64_t>& msgIds)
{
    TlWriter writer;
    writer.writeConstructor(kMsgsAck);
    writer.writeVectorHeader(msgIds.size());
    std::size_t i;
    for (i = 0; i < msgIds.size(); ++i) {
        writer.writeInt64(static_cast<int64_t>(msgIds[i]));
    }
    return writer.data();
}

std::vector<uint8_t> buildContainer(
    const std::vector<ContainerMessage>& messages)
{
    TlWriter writer;
    writer.writeConstructor(kMsgContainer);
    writer.writeVectorHeader(messages.size());
    std::size_t i;
    for (i = 0; i < messages.size(); ++i) {
        writer.writeInt64(static_cast<int64_t>(messages[i].msgId));
        writer.writeInt32(messages[i].seqNo);
        writer.writeBytes(messages[i].body.empty()
                              ? NULL
                              : &messages[i].body[0],
                          messages[i].body.size());
    }
    return writer.data();
}

RpcMessage parseServerMessage(const std::vector<uint8_t>& body)
{
    RpcMessage result;
    parseServerMessage(body, result);
    return result;
}

bool parseServerMessage(const std::vector<uint8_t>& body, RpcMessage& out)
{
    out = RpcMessage();
    Reader reader(body);
    uint32_t constructor;

    if (!reader.readUInt32(constructor)) {
        return false;
    }

    if (constructor == kRpcResult) {
        if (!reader.readUInt64(out.reqMsgId) || reader.remaining() < 4) {
            out = RpcMessage();
            return false;
        }
        out.rawResult.assign(body.begin() + reader.position(), body.end());
        out.kind = RpcMessage::RpcResult;
        return true;
    }

    if (constructor == kRpcError) {
        if (!reader.readInt32(out.errorCode)
            || !reader.readString(out.errorMessage)
            || !reader.atEnd()) {
            out = RpcMessage();
            return false;
        }
        out.kind = RpcMessage::RpcError;
        return true;
    }

    if (constructor == kPong) {
        if (!reader.readUInt64(out.reqMsgId)
            || !reader.readUInt64(out.pongMsgId)
            || !reader.atEnd()) {
            out = RpcMessage();
            return false;
        }
        out.kind = RpcMessage::Pong;
        return true;
    }

    if (constructor == kBadServerSalt) {
        if (!reader.readUInt64(out.badMsgId)
            || !reader.readInt32(out.errorCode)
            || !reader.readInt32(out.errorCode)
            || !reader.readUInt64(out.newSalt)
            || !reader.atEnd()) {
            out = RpcMessage();
            return false;
        }
        out.kind = RpcMessage::BadServerSalt;
        return true;
    }

    if (constructor == kBadMsgNotification) {
        if (!reader.readUInt64(out.badMsgId)
            || !reader.readInt32(out.errorCode)
            || !reader.readInt32(out.errorCode)
            || !reader.atEnd()) {
            out = RpcMessage();
            return false;
        }
        out.kind = RpcMessage::BadMsgNotification;
        return true;
    }

    if (constructor == kNewSessionCreated) {
        if (!reader.readUInt64(out.firstMsgId)
            || !reader.readUInt64(out.firstUniqueId)
            || !reader.readUInt64(out.serverSalt)
            || !reader.atEnd()) {
            out = RpcMessage();
            return false;
        }
        out.kind = RpcMessage::NewSessionCreated;
        return true;
    }

    if (constructor == kMsgsAck) {
        std::size_t count;
        if (!readVectorHeader(reader, count)
            || count > reader.remaining() / 8) {
            out = RpcMessage();
            return false;
        }
        out.msgIds.reserve(count);
        std::size_t i;
        for (i = 0; i < count; ++i) {
            uint64_t id;
            if (!reader.readUInt64(id)) {
                out = RpcMessage();
                return false;
            }
            out.msgIds.push_back(id);
        }
        if (!reader.atEnd()) {
            out = RpcMessage();
            return false;
        }
        out.kind = RpcMessage::MsgsAck;
        return true;
    }

    if (constructor == kMsgContainer) {
        std::size_t count;
        if (!readVectorHeader(reader, count)
            || count > 100000
            || count > reader.remaining() / 16) {
            out = RpcMessage();
            return false;
        }
        out.children.reserve(count);
        std::size_t i;
        for (i = 0; i < count; ++i) {
            ContainerMessage child;
            if (!reader.readUInt64(child.msgId)
                || !reader.readInt32(child.seqNo)
                || !reader.readBytes(child.body)) {
                out = RpcMessage();
                return false;
            }
            out.children.push_back(child);
        }
        if (!reader.atEnd()) {
            out = RpcMessage();
            return false;
        }
        out.kind = RpcMessage::Container;
        return true;
    }

    if (constructor == kGzipPacked) {
        std::vector<uint8_t> gzip;
        if (!reader.readBytes(gzip) || !reader.atEnd()
            || !inflateGzip(gzip, out.rawResult)) {
            out = RpcMessage();
            return false;
        }
        out.kind = RpcMessage::GzipPacked;
        return true;
    }

    if (constructor == kFutureSalts) {
        int32_t now;
        std::size_t count;
        if (!reader.readUInt64(out.reqMsgId)
            || !reader.readInt32(now)
            || !readVectorHeader(reader, count)
            || count > 100000
            || count > reader.remaining() / 20) {
            out = RpcMessage();
            return false;
        }

        std::size_t i;
        for (i = 0; i < count; ++i) {
            uint32_t saltConstructor;
            int32_t validSince;
            int32_t validUntil;
            uint64_t salt;
            if (!reader.readUInt32(saltConstructor)
                || saltConstructor != kFutureSalt
                || !reader.readInt32(validSince)
                || !reader.readInt32(validUntil)
                || !reader.readUInt64(salt)) {
                out = RpcMessage();
                return false;
            }
        }
        if (!reader.atEnd()) {
            out = RpcMessage();
            return false;
        }
        out.kind = RpcMessage::FutureSalts;
        return true;
    }

    return false;
}

} // namespace tg
