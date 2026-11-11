// SPDX-License-Identifier: GPL-3.0-or-later
#include "TsDemux.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace perle {

namespace {

const size_t kPacketSize = 188;
const int kPatPid = 0;
const uint8_t kStreamTypeH264 = 0x1B;
const uint8_t kStreamTypeAdtsAac = 0x0F;

struct PesUnit {
    std::vector<uint8_t> data;   // elementary stream bytes after the PES header
    bool hasPts = false;
    bool hasDts = false;
    int64_t pts = 0;             // raw 33-bit values
    int64_t dts = 0;
};

// Reads the 33-bit timestamp of a PES header (5 bytes, marker bits in between).
int64_t ReadTimestamp(const uint8_t* p)
{
    return (int64_t)(((p[0] >> 1) & 0x07) << 30) | ((int64_t)p[1] << 22) | ((int64_t)((p[2] >> 1) & 0x7F) << 15)
        | ((int64_t)p[3] << 7) | (int64_t)((p[4] >> 1) & 0x7F);
}

// Signed distance from origin on the 33-bit clock (handles the wrap-around every ~26 hours).
int64_t Unwrap(int64_t value, int64_t origin)
{
    const int64_t wrap = (int64_t)1 << 33;
    int64_t d = (value - origin) % wrap;
    if (d < 0) d += wrap;
    if (d >= wrap / 2) d -= wrap;
    return d;
}

void ParsePes(const std::vector<uint8_t>& p, PesUnit& unit)
{
    // 00 00 01 stream_id length(2) then, for most streams, flags(2) header_length(1) and the optional fields
    if (p.size() < 9 || p[0] != 0 || p[1] != 0 || p[2] != 1) return;
    uint8_t streamId = p[3];
    if (streamId == 0xBC || streamId == 0xBE || streamId == 0xBF || (streamId >= 0xF0 && streamId <= 0xF2) || streamId == 0xF8 || streamId == 0xFF)
    {
        unit.data.assign(p.begin() + 6, p.end());
        return;
    }
    uint8_t flags = p[7];
    size_t headerLen = p[8];
    size_t start = 9 + headerLen;
    if (start > p.size()) return;
    if ((flags & 0x80) && p.size() >= 14)
    {
        unit.hasPts = true;
        unit.pts = ReadTimestamp(&p[9]);
        unit.dts = unit.pts;
    }
    if ((flags & 0x40) && p.size() >= 19)
    {
        unit.hasDts = true;
        unit.dts = ReadTimestamp(&p[14]);
    }
    unit.data.assign(p.begin() + start, p.end());
}

// Splits Annex B data into NAL units (without start codes).
void SplitNals(const std::vector<uint8_t>& b, std::vector<std::pair<size_t, size_t> >& nals)
{
    size_t n = b.size();
    size_t i = 0;
    size_t cur = (size_t)-1;
    while (i + 2 < n)
    {
        if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
        {
            if (cur != (size_t)-1) nals.push_back(std::make_pair(cur, i - cur));
            i += 3;
            cur = i;
            continue;
        }
        i++;
    }
    if (cur != (size_t)-1 && cur < n)
    {
        // trailing zeros belong to the next start code (00 00 00 01): strip them
        size_t end = n;
        while (end > cur && b[end - 1] == 0) end--;
        nals.push_back(std::make_pair(cur, end - cur));
    }
}

void AppendStartCode(std::vector<uint8_t>& out, const uint8_t* nal, size_t len)
{
    out.push_back(0); out.push_back(0); out.push_back(0); out.push_back(1);
    out.insert(out.end(), nal, nal + len);
}

// Removes emulation prevention bytes (00 00 03) for the exp-Golomb reader.
class BitReader
{
public:
    explicit BitReader(const std::vector<uint8_t>& nal) : pos_(0)
    {
        int zeros = 0;
        for (size_t i = 0; i < nal.size(); i++)
        {
            uint8_t b = nal[i];
            if (zeros >= 2 && b == 3) { zeros = 0; continue; } // emulation prevention byte
            rbsp_.push_back(b);
            zeros = (b == 0) ? zeros + 1 : 0;
        }
    }
    uint32_t Bit()
    {
        size_t byte = pos_ >> 3;
        if (byte >= rbsp_.size()) { pos_++; return 0; }
        uint32_t v = (rbsp_[byte] >> (7 - (pos_ & 7))) & 1;
        pos_++;
        return v;
    }
    uint32_t Bits(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; i++) v = (v << 1) | Bit();
        return v;
    }
    uint32_t Ue()
    {
        int zeros = 0;
        while (Bit() == 0 && zeros < 32) zeros++;
        if (zeros == 0) return 0;
        return ((1u << zeros) - 1) + Bits(zeros);
    }
    int32_t Se()
    {
        uint32_t k = Ue();
        return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2);
    }
