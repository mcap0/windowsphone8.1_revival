#include "../core/tl.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class Tester {
public:
    Tester() : failures_(0) {}

    void check(const std::string& name, bool passed, const std::string& detail)
    {
        if (passed) {
            std::cout << "  ok " << name << '\n';
        } else {
            ++failures_;
            std::cout << "  FAIL " << name << ": " << detail << '\n';
        }
    }

    int exitCode() const
    {
        return failures_ == 0 ? 0 : 1;
    }

private:
    int failures_;
};

std::string describeException(const std::exception& error)
{
    return error.what();
}

void testStringLength(Tester& tester, std::size_t length)
{
    std::string input;
    std::size_t i;
    std::ostringstream name;

    for (i = 0; i < length; ++i) {
        input.push_back(static_cast<char>((i * 37u) & 0xffu));
    }

    name << "string round trip length " << length;
    try {
        tg::TlWriter writer;
        writer.writeString(input);
        tg::TlReader reader(writer.data().data(), writer.data().size());
        tester.check(name.str(), reader.readString() == input, "round-trip value differs");
    } catch (const std::exception& error) {
        tester.check(name.str(), false, describeException(error));
    }
}

bool throwsRuntimeErrorReadInt32()
{
    const uint8_t bytes[] = { 1, 2, 3 };
    try {
        tg::TlReader reader(bytes, sizeof(bytes));
        reader.readInt32();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

bool throwsRuntimeErrorReadBytes(const uint8_t* bytes, std::size_t size)
{
    try {
        tg::TlReader reader(bytes, size);
        reader.readBytes();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

bool throwsRuntimeErrorReadRaw(const uint8_t* bytes, std::size_t size)
{
    uint8_t output[16];
    try {
        tg::TlReader reader(bytes, size);
        reader.readRaw(output, sizeof(output));
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

} // namespace

int runTlTests()
{
    Tester tester;

    tester.check("CRC boolTrue", tg::crc32("boolTrue = Bool") == 0x997275b5u,
                 "unexpected constructor id");
    tester.check("CRC boolFalse", tg::crc32("boolFalse = Bool") == 0xbc799737u,
                 "unexpected constructor id");

    try {
        tg::TlWriter writer;
        const uint8_t expected[] = { 0xb5, 0x75, 0x72, 0x99 };
        writer.writeBool(true);
        tester.check("writeBool true bytes",
                     writer.data().size() == sizeof(expected)
                         && std::equal(writer.data().begin(), writer.data().end(), expected),
                     "constructor bytes differ");
    } catch (const std::exception& error) {
        tester.check("writeBool true bytes", false, describeException(error));
    }

    try {
        tg::TlWriter writer;
        writer.writeInt32(-123456789);
        writer.writeInt64(-1234567890123456789LL);
        writer.writeDouble(-123.5);

        tg::TlReader reader(writer.data().data(), writer.data().size());
        const bool passed = reader.readInt32() == -123456789
            && reader.readInt64() == -1234567890123456789LL
            && reader.readDouble() == -123.5;
        tester.check("negative integer and double round trip", passed, "round-trip value differs");
    } catch (const std::exception& error) {
        tester.check("negative integer and double round trip", false, describeException(error));
    }

    try {
        tg::TlWriter writer;
        const double nanValue = (std::numeric_limits<double>::quiet_NaN)();
        writer.writeDouble(nanValue);
        tg::TlReader reader(writer.data().data(), writer.data().size());
        tester.check("NaN-safe double round trip", std::isnan(reader.readDouble()),
                     "decoded value is not NaN");
    } catch (const std::exception& error) {
        tester.check("NaN-safe double round trip", false, describeException(error));
    }

    try {
        tg::TlWriter writer;
        const uint8_t emptyExpected[] = { 0, 0, 0, 0 };
        writer.writeString("");
        tester.check("empty string bytes",
                     writer.data().size() == sizeof(emptyExpected)
                         && std::equal(writer.data().begin(), writer.data().end(), emptyExpected),
                     "encoded bytes differ");
    } catch (const std::exception& error) {
        tester.check("empty string bytes", false, describeException(error));
    }

    try {
        tg::TlWriter writer;
        const uint8_t rawBytes[16] = {
            0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
            0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f
        };
        writer.writeRaw(rawBytes, sizeof(rawBytes));
        tester.check("writeRaw appends bytes unchanged",
                     writer.data().size() == sizeof(rawBytes)
                         && std::equal(writer.data().begin(), writer.data().end(), rawBytes),
                     "raw bytes differ or were prefixed");
    } catch (const std::exception& error) {
        tester.check("writeRaw appends bytes unchanged", false, describeException(error));
    }

    {
        const uint8_t rawBytes[16] = {
            0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
            0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f
        };
        uint8_t output[16] = { 0 };

        try {
            tg::TlReader reader(rawBytes, sizeof(rawBytes));
            reader.readRaw(output, sizeof(output));
            tester.check("readRaw reads 16 bytes without a prefix",
                         std::equal(output, output + sizeof(output), rawBytes),
                         "raw bytes differ or a prefix was consumed");
        } catch (const std::exception& error) {
            tester.check("readRaw reads 16 bytes without a prefix", false,
                         describeException(error));
        }

        tester.check("readRaw rejects truncated input",
                     throwsRuntimeErrorReadRaw(rawBytes, sizeof(rawBytes) - 1),
                     "reader accepted fewer than 16 raw bytes");
    }

    try {
        tg::TlWriter writer;
        const uint8_t abcExpected[] = { 3, 'a', 'b', 'c' };
        writer.writeString("abc");
        tester.check("abc string bytes",
                     writer.data().size() == sizeof(abcExpected)
                         && std::equal(writer.data().begin(), writer.data().end(), abcExpected),
                     "encoded bytes differ");
    } catch (const std::exception& error) {
        tester.check("abc string bytes", false, describeException(error));
    }

    try {
        tg::TlWriter writer;
        const std::string value(254, 'x');
        writer.writeBytes(value);
        const std::vector<uint8_t>& encoded = writer.data();
        const bool prefixCorrect = encoded.size() >= 4
            && encoded[0] == 0xfeu && encoded[1] == 0xfeu
            && encoded[2] == 0 && encoded[3] == 0;
        tester.check("254-byte string prefix and alignment",
                     prefixCorrect && encoded.size() % 4 == 0,
                     "prefix or encoded size is incorrect");
    } catch (const std::exception& error) {
        tester.check("254-byte string prefix and alignment", false, describeException(error));
    }

    testStringLength(tester, 0);
    testStringLength(tester, 1);
    testStringLength(tester, 253);
    testStringLength(tester, 254);
    testStringLength(tester, 255);
    testStringLength(tester, 1000);

    tester.check("truncated int32 rejected", throwsRuntimeErrorReadInt32(),
                 "reader accepted a truncated int32");

    {
        const uint8_t truncatedLength[] = { 0xfe, 1 };
        tester.check("truncated byte-string length rejected",
                     throwsRuntimeErrorReadBytes(truncatedLength, sizeof(truncatedLength)),
                     "reader accepted a truncated length prefix");
    }

    {
        const uint8_t truncatedData[] = { 3, 'a', 'b' };
        tester.check("truncated byte-string data rejected",
                     throwsRuntimeErrorReadBytes(truncatedData, sizeof(truncatedData)),
                     "reader accepted truncated data or padding");
    }

    {
        const uint8_t badPadding[] = { 1, 'a', 0, 1 };
        tester.check("non-zero byte-string padding rejected",
                     throwsRuntimeErrorReadBytes(badPadding, sizeof(badPadding)),
                     "reader accepted non-zero padding");
    }

    try {
        tg::TlWriter writer;
        writer.writeVectorHeader(42);
        tg::TlReader reader(writer.data().data(), writer.data().size());
        tester.check("vector header round trip", reader.readVectorHeader() == 42,
                     "vector count differs");
    } catch (const std::exception& error) {
        tester.check("vector header round trip", false, describeException(error));
    }

    return tester.exitCode();
}
