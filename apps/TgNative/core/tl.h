#ifndef TG_CORE_TL_H
#define TG_CORE_TL_H

#include <cstddef>
#include <stdint.h>
#include <string>
#include <vector>

namespace tg {

static const uint32_t kBoolTrue = 0x997275b5u;
static const uint32_t kBoolFalse = 0xbc799737u;
static const uint32_t kVector = 0x1cb5c415u;

class TlWriter {
public:
    void writeInt32(int32_t value);
    void writeUInt32(uint32_t value);
    void writeInt64(int64_t value);
    void writeDouble(double value);
    void writeBool(bool value);
    void writeBytes(const std::string& value);
    void writeBytes(const uint8_t* value, std::size_t size);
    void writeRaw(const uint8_t* value, std::size_t size);
    void writeString(const std::string& value);
    void writeConstructor(uint32_t id);
    void writeVectorHeader(std::size_t count);

    const std::vector<uint8_t>& data() const;

private:
    std::vector<uint8_t> data_;
};

class TlReader {
public:
    TlReader(const uint8_t* data, std::size_t size);

    int32_t readInt32();
    uint32_t readUInt32();
    int64_t readInt64();
    double readDouble();
    bool readBool();
    std::string readBytes();
    std::string readString();
    uint32_t readConstructor();
    std::size_t readVectorHeader();
    void readRaw(uint8_t* out, std::size_t size);

private:
    void require(std::size_t count) const;
    uint8_t readByte();

    const uint8_t* data_;
    std::size_t size_;
    std::size_t position_;
};

uint32_t crc32(const char* s);

} // namespace tg

#endif