private:
    std::vector<uint8_t> rbsp_;
    size_t pos_;
};

void SkipScalingList(BitReader& r, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; j++)
    {
        if (next != 0)
        {
            int delta = r.Se();
            next = (last + delta + 256) % 256;
        }
        last = (next == 0) ? last : next;
    }
}

// Converts the RBSP of an SPS (without the NAL header byte handling: the header byte is skipped here).
bool SpsSize(const std::vector<uint8_t>& nal, int& width, int& height)
{
    if (nal.size() < 4) return false;
    BitReader r(std::vector<uint8_t>(nal.begin() + 1, nal.end()));
    int profile = r.Bits(8);
    r.Bits(8); // constraint flags
    r.Bits(8); // level
    r.Ue();    // sps id
    int chroma = 1;
    bool separate = false;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83
        || profile == 86 || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135)
    {
        chroma = (int)r.Ue();
        if (chroma == 3) separate = r.Bit() != 0;
        r.Ue(); // bit depth luma
        r.Ue(); // bit depth chroma
        r.Bit();
        if (r.Bit())
        {
            int lists = chroma != 3 ? 8 : 12;
            for (int i = 0; i < lists; i++)
                if (r.Bit()) SkipScalingList(r, i < 6 ? 16 : 64);
        }
    }
    r.Ue(); // log2 max frame num
    uint32_t pocType = r.Ue();
    if (pocType == 0) r.Ue();
    else if (pocType == 1)
    {
        r.Bit(); r.Se(); r.Se();
        uint32_t cycle = r.Ue();
        for (uint32_t i = 0; i < cycle && i < 256; i++) r.Se();
    }
    r.Ue(); // max ref frames
    r.Bit(); // gaps
    uint32_t wMbs = r.Ue() + 1;
    uint32_t hMapUnits = r.Ue() + 1;
    int frameMbsOnly = (int)r.Bit();
    if (!frameMbsOnly) r.Bit();
    r.Bit(); // direct 8x8
    uint32_t cl = 0, cr = 0, ct = 0, cb = 0;
    if (r.Bit()) { cl = r.Ue(); cr = r.Ue(); ct = r.Ue(); cb = r.Ue(); }
    if (separate) chroma = 0;
    int subW = (chroma == 1 || chroma == 2) ? 2 : 1;
    int subH = (chroma == 1) ? 2 : 1;
    int cropX = (chroma == 0) ? 1 : subW;
    int cropY = ((chroma == 0) ? 1 : subH) * (2 - frameMbsOnly);
    width = (int)(wMbs * 16 - (cl + cr) * cropX);
    height = (int)((2 - frameMbsOnly) * hMapUnits * 16 - (ct + cb) * cropY);
    return width > 0 && height > 0;
}

