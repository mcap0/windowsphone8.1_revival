#include "tl.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace tg {

namespace {

const uint32_t kMaxTlBytesLength = 0x00ffffffu;

void appendLittleEndian32(std::vector<uint8_t>& data, uint32_t value)
{
    data.push_back(static_cast<uint8_t>(value & 0xffu));
    data.push_back(static_cast<uint8_t>((value >> 8) & 0xffu));
    data.push_back(static_cast<uint8_t>((value >> 16) & 0xffu));
    data.push_back(static_cast<uint8_t>((value >> 24) & 0xffu));
}

} // namespace

void TlWriter::writeInt32(int32_t value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    writeUInt32(bits);
}

void TlWriter::writeUInt32(uint32_t value)
{
    appendLittleEndian32(data_, value);
}

void TlWriter::writeInt64(int64_t value)
{
    uint64_t bits;
    unsigned int i;
    std::memcpy(&bits, &value, sizeof(bits));
    for (i = 0; i < 8; ++i) {
        data_.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xffu));
    }
}

void TlWriter::writeDouble(double value)
{
    uint64_t bits;
    unsigned int i;
    static_assert(sizeof(double) == sizeof(uint64_t), "TL requires 64-bit doubles");
    std::memcpy(&bits, &value, sizeof(bits));
    for (i = 0; i < 8; ++i) {
        data_.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xffu));
    }
}

void TlWriter::writeBool(bool value)
{
    writeConstructor(value ? kBoolTrue : kBoolFalse);
}

