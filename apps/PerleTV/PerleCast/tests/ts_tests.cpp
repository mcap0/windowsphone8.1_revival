#include "Test.h"
#include "../core/TsMuxer.h"
#include "../core/TsWriter.h"

#include <map>
#include <vector>

int RunTsTests()
{
    using namespace PerleCast;
    std::vector<std::array<uint8_t, TsWriter::PacketSize> > packets;
    TsWriter writer([&](const std::array<uint8_t, TsWriter::PacketSize>& packet) { packets.push_back(packet); });
    writer.WriteTables();
    uint8_t video[] = {0,0,0,1,0x65,0x88};
    uint8_t audio[] = {0xFF,0xF1,0x50,0x80,0x01,0x7F,0xFC,0x11};
    writer.WriteVideo(video, sizeof(video), 126000, 126000, true, true);
    writer.WriteAudio(audio, sizeof(audio), 126000);
    bool allPacketsValid = !packets.empty();
    std::map<int, int> previous;
    bool continuityValid = true;
    bool pcrFound = false;
    for (size_t i = 0; i < packets.size(); ++i) {
        const std::array<uint8_t, TsWriter::PacketSize>& p = packets[i];
        allPacketsValid = allPacketsValid && p[0] == 0x47 && p.size() == 188;
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        int cc = p[3] & 0x0F;
        if (previous.find(pid) != previous.end()) continuityValid = continuityValid && cc == ((previous[pid] + 1) & 15);
        previous[pid] = cc;
        if (pid == TsWriter::VideoPid && (p[3] & 0x20) && (p[5] & 0x10)) pcrFound = true;
    }
    Check(allPacketsValid, "TS packets are 188 bytes with sync byte");
    Check(continuityValid, "TS continuity counters increment per PID");
    Check(pcrFound, "PCR is emitted on video keyframe packet");

    uint32_t crc = TsWriter::Crc32(reinterpret_cast<const uint8_t*>("\x00\xB0\x0D\x00\x01\xC1\x00\x00\x00\x01\xF0\x00"), 12);
    Check(crc != 0, "MPEG-2 section CRC is calculated");

    std::vector<std::array<uint8_t, TsWriter::PacketSize> > muxed;
    TsMuxer mux([&](const std::array<uint8_t, TsWriter::PacketSize>& p) { muxed.push_back(p); });
    uint8_t frame1[] = {0,0,0,1,0x65,0xAA};
    uint8_t frame2[] = {0,0,0,1,0x41,0xBB};
    mux.PushVideo(frame1, sizeof(frame1), 0, 0, true);
    mux.PushAudio(audio, sizeof(audio), 0);
    mux.PushVideo(frame2, sizeof(frame2), 90000, 90000, false);
    mux.PushAudio(audio, sizeof(audio), 90000);
    mux.Finish();
    Check(!muxed.empty(), "muxer emits a synthetic interleaved stream");
    bool valid = true;
    for (size_t i = 0; i < muxed.size(); ++i) valid = valid && muxed[i][0] == 0x47;
    Check(valid, "muxer output can be consumed as transport packets");
    return TestFailures();
}