// Appends the ADTS frames of one buffer, the first one at time pts (90 kHz, relative to the segment base).
void AppendAdts(const std::vector<uint8_t>& b, int64_t pts, TsSegmentData& out)
{
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    size_t p = 0;
    while (p + 7 <= b.size())
    {
        if (b[p] != 0xFF || (b[p + 1] & 0xF0) != 0xF0) { p++; continue; }
        int protectionAbsent = b[p + 1] & 1;
        int profile = (b[p + 2] >> 6) & 3;          // AOT - 1
        int freqIndex = (b[p + 2] >> 2) & 0x0F;
        int channels = ((b[p + 2] & 1) << 2) | ((b[p + 3] >> 6) & 3);
        size_t frameLen = ((b[p + 3] & 0x03) << 11) | (b[p + 4] << 3) | ((b[p + 5] >> 5) & 0x07);
        size_t hdr = protectionAbsent ? 7 : 9;
        if (freqIndex > 12 || frameLen < hdr || p + frameLen > b.size()) { p++; continue; }
        if (out.audioSampleRate == 0)
        {
            out.audioSampleRate = rates[freqIndex];
            out.audioChannels = channels;
            out.audioSpecificConfig.clear();
            out.audioSpecificConfig.push_back((uint8_t)(((profile + 1) << 3) | (freqIndex >> 1)));
            out.audioSpecificConfig.push_back((uint8_t)(((freqIndex & 1) << 7) | (channels << 3)));
        }
        AudioFrame f;
        f.pts90k = pts;
        f.aac.assign(b.begin() + p + hdr, b.begin() + p + frameLen);
        out.audio.push_back(f);
        pts += (int64_t)1024 * 90000 / rates[freqIndex];
        p += frameLen;
    }
}

// Reads the base time of an ID3v2 tag (0 when absent) and returns the offset of the first frame after it.
size_t Id3Base(const std::vector<uint8_t>& b, int64_t& base)
{
    base = 0;
    if (b.size() < 10 || b[0] != 'I' || b[1] != 'D' || b[2] != '3') return 0;
    size_t size = ((size_t)(b[6] & 0x7F) << 21) | ((size_t)(b[7] & 0x7F) << 14) | ((size_t)(b[8] & 0x7F) << 7) | (b[9] & 0x7F);
    size_t end = std::min(b.size(), 10 + size);
    size_t p = 10;
    const char owner[] = "com.apple.streaming.transportStreamTimestamp";
    while (p + 10 <= end)
    {
        if (b[p] == 0) break;
        std::string id(b.begin() + p, b.begin() + p + 4);
        size_t fsize = ((size_t)b[p + 4] << 24) | ((size_t)b[p + 5] << 16) | ((size_t)b[p + 6] << 8) | b[p + 7];
        if (fsize > end - p - 10) break;
        if (id == "PRIV")
        {
            const uint8_t* pl = &b[p + 10];
            size_t plen = fsize;
            size_t ownerLen = std::strlen(owner);
            if (plen > ownerLen + 1 && std::memcmp(pl, owner, ownerLen) == 0 && plen >= ownerLen + 1 + 8)
            {
                const uint8_t* v = pl + plen - 8;
                int64_t ts = 0;
                for (int i = 0; i < 8; i++) ts = (ts << 8) | v[i];
                base = ts & (((int64_t)1 << 33) - 1);
            }
        }
        p += 10 + fsize;
    }
    return end;
}

}  // namespace

bool ParseSpsSize(const std::vector<uint8_t>& spsAnnexB, int& width, int& height)
{
    std::vector<std::pair<size_t, size_t> > nals;
    SplitNals(spsAnnexB, nals);
    for (size_t i = 0; i < nals.size(); i++)
    {
        std::vector<uint8_t> nal(spsAnnexB.begin() + nals[i].first, spsAnnexB.begin() + nals[i].first + nals[i].second);
        if (!nal.empty() && (nal[0] & 0x1F) == 7) return SpsSize(nal, width, height);
    }
    return false;
}

