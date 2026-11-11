#include "TsWriter.h"

#include <algorithm>
#include <cstring>

namespace PerleCast {

namespace {
void PutTimestamp(uint8_t* target, int prefix, int64_t value)
{
    uint64_t time = static_cast<uint64_t>(value) & 0x1FFFFFFFFULL;
    target[0] = static_cast<uint8_t>((prefix << 4) | ((time >> 29) & 0x0E) | 1);
    target[1] = static_cast<uint8_t>(time >> 22);
    target[2] = static_cast<uint8_t>(((time >> 14) & 0xFE) | 1);
    target[3] = static_cast<uint8_t>(time >> 7);
    target[4] = static_cast<uint8_t>(((time << 1) & 0xFE) | 1);
}
}

TsWriter::TsWriter(const PacketCallback& callback) : callback_(callback) {}

uint32_t TsWriter::Crc32(const uint8_t* data, size_t length)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x80000000) ? (crc << 1) ^ 0x04C11DB7 : crc << 1;
    }
    return crc;
}

std::vector<uint8_t> TsWriter::WithCrc(const std::vector<uint8_t>& section)
{
    uint32_t crc = Crc32(&section[0], section.size());
    std::vector<uint8_t> result(section);
    result.push_back(static_cast<uint8_t>(crc >> 24));
    result.push_back(static_cast<uint8_t>(crc >> 16));
    result.push_back(static_cast<uint8_t>(crc >> 8));
    result.push_back(static_cast<uint8_t>(crc));
    return result;
}

void TsWriter::WriteTables()
{
    std::vector<uint8_t> pat;
    const uint8_t patBytes[] = {0x00,0xB0,0x0D,0x00,0x01,0xC1,0x00,0x00,0x00,0x01,0xF0,0x00};
    pat.assign(patBytes, patBytes + sizeof(patBytes));
    WriteSection(PatPid, WithCrc(pat));

    std::vector<uint8_t> pmt;
    const uint8_t pmtBytes[] = {0x02,0xB0,0x17,0x00,0x01,0xC1,0x00,0x00,
        0xE1,0x00,0xF0,0x00,0x1B,0xE1,0x00,0xF0,0x00,0x0F,0xE1,0x01,0xF0,0x00};
    pmt.assign(pmtBytes, pmtBytes + sizeof(pmtBytes));
    WriteSection(PmtPid, WithCrc(pmt));
}

void TsWriter::WriteSdt()
{
    const uint8_t base[] = {0x42,0xF0,0x1B,0x00,0x01,0xC1,0x00,0x00,0xFF,0x01,0xFF,0x00,0x01,0xFC,0x80,0x0E,
        0x48,0x0C,0x01,0x07,'N','e','w','P','i','p','e',0x05,'N','e','w','P','i','p','e'};
    std::vector<uint8_t> sdt(base, base + sizeof(base));
    sdt[1] = static_cast<uint8_t>(0xF0 | ((sdt.size() + 4 - 3) >> 8));
    sdt[2] = static_cast<uint8_t>(sdt.size() + 4 - 3);
    WriteSection(SdtPid, WithCrc(sdt));
}

int TsWriter::NextContinuity(int pid)
{
    int value = continuity_[pid];
    continuity_[pid] = (value + 1) & 0x0F;
    return value;
}

void TsWriter::WriteSection(int pid, const std::vector<uint8_t>& section)
{
    std::array<uint8_t, PacketSize> packet;
    packet.fill(0xFF);
    packet[0] = SyncByte;
    packet[1] = static_cast<uint8_t>(0x40 | ((pid >> 8) & 0x1F));
    packet[2] = static_cast<uint8_t>(pid);
    packet[3] = static_cast<uint8_t>(0x10 | NextContinuity(pid));
    packet[4] = 0;
    size_t copy = std::min(section.size(), PacketSize - 5);
    std::copy(section.begin(), section.begin() + copy, packet.begin() + 5);
    callback_(packet);
}

void TsWriter::WriteVideo(const uint8_t* data, size_t length, int64_t pts, int64_t dts, bool keyframe, bool includePcr)
{
    WritePes(VideoPid, 0xE0, data, length, pts, dts, keyframe, includePcr);
}

void TsWriter::WriteAudio(const uint8_t* data, size_t length, int64_t pts)
{
    WritePes(AudioPid, 0xC0, data, length, pts, pts, true, false);
}

void TsWriter::WritePes(int pid, int streamId, const uint8_t* data, size_t length,
                        int64_t pts, int64_t dts, bool randomAccess, bool includePcr)
{
    bool hasDts = pts != dts;
    int optionalLength = hasDts ? 10 : 5;
    std::vector<uint8_t> pes(9 + optionalLength, 0);
    pes[0] = 0; pes[1] = 0; pes[2] = 1; pes[3] = static_cast<uint8_t>(streamId);
    size_t pesLength = 3 + optionalLength + length;
    uint16_t encodedLength = pesLength > 0xFFFF ? 0 : static_cast<uint16_t>(pesLength);
    pes[4] = static_cast<uint8_t>(encodedLength >> 8); pes[5] = static_cast<uint8_t>(encodedLength);
    pes[6] = 0x80; pes[7] = hasDts ? 0xC0 : 0x80; pes[8] = static_cast<uint8_t>(optionalLength);
    PutTimestamp(&pes[9], hasDts ? 3 : 2, pts);
    if (hasDts) PutTimestamp(&pes[14], 1, dts);
    pes.insert(pes.end(), data, data + length);

    size_t offset = 0;
    bool first = true;
    while (offset < pes.size()) {
        std::array<uint8_t, PacketSize> packet;
        packet.fill(0xFF);
        packet[0] = SyncByte;
        packet[1] = static_cast<uint8_t>((first ? 0x40 : 0) | ((pid >> 8) & 0x1F));
        packet[2] = static_cast<uint8_t>(pid);

        int flags = (first && randomAccess ? 0x40 : 0) | (first && includePcr ? 0x10 : 0);
        int minimum = first && includePcr ? 8 : (first && flags ? 2 : 0);
        size_t remaining = pes.size() - offset;
        size_t space = PacketSize - 4 - minimum;
        size_t fieldLength = remaining < space ? minimum + (space - remaining) : minimum;
        size_t payloadSize = PacketSize - 4 - fieldLength;
        packet[3] = static_cast<uint8_t>((fieldLength > 0 ? 0x30 : 0x10) | NextContinuity(pid));
        if (fieldLength > 0) {
            packet[4] = static_cast<uint8_t>(fieldLength - 1);
            if (fieldLength > 1) packet[5] = static_cast<uint8_t>(flags);
            if (first && includePcr) {
                int64_t pcr = std::max<int64_t>(0, dts - 63000);
                uint64_t base = static_cast<uint64_t>(pcr) & 0x1FFFFFFFFULL;
                packet[6] = static_cast<uint8_t>(base >> 25);
                packet[7] = static_cast<uint8_t>(base >> 17);
                packet[8] = static_cast<uint8_t>(base >> 9);
                packet[9] = static_cast<uint8_t>(base >> 1);
                packet[10] = static_cast<uint8_t>(((base & 1) << 7) | 0x7E);
                packet[11] = 0;
            }
        }
        std::copy(pes.begin() + offset, pes.begin() + offset + payloadSize, packet.begin() + 4 + fieldLength);
        callback_(packet);
        offset += payloadSize;
        first = false;
    }
}

} // namespace PerleCast