void TlWriter::writeBytes(const std::string& value)
{
    writeBytes(reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

void TlWriter::writeBytes(const uint8_t* value, std::size_t size)
{
    std::size_t prefixSize;
    std::size_t padding;
    std::size_t i;

    if (size > kMaxTlBytesLength) {
        throw std::length_error("TL byte string exceeds 0xffffff bytes");
    }
    if (size != 0 && value == 0) {
        throw std::invalid_argument("TL byte string data is null");
    }

    if (size <= 253) {
        data_.push_back(static_cast<uint8_t>(size));
        prefixSize = 1;
    } else {
        const uint32_t length = static_cast<uint32_t>(size);
        data_.push_back(0xfeu);
        data_.push_back(static_cast<uint8_t>(length & 0xffu));
        data_.push_back(static_cast<uint8_t>((length >> 8) & 0xffu));
        data_.push_back(static_cast<uint8_t>((length >> 16) & 0xffu));
        prefixSize = 4;
    }

    for (i = 0; i < size; ++i) {
        data_.push_back(value[i]);
    }

    padding = (4 - ((prefixSize + size) & 3u)) & 3u;
    for (i = 0; i < padding; ++i) {
        data_.push_back(0);
    }
}

void TlWriter::writeRaw(const uint8_t* value, std::size_t size)
{
    std::size_t i;

    if (size != 0 && value == 0) {
        throw std::invalid_argument("raw data is null");
    }

    for (i = 0; i < size; ++i) {
        data_.push_back(value[i]);
    }
}

void TlWriter::writeString(const std::string& value)
{
    writeBytes(value);
}

void TlWriter::writeConstructor(uint32_t id)
{
    writeUInt32(id);
}

void TlWriter::writeVectorHeader(std::size_t count)
{
    if (count > static_cast<std::size_t>((std::numeric_limits<int32_t>::max)())) {
        throw std::length_error("TL vector count exceeds int32 range");
    }

    writeConstructor(kVector);
    writeInt32(static_cast<int32_t>(count));
}

const std::vector<uint8_t>& TlWriter::data() const
{
    return data_;
}

TlReader::TlReader(const uint8_t* data, std::size_t size)
    : data_(data), size_(size), position_(0)
{
    if (size != 0 && data == 0) {
        throw std::runtime_error("TL input data is null");
    }
}

void TlReader::require(std::size_t count) const
{
    if (position_ > size_ || count > size_ - position_) {
        throw std::runtime_error("truncated TL input");
    }
}

uint8_t TlReader::readByte()
{
    require(1);
    return data_[position_++];
}

uint32_t TlReader::readUInt32()
{
    uint32_t value;
    require(4);
    value = static_cast<uint32_t>(data_[position_])
        | (static_cast<uint32_t>(data_[position_ + 1]) << 8)
        | (static_cast<uint32_t>(data_[position_ + 2]) << 16)
        | (static_cast<uint32_t>(data_[position_ + 3]) << 24);
    position_ += 4;
    return value;
}

int32_t TlReader::readInt32()
{
    const uint32_t bits = readUInt32();
    int32_t value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

int64_t TlReader::readInt64()
{
    uint64_t bits = 0;
    unsigned int i;
    int64_t value;

    require(8);
    for (i = 0; i < 8; ++i) {
        bits |= static_cast<uint64_t>(data_[position_ + i]) << (i * 8);
    }
    position_ += 8;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

double TlReader::readDouble()
{
    uint64_t bits = 0;
    unsigned int i;
    double value;

    static_assert(sizeof(double) == sizeof(uint64_t), "TL requires 64-bit doubles");
    require(8);
    for (i = 0; i < 8; ++i) {
        bits |= static_cast<uint64_t>(data_[position_ + i]) << (i * 8);
    }
    position_ += 8;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool TlReader::readBool()
{
    const uint32_t id = readConstructor();
    if (id == kBoolTrue) {
        return true;
    }
    if (id == kBoolFalse) {
        return false;
    }
    throw std::runtime_error("invalid TL Bool constructor");
}

std::string TlReader::readBytes()
{
    const uint8_t first = readByte();
    uint32_t length;
    std::size_t prefixSize;
    std::size_t padding;
    std::size_t i;
    std::string result;

    if (first <= 253) {
        length = first;
        prefixSize = 1;
    } else if (first == 0xfeu) {
        require(3);
        length = static_cast<uint32_t>(data_[position_])
            | (static_cast<uint32_t>(data_[position_ + 1]) << 8)
            | (static_cast<uint32_t>(data_[position_ + 2]) << 16);
        position_ += 3;
        if (length <= 253) {
            throw std::runtime_error("non-canonical TL byte string length");
        }
        prefixSize = 4;
    } else {
        throw std::runtime_error("invalid TL byte string length prefix");
    }

    require(static_cast<std::size_t>(length));
    if (length != 0) {
        result.assign(reinterpret_cast<const char*>(data_ + position_),
                      static_cast<std::size_t>(length));
    }
    position_ += static_cast<std::size_t>(length);

    padding = (4 - ((prefixSize + static_cast<std::size_t>(length)) & 3u)) & 3u;
    require(padding);
    for (i = 0; i < padding; ++i) {
        if (data_[position_ + i] != 0) {
            throw std::runtime_error("non-zero TL byte string padding");
        }
    }
    position_ += padding;
    return result;
}

std::string TlReader::readString()
{
    return readBytes();
}

uint32_t TlReader::readConstructor()
{
    return readUInt32();
}

std::size_t TlReader::readVectorHeader()
{
    const uint32_t constructor = readConstructor();
    const int32_t count = readInt32();

    if (constructor != kVector) {
        throw std::runtime_error("invalid TL vector constructor");
    }
    if (count < 0) {
        throw std::runtime_error("negative TL vector count");
    }
    return static_cast<std::size_t>(count);
}

void TlReader::readRaw(uint8_t* out, std::size_t size)
{
    require(size);
    if (size != 0 && out == 0) {
        throw std::invalid_argument("raw output data is null");
    }
    if (size != 0) {
        std::memcpy(out, data_ + position_, size);
    }
    position_ += size;
}

uint32_t crc32(const char* s)
{
    uint32_t crc = 0xffffffffu;
    const unsigned char* p;

    if (s == 0) {
        throw std::invalid_argument("CRC-32 input is null");
    }

    p = reinterpret_cast<const unsigned char*>(s);
    while (*p != 0) {
        unsigned int bit;
        crc ^= static_cast<uint32_t>(*p++);
        for (bit = 0; bit < 8; ++bit) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xedb88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xffffffffu;
}

} // namespace tg