bool ParseTsSegment(const uint8_t* data, size_t size, TsSegmentData& out)
{
    out = TsSegmentData();
    int pmtPid = -1;
    int videoPid = -1, audioPid = -1;
    std::map<int, std::vector<PesUnit> > units; // pid -> PES packets in order
    std::map<int, PesUnit> open;                // pid -> PES being collected
    std::map<int, bool> started;

    auto flush = [&](int pid) {
        std::map<int, PesUnit>::iterator it = open.find(pid);
        if (it == open.end()) return;
        units[pid].push_back(it->second);
        open.erase(it);
    };

    for (size_t off = 0; off + kPacketSize <= size; off += kPacketSize)
    {
        const uint8_t* pk = data + off;
        if (pk[0] != 0x47) { out.error = "sincronizzazione TS persa"; return false; }
        bool pusi = (pk[1] & 0x40) != 0;
        int pid = ((pk[1] & 0x1F) << 8) | pk[2];
        int afc = (pk[3] >> 4) & 3;
        size_t pos = 4;
        if (afc & 2)
        {
            if (pos >= kPacketSize) continue;
            pos += 1 + pk[4];
        }
        if (!(afc & 1) || pos >= kPacketSize) continue;
        const uint8_t* payload = pk + pos;
        size_t plen = kPacketSize - pos;

        if (pid == kPatPid && pusi && plen > 1)
        {
            size_t p = 1 + payload[0];
            // PAT: table_id 0, section length; program entries of 4 bytes, the last 4 bytes are the CRC
            if (p + 8 <= plen && payload[p] == 0)
            {
                size_t secLen = ((payload[p + 1] & 0x0F) << 8) | payload[p + 2];
                size_t end = std::min(plen, p + 3 + secLen);
                for (size_t q = p + 8; q + 4 <= end - 4; q += 4)
                {
                    int program = (payload[q] << 8) | payload[q + 1];
                    if (program != 0 && pmtPid < 0) pmtPid = ((payload[q + 2] & 0x1F) << 8) | payload[q + 3];
                }
            }
            continue;
        }
        if (pid == pmtPid && pusi && plen > 1)
        {
            size_t p = 1 + payload[0];
            if (p + 12 <= plen && payload[p] == 2)
            {
                size_t secLen = ((payload[p + 1] & 0x0F) << 8) | payload[p + 2];
                size_t progInfo = ((payload[p + 10] & 0x0F) << 8) | payload[p + 11];
                size_t q = p + 12 + progInfo;
                size_t end = std::min(plen, p + 3 + secLen);
                while (q + 5 <= end - 4)
                {
                    uint8_t type = payload[q];
                    int elPid = ((payload[q + 1] & 0x1F) << 8) | payload[q + 2];
                    size_t esLen = ((payload[q + 3] & 0x0F) << 8) | payload[q + 4];
                    if (type == kStreamTypeH264 && videoPid < 0) videoPid = elPid;
                    if (type == kStreamTypeAdtsAac && audioPid < 0) audioPid = elPid;
                    q += 5 + esLen;
                }
            }
            continue;
        }
        if (pid != videoPid && pid != audioPid) continue;

        if (pusi)
        {
            flush(pid);
            PesUnit u;
            open[pid] = u;
            started[pid] = true;
            std::vector<uint8_t> head(payload, payload + plen);
            ParsePes(head, open[pid]);
        }
        else if (started[pid])
        {
            open[pid].data.insert(open[pid].data.end(), payload, payload + plen);
        }
    }
    for (std::map<int, PesUnit>::iterator it = open.begin(); it != open.end();)
    {
        std::map<int, PesUnit>::iterator cur = it++;
        units[cur->first].push_back(cur->second);
    }
    open.clear();

    if (pmtPid < 0) { out.error = "PAT/PMT non trovati"; return false; }

    // ---- video: one PES = one access unit (Annex B), SPS/PPS kept for the sequence header
    std::vector<PesUnit>& vu = units[videoPid];
    std::vector<PesUnit>& au = units[audioPid];
    bool hasVideo = videoPid >= 0 && !vu.empty();
    bool hasAudio = audioPid >= 0 && !au.empty();
    if (!hasVideo && !hasAudio) { out.error = "nessun PES audio o video"; return false; }

    // origin: first DTS of the video (or first PTS of the audio when there is no video), so no time is negative
    int64_t origin = 0;
    bool haveOrigin = false;
    if (hasVideo)
        for (size_t i = 0; i < vu.size() && !haveOrigin; i++)
            if (vu[i].hasPts) { origin = vu[i].hasDts ? vu[i].dts : vu[i].pts; haveOrigin = true; }
    if (!haveOrigin && hasAudio)
        for (size_t i = 0; i < au.size() && !haveOrigin; i++)
            if (au[i].hasPts) { origin = au[i].pts; haveOrigin = true; }
    // the video timeline starts at the DTS of the first access unit; audio PTS are on the same clock
    if (!haveOrigin) { out.error = "nessun PTS"; return false; }
    out.baseRaw = origin;

    if (hasVideo)
    {
        out.hasVideo = true;
        for (size_t i = 0; i < vu.size(); i++)
        {
            const PesUnit& u = vu[i];
            if (!u.hasPts) continue;
            VideoAccessUnit a;
            a.pts90k = Unwrap(u.pts, origin);
            a.dts90k = u.hasDts ? Unwrap(u.dts, origin) : a.pts90k;
            a.dur90k = 0;
            a.key = false;
            std::vector<std::pair<size_t, size_t> > nals;
            SplitNals(u.data, nals);
            std::vector<uint8_t> annexB;
            bool hasSps = false, hasPps = false;
            for (size_t n = 0; n < nals.size(); n++)
            {
                const uint8_t* nal = &u.data[nals[n].first];
                size_t len = nals[n].second;
                if (len == 0) continue;
                int type = nal[0] & 0x1F;
                if (type == 7)
                {
                    hasSps = true;
                    if (out.spsAnnexB.empty()) { out.spsAnnexB.clear(); AppendStartCode(out.spsAnnexB, nal, len); }
                }
                else if (type == 8)
                {
                    hasPps = true;
                    if (out.ppsAnnexB.empty()) { out.ppsAnnexB.clear(); AppendStartCode(out.ppsAnnexB, nal, len); }
                }
                else if (type == 5) a.key = true;
                AppendStartCode(annexB, nal, len);
            }
            // key frames always carry SPS and PPS in front, so the decoder can start at any segment
            if (a.key && (!hasSps || !hasPps) && !out.spsAnnexB.empty() && !out.ppsAnnexB.empty())
            {
                std::vector<uint8_t> prefix(out.spsAnnexB);
                prefix.insert(prefix.end(), out.ppsAnnexB.begin(), out.ppsAnnexB.end());
                prefix.insert(prefix.end(), annexB.begin(), annexB.end());
                annexB.swap(prefix);
            }
            a.annexB.swap(annexB);
            out.video.push_back(a);
        }
        // durations: distance to the next access unit in decoding order
        for (size_t i = 0; i < out.video.size(); i++)
        {
            if (i + 1 < out.video.size())
                out.video[i].dur90k = std::max<int64_t>(0, out.video[i + 1].dts90k - out.video[i].dts90k);
            else
                out.video[i].dur90k = i > 0 ? out.video[i - 1].dur90k : 3600;
        }
    }

    if (hasAudio)
    {
        out.hasAudio = true;
        for (size_t i = 0; i < au.size(); i++)
            if (au[i].hasPts) AppendAdts(au[i].data, Unwrap(au[i].pts, origin), out);
        if (out.audio.empty()) out.hasAudio = false;
    }

    if (!out.hasVideo && !out.hasAudio) { out.error = "nessun campione"; return false; }
    return true;
}

bool ParseAacSegment(const uint8_t* data, size_t size, TsSegmentData& out)
{
    out = TsSegmentData();
    std::vector<uint8_t> b(data, data + size);
    int64_t base = 0;
    size_t start = Id3Base(b, base);
    out.baseRaw = base;
    AppendAdts(std::vector<uint8_t>(b.begin() + start, b.end()), 0, out);
    out.hasAudio = !out.audio.empty();
    if (!out.hasAudio) { out.error = "nessun frame ADTS"; return false; }
    return true;
}

}  // namespace perle
