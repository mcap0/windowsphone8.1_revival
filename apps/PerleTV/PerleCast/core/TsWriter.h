#ifndef PERLECAST_TSWRITER_H
#define PERLECAST_TSWRITER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

namespace PerleCast {

class TsWriter {
public:
    static const size_t PacketSize = 188;
    static const uint8_t SyncByte = 0x47;
    static const int PatPid = 0x0000;
    static const int SdtPid = 0x0011;
    static const int PmtPid = 0x1000;
    static const int VideoPid = 0x0100;
    static const int AudioPid = 0x0101;
    static const int StreamTypeH264 = 0x1B;
    static const int StreamTypeAac = 0x0F;
    static const int64_t Clock = 90000;

    typedef std::function<void(const std::array<uint8_t, PacketSize>&)> PacketCallback;

    explicit TsWriter(const PacketCallback& callback);
    void WriteTables();
    void WriteSdt();
    void WriteVideo(const uint8_t* data, size_t length, int64_t pts, int64_t dts, bool keyframe, bool includePcr);
    void WriteAudio(const uint8_t* data, size_t length, int64_t pts);
    static uint32_t Crc32(const uint8_t* data, size_t length);
    static std::vector<uint8_t> WithCrc(const std::vector<uint8_t>& section);

private:
    PacketCallback callback_;
    std::map<int, int> continuity_;
    void WriteSection(int pid, const std::vector<uint8_t>& section);
    void WritePes(int pid, int streamId, const uint8_t* data, size_t length, int64_t pts,
                  int64_t dts, bool randomAccess, bool includePcr);
    int NextContinuity(int pid);
};

} // namespace PerleCast

#endif
