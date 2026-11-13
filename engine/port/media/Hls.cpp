// HTTP Live Streaming and MPEG transport streams for the Lumia port's media pipeline (PLAN §75.3 item 6, stage M3).
// Plain C++ like Demux.cpp, tested on the PC against ffprobe (engine/tools/demuxtest.py).
//
// A play list names segments; each is fetched whole, decrypted if it has an AES-128 key, and taken apart at once into
// the samples of its tracks: transport stream (H.264 + AAC/MP3 in PES packets), fragmented MP4 (EXT-X-MAP; Demux.cpp's
// reader over the bytes) or a bare run of AAC/MP3 frames. A master play list is answered with one variant, chosen
// once: the best the phone decodes in software. Audio in a play list of its own (EXT-X-MEDIA) is a second stream.
//
// Times: the samples' own (PES or tfdt) minus an offset fixed where the stream is entered, so that a segment starts
// at the sum of the durations before it; the offset is kept across segments (the stream's own times rule, not the
// play list's rounded ones) and found again at a discontinuity.

#if defined(LW_BUILDING)
#include "config.h"
#endif
#include "Demux.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>

namespace Lumia {
namespace Media {

namespace {

// The tallest picture taken from a master play list: the H.264 decoder runs on the CPU (PLAN §75.3 item 2).
constexpr int maxVariantHeight = 540;
constexpr long long maxVariantBandwidth = 1800000; // when the play list gives no resolution

constexpr uint64_t maxSegmentBytes = 96ull * 1024 * 1024;

// ---------------------------------------------------------------------------------------------------------------
// AES-128 CBC decryption (segments with EXT-X-KEY METHOD=AES-128).

struct Aes128 {
    uint8_t sbox[256], inverse[256];
    uint8_t roundKeys[176];

    static uint8_t rotl8(uint8_t x, int shift) { return static_cast<uint8_t>((x << shift) | (x >> (8 - shift))); }
    static uint8_t xtime(uint8_t x) { return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
    static uint8_t multiply(uint8_t a, uint8_t b)
    {
        uint8_t result = 0;
        while (b) {
            if (b & 1)
                result ^= a;
            a = xtime(a);
            b >>= 1;
        }
        return result;
    }

    explicit Aes128(const uint8_t key[16])
    {
        uint8_t p = 1, q = 1;
        do {
            p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q ^= static_cast<uint8_t>(q << 1);
            q ^= static_cast<uint8_t>(q << 2);
            q ^= static_cast<uint8_t>(q << 4);
            if (q & 0x80)
                q ^= 0x09;
            sbox[p] = static_cast<uint8_t>(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i)
            inverse[sbox[i]] = static_cast<uint8_t>(i);

        memcpy(roundKeys, key, 16);
        uint8_t rcon = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t t[4] = { roundKeys[i - 4], roundKeys[i - 3], roundKeys[i - 2], roundKeys[i - 1] };
            if (!(i % 16)) {
                uint8_t first = t[0];
                t[0] = static_cast<uint8_t>(sbox[t[1]] ^ rcon);
                t[1] = sbox[t[2]];
                t[2] = sbox[t[3]];
                t[3] = sbox[first];
                rcon = xtime(rcon);
            }
            for (int k = 0; k < 4; ++k)
                roundKeys[i + k] = static_cast<uint8_t>(roundKeys[i - 16 + k] ^ t[k]);
        }
    }

    void decryptBlock(uint8_t s[16]) const
    {
        auto addKey = [&](int round) {
            for (int i = 0; i < 16; ++i)
                s[i] ^= roundKeys[round * 16 + i];
        };
        auto invShiftSub = [&] {
            uint8_t t[16];
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row)
                    t[((column + row) % 4) * 4 + row] = inverse[s[column * 4 + row]];
            }
            memcpy(s, t, 16);
        };
        addKey(10);
        for (int round = 9; round >= 1; --round) {
            invShiftSub();
            addKey(round);
            for (int column = 0; column < 4; ++column) {
                uint8_t* c = s + column * 4;
                uint8_t a = c[0], b = c[1], d = c[2], e = c[3];
                c[0] = static_cast<uint8_t>(multiply(a, 14) ^ multiply(b, 11) ^ multiply(d, 13) ^ multiply(e, 9));
                c[1] = static_cast<uint8_t>(multiply(a, 9) ^ multiply(b, 14) ^ multiply(d, 11) ^ multiply(e, 13));
                c[2] = static_cast<uint8_t>(multiply(a, 13) ^ multiply(b, 9) ^ multiply(d, 14) ^ multiply(e, 11));
                c[3] = static_cast<uint8_t>(multiply(a, 11) ^ multiply(b, 13) ^ multiply(d, 9) ^ multiply(e, 14));
            }
        }
        invShiftSub();
        addKey(0);
    }
};

// In place; the padding (PKCS#7) is cut off. False: not a whole number of blocks.
bool decryptCbc(std::vector<uint8_t>& data, const uint8_t key[16], const uint8_t iv[16])
{
    if (data.empty() || data.size() % 16)
        return false;
    Aes128 aes(key);
    uint8_t previous[16], current[16];
    memcpy(previous, iv, 16);
    for (size_t at = 0; at < data.size(); at += 16) {
        memcpy(current, &data[at], 16);
        aes.decryptBlock(&data[at]);
        for (int i = 0; i < 16; ++i)
            data[at + i] ^= previous[i];
        memcpy(previous, current, 16);
    }
    uint8_t pad = data.back();
    if (pad >= 1 && pad <= 16)
        data.resize(data.size() - pad);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// H.264 in a byte stream

struct Nal {
    const uint8_t* data;
    size_t size;
};

// The NAL units between start codes.
template<typename Each>
void forEachNal(const uint8_t* data, size_t size, Each each)
{
    size_t at = 0;
    auto startCode = [&](size_t from) -> size_t {
        for (size_t i = from; i + 3 <= size; ++i) {
            if (!data[i] && !data[i + 1] && data[i + 2] == 1)
                return i;
        }
        return size;
    };
    at = startCode(0);
    while (at < size) {
        size_t begin = at + 3;
        size_t next = startCode(begin);
        size_t end = next;
        while (end > begin && next < size && !data[end - 1])
            --end; // the zero of a four-byte start code, trailing zeros
        if (end > begin && !each(Nal { data + begin, end - begin }))
            return;
        at = next;
    }
}

struct ExpGolomb {
    std::vector<uint8_t> bytes; // without the emulation prevention bytes
    size_t bit { 0 };
    bool overrun { false };

    ExpGolomb(const uint8_t* data, size_t size)
    {
        bytes.reserve(size);
        for (size_t i = 0; i < size; ++i) {
            if (i >= 2 && data[i] == 3 && !data[i - 1] && !data[i - 2])
                continue;
            bytes.push_back(data[i]);
        }
    }
    uint32_t u(int count)
    {
        uint32_t value = 0;
        while (count--) {
            uint32_t b = 0;
            if (bit / 8 < bytes.size())
                b = (bytes[bit / 8] >> (7 - bit % 8)) & 1;
            else
                overrun = true;
            value = (value << 1) | b;
            ++bit;
        }
        return value;
    }
    uint32_t ue()
    {
        int zeros = 0;
        while (!u(1) && zeros < 32 && !overrun)
            ++zeros;
        return zeros ? ((1u << zeros) - 1 + u(zeros)) : 0;
    }
    int32_t se()
    {
        uint32_t k = ue();
        return (k & 1) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }
};

} // namespace

bool parseH264Size(const uint8_t* sps, size_t size, int& width, int& height)
{
    if (size < 5 || (sps[0] & 0x1F) != 7)
        return false;
    ExpGolomb r(sps + 1, size - 1);
    uint32_t profile = r.u(8);
    r.u(16);
    r.ue();
    uint32_t chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86
        || profile == 118 || profile == 128 || profile == 138 || profile == 139 || profile == 134 || profile == 135) {
        chroma = r.ue();
        if (chroma == 3)
            r.u(1);
        r.ue();
        r.ue();
        r.u(1);
        if (r.u(1)) {
            int lists = chroma != 3 ? 8 : 12;
            for (int i = 0; i < lists; ++i) {
                if (!r.u(1))
                    continue;
                int last = 8, next = 8;
                for (int j = 0, count = i < 6 ? 16 : 64; j < count; ++j) {
                    if (next)
                        next = (last + r.se() + 256) % 256;
                    last = next ? next : last;
                }
            }
        }
    }
    r.ue();
    uint32_t pocType = r.ue();
    if (!pocType)
        r.ue();
    else if (pocType == 1) {
        r.u(1);
        r.se();
        r.se();
        uint32_t count = r.ue();
        for (uint32_t i = 0; i < count && i < 256; ++i)
            r.se();
    }
    r.ue();
    r.u(1);
    uint32_t widthMbs = r.ue() + 1;
    uint32_t heightUnits = r.ue() + 1;
    uint32_t frameOnly = r.u(1);
    if (!frameOnly)
        r.u(1);
    r.u(1);
    uint32_t left = 0, right = 0, top = 0, bottom = 0;
    if (r.u(1)) {
        left = r.ue();
        right = r.ue();
        top = r.ue();
        bottom = r.ue();
    }
    if (r.overrun || widthMbs > 1024 || heightUnits > 1024)
        return false;
    uint32_t unitX = (chroma == 1 || chroma == 2) ? 2 : 1;
    uint32_t unitY = (chroma == 1 ? 2 : 1) * (2 - frameOnly);
    long long w = static_cast<long long>(widthMbs) * 16 - static_cast<long long>(unitX) * (left + right);
    long long h = static_cast<long long>(2 - frameOnly) * heightUnits * 16 - static_cast<long long>(unitY) * (top + bottom);
    if (w <= 0 || h <= 0)
        return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Transport stream: 188-byte packets, a program's elementary streams in PES packets.

class TsParser {
public:
    std::vector<Sample> video, audio; // what the bytes fed since the last take hold, times in microseconds (33 bits unwrapped)
    Track videoTrack, audioTrack;
    bool hasVideo { false }, hasAudio { false };
    std::string unsupported;          // streams of the program the phone has no decoder for

    // A new timeline (a seek, a discontinuity): partial packets and the time unwrapping are forgotten, the tracks stay.
    void reset()
    {
        m_videoPes.clear();
        m_audioPes.clear();
        m_audioCarry.clear();
        m_haveVideoPes = m_haveAudioPes = false;
        m_haveReference = false;
        m_videoUnwrap = m_audioUnwrap = Unwrap();
        m_partial.clear();
        m_section.clear();
    }

    void feed(const uint8_t* data, size_t size)
    {
        size_t at = 0;
        if (!m_partial.empty()) {
            size_t need = 188 - m_partial.size();
            size_t take = std::min(need, size);
            m_partial.insert(m_partial.end(), data, data + take);
            at = take;
            if (m_partial.size() < 188)
                return;
            if (m_partial[0] == 0x47)
                packet(m_partial.data());
            m_partial.clear();
        }
        while (at < size) {
            if (data[at] != 0x47) {
                ++at; // lost sync: the next packet start that is followed by another
                while (at < size && !(data[at] == 0x47 && (at + 188 >= size || data[at + 188] == 0x47)))
                    ++at;
                continue;
            }
            if (size - at < 188) {
                m_partial.assign(data + at, data + size);
                return;
            }
            packet(data + at);
            at += 188;
        }
    }

    // The end of a segment: the PES packets still open are complete.
    void flush()
    {
        size_t firstVideo = m_videoFlushed;
        finishVideo();
        finishAudio();
        // A picture lasts until the next one in decode order; the last one as long as the one before it.
        for (size_t i = firstVideo; i < video.size(); ++i) {
            if (i + 1 < video.size())
                video[i].durationUs = std::max<long long>(video[i + 1].dtsUs - video[i].dtsUs, 0);
            else if (i > 0)
                video[i].durationUs = video[i - 1].durationUs;
        }
        m_videoFlushed = video.size();
    }

    void take(std::vector<Sample>& videoOut, std::vector<Sample>& audioOut)
    {
        videoOut.swap(video);
        audioOut.swap(audio);
        video.clear();
        audio.clear();
        m_videoFlushed = 0;
    }

private:
    struct Unwrap {
        bool have { false };
        uint64_t lastRaw { 0 };
        long long lastExtended { 0 };
    };

    long long unwrap(Unwrap& state, uint64_t raw)
    {
        if (!state.have) {
            if (!m_haveReference) {
                m_haveReference = true;
                m_referenceRaw = raw;
            }
            state.have = true;
            state.lastRaw = m_referenceRaw;
            state.lastExtended = static_cast<long long>(m_referenceRaw);
        }
        long long delta = static_cast<long long>((raw - state.lastRaw) & 0x1FFFFFFFFull);
        if (delta >= (1ll << 32))
            delta -= (1ll << 33);
        state.lastRaw = raw;
        state.lastExtended += delta;
        return state.lastExtended;
    }

    static long long toUs(long long ticks) { return ticks * 100 / 9; }

    void packet(const uint8_t* p)
    {
        int pid = ((p[1] & 0x1F) << 8) | p[2];
        bool start = p[1] & 0x40;
        int adaptation = (p[3] >> 4) & 3;
        if (!(adaptation & 1) || (p[1] & 0x80))
            return;
        size_t offset = 4;
        if (adaptation & 2)
            offset += 1 + p[4];
        if (offset >= 188)
            return;
        const uint8_t* payload = p + offset;
        size_t size = 188 - offset;
        if (!pid || pid == m_pmtPid) {
            section(pid, start, payload, size);
            return;
        }
        if (pid == m_videoPid) {
            if (start) {
                finishVideo();
                m_haveVideoPes = true;
            }
            if (m_haveVideoPes)
                m_videoPes.insert(m_videoPes.end(), payload, payload + size);
        } else if (pid == m_audioPid) {
            if (start) {
                finishAudio();
                m_haveAudioPes = true;
            }
            if (m_haveAudioPes)
                m_audioPes.insert(m_audioPes.end(), payload, payload + size);
        }
    }

    void section(int pid, bool start, const uint8_t* payload, size_t size)
    {
        if (start) {
            size_t pointer = payload[0];
            if (1 + pointer >= size)
                return;
            m_section.assign(payload + 1 + pointer, payload + size);
            m_sectionPid = pid;
        } else if (m_sectionPid == pid && !m_section.empty())
            m_section.insert(m_section.end(), payload, payload + size);
        else
            return;
        if (m_section.size() < 3)
            return;
        size_t length = 3 + (((m_section[1] & 0x0F) << 8) | m_section[2]);
        if (m_section.size() < length)
            return;
        std::vector<uint8_t> s(m_section.begin(), m_section.begin() + length);
        m_section.clear();
        if (length < 12)
            return;
        if (!pid && s[0] == 0) {
            for (size_t at = 8; at + 4 <= length - 4; at += 4) {
                int program = (s[at] << 8) | s[at + 1];
                if (program) {
                    m_pmtPid = ((s[at + 2] & 0x1F) << 8) | s[at + 3];
                    break;
                }
            }
            return;
        }
        if (pid != m_pmtPid || s[0] != 2 || m_haveProgram)
            return;
        size_t at = 12 + (((s[10] & 0x0F) << 8) | s[11]);
        while (at + 5 <= length - 4) {
            int type = s[at];
            int streamPid = ((s[at + 1] & 0x1F) << 8) | s[at + 2];
            size_t info = ((s[at + 3] & 0x0F) << 8) | s[at + 4];
            at += 5 + info;
            if (type == 0x1B && m_videoPid < 0)
                m_videoPid = streamPid;
            else if ((type == 0x0F || type == 0x03 || type == 0x04) && m_audioPid < 0) {
                m_audioPid = streamPid;
                m_audioAdts = type == 0x0F;
            } else if (type == 0x24)
                unsupported += unsupported.empty() ? "video hevc" : ", video hevc";
            else if (type == 0x81 || type == 0x87 || type == 0x06)
                unsupported += unsupported.empty() ? "audio ac-3 o dati privati" : ", audio ac-3 o dati privati";
            else if (type == 0x02 || type == 0x01)
                unsupported += unsupported.empty() ? "video mpeg-2" : ", video mpeg-2";
        }
        m_haveProgram = true;
    }

    // The PES header: where the payload starts, and the times. False: not a PES packet.
    static bool pesHeader(const std::vector<uint8_t>& pes, size_t& payload, bool& hasPts, uint64_t& pts, uint64_t& dts)
    {
        if (pes.size() < 9 || pes[0] || pes[1] || pes[2] != 1)
            return false;
        payload = 9 + pes[8];
        if (payload > pes.size())
            return false;
        auto stamp = [&](size_t at) {
            return (static_cast<uint64_t>((pes[at] >> 1) & 7) << 30) | (static_cast<uint64_t>(pes[at + 1]) << 22)
                | (static_cast<uint64_t>(pes[at + 2] >> 1) << 15) | (static_cast<uint64_t>(pes[at + 3]) << 7) | (pes[at + 4] >> 1);
        };
        hasPts = (pes[7] & 0x80) && pes[8] >= 5;
        if (hasPts) {
            pts = stamp(9);
            dts = ((pes[7] & 0x40) && pes[8] >= 10) ? stamp(14) : pts;
        }
        return true;
    }

    void finishVideo()
    {
        if (!m_haveVideoPes)
            return;
        m_haveVideoPes = false;
        std::vector<uint8_t> pes;
        pes.swap(m_videoPes);
        size_t payload = 0;
        bool hasPts = false;
        uint64_t pts = 0, dts = 0;
        if (!pesHeader(pes, payload, hasPts, pts, dts) || payload >= pes.size())
            return;
        const uint8_t* data = pes.data() + payload;
        size_t size = pes.size() - payload;
        if (!hasPts) {
            // The rest of the picture the PES before this one began.
            if (!video.empty())
                video.back().data.insert(video.back().data.end(), data, data + size);
            return;
        }
        Sample sample;
        // Decode times first: they never go back, so they carry the unwrapping; the picture's time is an offset.
        long long decode = unwrap(m_videoUnwrap, dts);
        long long offset = static_cast<long long>((pts - dts) & 0x1FFFFFFFFull);
        if (offset >= (1ll << 32))
            offset -= (1ll << 33);
        sample.dtsUs = toUs(decode);
        sample.ptsUs = toUs(decode + offset);
        bool key = false, recovery = false;
        std::vector<uint8_t> sps, pps;
        forEachNal(data, size, [&](Nal nal) {
            int type = nal.data[0] & 0x1F;
            if (type == 5)
                key = true;
            else if (type == 6 && nal.size >= 2 && nal.data[1] == 6)
                recovery = true;
            else if (type == 7 && sps.empty())
                sps.assign(nal.data, nal.data + nal.size);
            else if (type == 8 && pps.empty())
                pps.assign(nal.data, nal.data + nal.size);
            return true;
        });
        sample.key = key || recovery;
        if (!sps.empty() && !pps.empty() && !hasVideo) {
            int width = 0, height = 0;
            if (parseH264Size(sps.data(), sps.size(), width, height)) {
                hasVideo = true;
                videoTrack.video = true;
                videoTrack.codec = Codec::H264;
                videoTrack.name = "h264";
                videoTrack.width = width;
                videoTrack.height = height;
                static const uint8_t start[4] = { 0, 0, 0, 1 };
                videoTrack.config.assign(start, start + 4);
                videoTrack.config.insert(videoTrack.config.end(), sps.begin(), sps.end());
                videoTrack.config.insert(videoTrack.config.end(), start, start + 4);
                videoTrack.config.insert(videoTrack.config.end(), pps.begin(), pps.end());
            }
        }
        sample.data.assign(data, data + size);
        video.push_back(std::move(sample));
    }

    void finishAudio()
    {
        if (!m_haveAudioPes)
            return;
        m_haveAudioPes = false;
        std::vector<uint8_t> pes;
        pes.swap(m_audioPes);
        size_t payload = 0;
        bool hasPts = false;
        uint64_t pts = 0, dts = 0;
        if (!pesHeader(pes, payload, hasPts, pts, dts))
            return;
        // Frames may run over the end of a PES: what was left of the last one stands in front.
        size_t carried = m_audioCarry.size();
        std::vector<uint8_t> bytes;
        bytes.swap(m_audioCarry);
        bytes.insert(bytes.end(), pes.begin() + payload, pes.end());
        long long stampUs = hasPts ? toUs(unwrap(m_audioUnwrap, pts)) : 0;
        bool stamped = !hasPts;
        size_t at = 0;
        size_t headerSize = m_audioAdts ? 7 : 4;
        while (at + headerSize <= bytes.size()) {
            FrameInfo info;
            bool ok = m_audioAdts ? parseAdtsHeader(&bytes[at], info) : parseMp3Header(&bytes[at], info);
            if (!ok) {
                ++at;
                continue;
            }
            if (at + info.size > bytes.size())
                break;
            if (!stamped && at >= carried) {
                m_audioNextUs = stampUs;
                stamped = true;
            }
            if (!hasAudio) {
                hasAudio = true;
                audioTrack.video = false;
                audioTrack.sampleRate = info.rate;
                audioTrack.channels = info.channels;
                if (m_audioAdts) {
                    audioTrack.codec = Codec::AAC;
                    audioTrack.name = "aac";
                    unsigned config = (info.profile << 11) | (info.rateIndex << 7) | (info.channelConfig << 3);
                    audioTrack.config = { static_cast<uint8_t>(config >> 8), static_cast<uint8_t>(config) };
                } else {
                    audioTrack.codec = Codec::MP3;
                    audioTrack.name = "mp3";
                }
            }
            Sample sample;
            sample.key = true;
            sample.ptsUs = sample.dtsUs = m_audioNextUs;
            sample.durationUs = info.rate ? static_cast<long long>(info.samples) * 1000000 / info.rate : 0;
            sample.data.assign(bytes.begin() + at + info.skip, bytes.begin() + at + info.size);
            m_audioNextUs += sample.durationUs;
            audio.push_back(std::move(sample));
            at += info.size;
        }
        if (at < bytes.size())
            m_audioCarry.assign(bytes.begin() + at, bytes.end());
    }

    std::vector<uint8_t> m_partial;   // a packet cut by the end of the bytes fed
    std::vector<uint8_t> m_section;
    int m_sectionPid { -1 };
    int m_pmtPid { -1 }, m_videoPid { -1 }, m_audioPid { -1 };
    bool m_haveProgram { false };
    bool m_audioAdts { true };
    std::vector<uint8_t> m_videoPes, m_audioPes, m_audioCarry;
    bool m_haveVideoPes { false }, m_haveAudioPes { false };
    bool m_haveReference { false };
    uint64_t m_referenceRaw { 0 };
    Unwrap m_videoUnwrap, m_audioUnwrap;
    long long m_audioNextUs { 0 };
    size_t m_videoFlushed { 0 };
};

bool looksLikeTransportStream(const uint8_t* data, size_t size)
{
    return size >= 376 && data[0] == 0x47 && data[188] == 0x47;
}

// ---------------------------------------------------------------------------------------------------------------
// Play lists

struct Attributes {
    std::map<std::string, std::string> values;

    explicit Attributes(const std::string& text)
    {
        size_t at = 0;
        while (at < text.size()) {
            size_t equals = text.find('=', at);
            if (equals == std::string::npos)
                break;
            std::string name = text.substr(at, equals - at);
            while (!name.empty() && (name.front() == ' ' || name.front() == ','))
                name.erase(name.begin());
            std::string value;
            at = equals + 1;
            if (at < text.size() && text[at] == '"') {
                size_t close = text.find('"', at + 1);
                if (close == std::string::npos)
                    close = text.size();
                value = text.substr(at + 1, close - at - 1);
                at = close + 1;
            } else {
                size_t comma = text.find(',', at);
                if (comma == std::string::npos)
                    comma = text.size();
                value = text.substr(at, comma - at);
                at = comma;
            }
            values[name] = value;
            if (at < text.size() && text[at] == ',')
                ++at;
        }
    }
    bool has(const char* name) const { return values.count(name); }
    std::string get(const char* name) const
    {
        auto it = values.find(name);
        return it == values.end() ? std::string() : it->second;
    }
};

struct Segment {
    std::string reference;
    double duration { 0 };
    long long startUs { 0 };          // the sum of the durations before it in this play list
    uint64_t sequence { 0 };
    uint64_t discontinuity { 0 };     // how many discontinuities stand before it since the stream's start
    uint64_t rangeOffset { 0 }, rangeLength { 0 };
    int key { -1 };                   // index into MediaPlaylist::keys, -1 = clear
    bool hasIv { false };
    uint8_t iv[16] { };
    int map { -1 };                   // index into MediaPlaylist::maps
};

struct Key {
    std::string reference;
    bool supported { true };
};

struct Map {
    std::string reference;
    uint64_t rangeOffset { 0 }, rangeLength { 0 };
};

struct MediaPlaylist {
    std::string url;
    std::vector<Segment> segments;
    std::vector<Key> keys;
    std::vector<Map> maps;
    double targetDuration { 6 };
    bool ended { false };
    long long durationUs { 0 };
};

struct Variant {
    std::string reference;
    long long bandwidth { 0 };
    int width { 0 }, height { 0 };
    std::string codecs, audioGroup;
};

struct Rendition {
    std::string group, reference, name;
    bool isDefault { false };
};

struct MasterPlaylist {
    std::vector<Variant> variants;
    std::vector<Rendition> audio;
};

std::vector<std::string> lines(const std::vector<uint8_t>& bytes)
{
    std::vector<std::string> out;
    std::string line;
    size_t at = 0;
    if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        at = 3;
    for (; at <= bytes.size(); ++at) {
        if (at == bytes.size() || bytes[at] == '\n') {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            size_t lead = 0;
            while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t'))
                ++lead;
            if (lead < line.size())
                out.push_back(line.substr(lead));
            line.clear();
        } else
            line += static_cast<char>(bytes[at]);
    }
    return out;
}

bool startsWith(const std::string& s, const char* prefix)
{
    return !s.compare(0, strlen(prefix), prefix);
}

void parseRange(const std::string& text, uint64_t previousEnd, uint64_t& offset, uint64_t& length)
{
    length = strtoull(text.c_str(), nullptr, 10);
    size_t at = text.find('@');
    offset = at == std::string::npos ? previousEnd : strtoull(text.c_str() + at + 1, nullptr, 10);
}

bool parseHex(const std::string& text, uint8_t out[16])
{
    std::string hex = text;
    if (startsWith(hex, "0x") || startsWith(hex, "0X"))
        hex = hex.substr(2);
    if (hex.size() > 32)
        return false;
    hex = std::string(32 - hex.size(), '0') + hex;
    for (int i = 0; i < 16; ++i) {
        char byte[3] = { hex[2 * i], hex[2 * i + 1], 0 };
        char* end = nullptr;
        out[i] = static_cast<uint8_t>(strtoul(byte, &end, 16));
        if (end != byte + 2)
            return false;
    }
    return true;
}

bool isMaster(const std::vector<std::string>& text)
{
    for (auto& line : text) {
        if (startsWith(line, "#EXT-X-STREAM-INF"))
            return true;
        if (startsWith(line, "#EXTINF"))
            return false;
    }
    return false;
}

MasterPlaylist parseMaster(const std::vector<std::string>& text)
{
    MasterPlaylist master;
    bool pending = false;
    Variant variant;
    for (auto& line : text) {
        if (startsWith(line, "#EXT-X-STREAM-INF:")) {
            Attributes a(line.substr(18));
            variant = Variant();
            variant.bandwidth = strtoll(a.get("BANDWIDTH").c_str(), nullptr, 10);
            std::string resolution = a.get("RESOLUTION");
            size_t x = resolution.find_first_of("xX");
            if (x != std::string::npos) {
                variant.width = atoi(resolution.c_str());
                variant.height = atoi(resolution.c_str() + x + 1);
            }
            variant.codecs = a.get("CODECS");
            variant.audioGroup = a.get("AUDIO");
            pending = true;
        } else if (startsWith(line, "#EXT-X-MEDIA:")) {
            Attributes a(line.substr(13));
            if (a.get("TYPE") == "AUDIO") {
                Rendition r;
                r.group = a.get("GROUP-ID");
                r.reference = a.get("URI");
                r.name = a.get("NAME");
                r.isDefault = a.get("DEFAULT") == "YES";
                master.audio.push_back(r);
            }
        } else if (line[0] != '#' && pending) {
            variant.reference = line;
            master.variants.push_back(variant);
            pending = false;
        }
    }
    return master;
}

MediaPlaylist parseMedia(const std::vector<std::string>& text, const std::string& url)
{
    MediaPlaylist list;
    list.url = url;
    uint64_t sequence = 0, discontinuity = 0, rangeEnd = 0;
    int key = -1, map = -1;
    bool hasIv = false;
    uint8_t iv[16] { };
    Segment next;
    bool haveInfo = false;
    bool haveRange = false;
    long long startUs = 0;
    for (auto& line : text) {
        if (startsWith(line, "#EXTINF:")) {
            next.duration = atof(line.c_str() + 8);
            haveInfo = true;
        } else if (startsWith(line, "#EXT-X-TARGETDURATION:"))
            list.targetDuration = std::max(1.0, atof(line.c_str() + 22));
        else if (startsWith(line, "#EXT-X-MEDIA-SEQUENCE:"))
            sequence = strtoull(line.c_str() + 22, nullptr, 10);
        else if (startsWith(line, "#EXT-X-DISCONTINUITY-SEQUENCE:"))
            discontinuity = strtoull(line.c_str() + 30, nullptr, 10);
        else if (line == "#EXT-X-DISCONTINUITY")
            ++discontinuity;
        else if (line == "#EXT-X-ENDLIST")
            list.ended = true;
        else if (startsWith(line, "#EXT-X-BYTERANGE:")) {
            parseRange(line.substr(17), rangeEnd, next.rangeOffset, next.rangeLength);
            haveRange = true;
        } else if (startsWith(line, "#EXT-X-KEY:")) {
            Attributes a(line.substr(11));
            std::string method = a.get("METHOD");
            if (method == "NONE" || method.empty())
                key = -1;
            else {
                Key k;
                k.reference = a.get("URI");
                k.supported = method == "AES-128";
                list.keys.push_back(k);
                key = static_cast<int>(list.keys.size()) - 1;
            }
            hasIv = a.has("IV") && parseHex(a.get("IV"), iv);
        } else if (startsWith(line, "#EXT-X-MAP:")) {
            Attributes a(line.substr(11));
            Map m;
            m.reference = a.get("URI");
            if (a.has("BYTERANGE"))
                parseRange(a.get("BYTERANGE"), 0, m.rangeOffset, m.rangeLength);
            list.maps.push_back(m);
            map = static_cast<int>(list.maps.size()) - 1;
        } else if (line[0] != '#' && haveInfo) {
            next.reference = line;
            next.sequence = sequence++;
            next.discontinuity = discontinuity;
            next.key = key;
            next.map = map;
            next.hasIv = hasIv;
            memcpy(next.iv, iv, 16);
            next.startUs = startUs;
            if (haveRange)
                rangeEnd = next.rangeOffset + next.rangeLength;
            else
                rangeEnd = 0;
            startUs += static_cast<long long>(next.duration * 1000000.0 + 0.5);
            list.segments.push_back(next);
            next = Segment();
            haveInfo = haveRange = false;
        }
    }
    list.durationUs = startUs;
    return list;
}

// The variant to play: H.264 the phone decodes in software, the best one not over the limits; failing that the
// smallest. Variants in codecs it has no decoder for are left out when others exist.
const Variant* chooseVariant(const MasterPlaylist& master)
{
    auto playable = [](const Variant& v) {
        for (const char* other : { "hvc1", "hev1", "av01", "vp09", "vp8", "dvh1", "dvhe" }) {
            if (v.codecs.find(other) != std::string::npos)
                return false;
        }
        return true;
    };
    auto hasVideo = [](const Variant& v) {
        return v.height > 0 || v.codecs.empty() || v.codecs.find("avc") != std::string::npos;
    };
    std::vector<const Variant*> candidates;
    for (auto& v : master.variants) {
        if (playable(v) && hasVideo(v))
            candidates.push_back(&v);
    }
    if (candidates.empty()) {
        for (auto& v : master.variants) {
            if (playable(v))
                candidates.push_back(&v);
        }
    }
    if (candidates.empty()) {
        for (auto& v : master.variants)
            candidates.push_back(&v);
    }
    const Variant* best = nullptr;
    const Variant* smallest = nullptr;
    for (auto* v : candidates) {
        bool within = v->height > 0 ? v->height <= maxVariantHeight : v->bandwidth <= maxVariantBandwidth;
        if (within && (!best || v->bandwidth > best->bandwidth))
            best = v;
        if (!smallest || (v->height && smallest->height ? v->height < smallest->height : v->bandwidth < smallest->bandwidth))
            smallest = v;
    }
    return best ? best : smallest;
}

class MemorySource final : public ByteSource {
public:
    explicit MemorySource(const std::vector<uint8_t>& bytes)
        : m_bytes(bytes)
    {
    }
    long long size() final { return static_cast<long long>(m_bytes.size()); }
    bool read(unsigned long long offset, void* out, size_t size) final
    {
        if (offset > m_bytes.size() || size > m_bytes.size() - offset)
            return false;
        memcpy(out, m_bytes.data() + offset, size);
        return true;
    }

private:
    const std::vector<uint8_t>& m_bytes;
};

// ---------------------------------------------------------------------------------------------------------------
// One play list being played: where it stands, and the samples of the segments taken apart so far.

class HlsStream {
public:
    HlsStream(Fetcher& fetcher, std::map<uint64_t, long long>& offsets)
        : m_fetcher(fetcher)
        , m_offsets(offsets)
    {
    }

    std::deque<Sample> video, audio;
    Track videoTrack, audioTrack;
    bool hasVideo { false }, hasAudio { false };
    std::string note;
    bool failed { false };

    bool load(const std::string& base, const std::string& reference, std::string& error)
    {
        std::vector<uint8_t> bytes;
        std::string resolved;
        if (!m_fetcher.fetch(base, reference, 0, 0, bytes, resolved)) {
            error = "HLS: elenco non scaricato";
            return false;
        }
        return adopt(bytes, resolved, error);
    }

    bool adopt(const std::vector<uint8_t>& bytes, const std::string& url, std::string& error)
    {
        auto text = lines(bytes);
        if (text.empty() || !startsWith(text[0], "#EXTM3U")) {
            error = "HLS: non e' un elenco";
            return false;
        }
        m_list = parseMedia(text, url);
        if (m_list.segments.empty()) {
            error = "HLS: elenco senza segmenti";
            return false;
        }
        for (auto& key : m_list.keys) {
            if (!key.supported) {
                error = "HLS: cifratura non supportata (solo AES-128)";
                return false;
            }
        }
        return true;
    }

    bool live() const { return !m_list.ended; }
    long long durationUs() const { return m_list.ended ? m_list.durationUs : 0; }
    long long loadedEndUs() const { return m_timeUs; }

    // The first segment: tracks become known. A live stream is entered three segments from its end.
    bool start(std::string& error)
    {
        size_t first = 0;
        m_timeUs = 0;
        if (live() && m_list.segments.size() > 3)
            first = m_list.segments.size() - 3;
        m_nextSequence = m_list.segments[first].sequence;
        // A segment may hold one kind only at first (sound that starts late): a few are tried.
        for (int tries = 0; tries < 3 && !(hasVideo || hasAudio); ++tries) {
            if (!loadNext()) {
                error = m_error.empty() ? "HLS: primo segmento non letto" : m_error;
                return false;
            }
        }
        if (!hasVideo && !hasAudio) {
            error = note.empty() ? "HLS: nessuna traccia riproducibile" : "HLS: " + note;
            return false;
        }
        return true;
    }

    // Fetches and takes apart the next segment. False: the end of a finished list, a failure (failed), or stopped.
    bool loadNext()
    {
        for (;;) {
            size_t index = 0;
            if (find(index))
                return loadSegment(index);
            if (m_list.ended)
                return false;
            // Live: the list again, after a pause when it has nothing new.
            if (m_reloaded && !m_fetcher.wait(static_cast<int>(m_list.targetDuration * 500)))
                return false;
            m_reloaded = true;
            std::vector<uint8_t> bytes;
            std::string resolved;
            if (!m_fetcher.fetch(m_list.url, std::string(), 0, 0, bytes, resolved)) {
                if (++m_reloadFailures > 5) {
                    failed = true;
                    return false;
                }
                continue;
            }
            m_reloadFailures = 0;
            auto text = lines(bytes);
            if (text.empty() || !startsWith(text[0], "#EXTM3U"))
                continue;
            MediaPlaylist fresh = parseMedia(text, m_list.url);
            if (fresh.segments.empty())
                continue;
            m_list = std::move(fresh);
        }
    }

    // Seeks (a finished list only): the segment that holds the time, then the samples from the key frame at or
    // before it.
    void seek(long long timeUs)
    {
        video.clear();
        audio.clear();
        m_ts.reset();
        m_continuous = false;
        size_t index = 0;
        for (size_t i = 0; i < m_list.segments.size(); ++i) {
            if (m_list.segments[i].startUs <= timeUs)
                index = i;
        }
        m_nextSequence = m_list.segments[index].sequence;
        if (!loadNext())
            return;
        // Video: the last key frame at or before the time (the segment's first when none is).
        size_t from = 0;
        bool any = false;
        for (size_t i = 0; i < video.size(); ++i) {
            if (!video[i].key)
                continue;
            if (any && video[i].ptsUs > timeUs)
                break;
            from = i;
            any = true;
        }
        video.erase(video.begin(), video.begin() + static_cast<long>(from));
        while (!audio.empty() && audio.front().ptsUs + audio.front().durationUs <= timeUs)
            audio.pop_front();
    }

    // Streams of one presentation entered together share a timeline: the earlier of their first times is its zero.
    long long offset() const { return m_offset; }
    long long ownOffset() const { return m_ownOffset; } // what this stream's first times alone say
    void rebase(long long offset)
    {
        long long delta = offset - m_offset;
        for (auto* queue : { &video, &audio }) {
            for (auto& sample : *queue) {
                sample.ptsUs -= delta;
                sample.dtsUs -= delta;
            }
        }
        m_offset = offset;
    }

private:
    bool find(size_t& index)
    {
        if (m_list.segments.empty())
            return false;
        // Fallen behind a live list: its first segment.
        if (m_nextSequence < m_list.segments.front().sequence)
            m_nextSequence = m_list.segments.front().sequence;
        uint64_t at = m_nextSequence - m_list.segments.front().sequence;
        if (at >= m_list.segments.size())
            return false;
        index = static_cast<size_t>(at);
        return true;
    }

    bool fetchSegment(const Segment& segment, std::vector<uint8_t>& bytes)
    {
        std::string resolved;
        if (!m_fetcher.fetch(m_list.url, segment.reference, segment.rangeOffset, segment.rangeLength, bytes, resolved))
            return false;
        if (segment.key < 0)
            return true;
        const Key& key = m_list.keys[segment.key];
        if (m_keyReference != key.reference) {
            std::vector<uint8_t> keyBytes;
            if (!m_fetcher.fetch(m_list.url, key.reference, 0, 0, keyBytes, resolved) || keyBytes.size() != 16) {
                m_error = "HLS: chiave non scaricata";
                return false;
            }
            memcpy(m_key, keyBytes.data(), 16);
            m_keyReference = key.reference;
        }
        uint8_t iv[16] { };
        if (segment.hasIv)
            memcpy(iv, segment.iv, 16);
        else {
            for (int i = 0; i < 8; ++i)
                iv[15 - i] = static_cast<uint8_t>(segment.sequence >> (8 * i));
        }
        if (!decryptCbc(bytes, m_key, iv)) {
            m_error = "HLS: segmento cifrato di lunghezza sbagliata";
            return false;
        }
        return true;
    }

    bool loadSegment(size_t index)
    {
        Segment segment = m_list.segments[index];
        std::vector<uint8_t> bytes;
        if (!fetchSegment(segment, bytes)) {
            failed = true;
            return false;
        }
        m_reloaded = false;
        m_nextSequence = segment.sequence + 1;
        // The one after it, while this one plays.
        if (index + 1 < m_list.segments.size()) {
            const Segment& following = m_list.segments[index + 1];
            m_fetcher.prefetch(m_list.url, following.reference, following.rangeOffset, following.rangeLength);
        }

        bool sameTimeline = m_continuous && segment.discontinuity == m_discontinuity;
        if (!sameTimeline)
            m_ts.reset();
        std::vector<Sample> newVideo, newAudio;
        bool ownTimes = true; // false: the segment's samples start at zero (bare audio frames)
        const uint8_t* data = bytes.data();
        size_t size = bytes.size();
        // An ID3 tag in front (Apple's bare audio segments carry their time in one).
        while (size >= 10 && !memcmp(data, "ID3", 3)) {
            size_t tag = 10 + ((data[6] & 0x7F) << 21 | (data[7] & 0x7F) << 14 | (data[8] & 0x7F) << 7 | (data[9] & 0x7F));
            if (tag > size)
                break;
            data += tag;
            size -= tag;
        }
        if (looksLikeTransportStream(data, size)) {
            m_ts.feed(data, size);
            m_ts.flush();
            m_ts.take(newVideo, newAudio);
            if (m_ts.hasVideo && !hasVideo) {
                hasVideo = true;
                videoTrack = m_ts.videoTrack;
            }
            if (m_ts.hasAudio && !hasAudio) {
                hasAudio = true;
                audioTrack = m_ts.audioTrack;
            }
            if (!m_ts.unsupported.empty())
                note = m_ts.unsupported + " senza decoder sul telefono";
            m_container = "hls (ts)";
        } else {
            // Fragmented MP4 after its initialization segment, or bare frames: Demux.cpp's readers over the bytes.
            std::vector<uint8_t> whole;
            bool mp4 = size >= 8 && (!memcmp(data + 4, "styp", 4) || !memcmp(data + 4, "moof", 4) || !memcmp(data + 4, "ftyp", 4)
                || !memcmp(data + 4, "sidx", 4) || !memcmp(data + 4, "emsg", 4) || !memcmp(data + 4, "prft", 4));
            if (mp4 && segment.map >= 0) {
                if (m_mapIndex != segment.map) {
                    const Map& map = m_list.maps[segment.map];
                    std::string resolved;
                    m_mapBytes.clear();
                    if (!m_fetcher.fetch(m_list.url, map.reference, map.rangeOffset, map.rangeLength, m_mapBytes, resolved)) {
                        failed = true;
                        return false;
                    }
                    m_mapIndex = segment.map;
                }
                whole = m_mapBytes;
            }
            whole.insert(whole.end(), data, data + size);
            MemorySource source(whole);
            std::string error;
            auto demuxer = Demuxer::open(source, error);
            if (!demuxer) {
                m_error = "HLS: segmento non letto: " + error;
                failed = true;
                return false;
            }
            ownTimes = mp4;
            m_container = mp4 ? "hls (mp4)" : "hls (audio)";
            for (size_t i = 0; i < demuxer->tracks().size(); ++i) {
                const Track& track = demuxer->tracks()[i];
                auto& out = track.video ? newVideo : newAudio;
                Sample sample;
                while (demuxer->read(i, sample))
                    out.push_back(std::move(sample));
                if (track.video && !hasVideo) {
                    hasVideo = true;
                    videoTrack = track;
                } else if (!track.video && !hasAudio) {
                    hasAudio = true;
                    audioTrack = track;
                }
            }
            if (!error.empty())
                note = error;
        }

        // From the stream's times to the play list's.
        long long startUs = live() ? m_timeUs : segment.startUs;
        long long first = 0;
        bool any = false;
        for (auto* list : { &newVideo, &newAudio }) {
            for (auto& sample : *list) {
                if (!any || sample.ptsUs < first)
                    first = sample.ptsUs;
                any = true;
                if (sample.ptsUs > first + 2000000)
                    break; // the smallest time is among the first samples
            }
        }
        long long offset = 0;
        if (!ownTimes)
            offset = -startUs;
        else if (sameTimeline)
            offset = m_offset;
        else {
            offset = m_ownOffset = first - startUs;
            // The other stream of the same presentation (sound in its own play list) shares the timeline: its
            // offset, when it is the same but for a frame or two.
            auto known = m_offsets.find(segment.discontinuity);
            if (known != m_offsets.end() && std::llabs(known->second - offset) < 1000000 && !live())
                offset = known->second;
            else if (known == m_offsets.end())
                m_offsets[segment.discontinuity] = offset;
        }
        m_offset = offset;
        m_discontinuity = segment.discontinuity;
        m_continuous = true;
        m_timeUs = startUs + static_cast<long long>(segment.duration * 1000000.0 + 0.5);
        for (auto& sample : newVideo) {
            sample.ptsUs -= offset;
            sample.dtsUs -= offset;
            video.push_back(std::move(sample));
        }
        for (auto& sample : newAudio) {
            sample.ptsUs -= offset;
            sample.dtsUs -= offset;
            audio.push_back(std::move(sample));
        }
        return true;
    }

public:
    const char* container() const { return m_container; }

private:
    Fetcher& m_fetcher;
    std::map<uint64_t, long long>& m_offsets;
    MediaPlaylist m_list;
    TsParser m_ts;
    uint64_t m_nextSequence { 0 };
    long long m_timeUs { 0 };      // live: where the next segment starts on our timeline
    long long m_offset { 0 }, m_ownOffset { 0 };
    uint64_t m_discontinuity { 0 };
    bool m_continuous { false };
    bool m_reloaded { false };
    int m_reloadFailures { 0 };
    std::string m_keyReference;
    uint8_t m_key[16] { };
    int m_mapIndex { -1 };
    std::vector<uint8_t> m_mapBytes;
    std::string m_error;
    const char* m_container { "hls" };
};

class HlsDemuxer final : public Demuxer {
public:
    explicit HlsDemuxer(Fetcher& fetcher)
        : m_fetcher(fetcher)
        , m_main(fetcher, m_offsets)
    {
    }

    bool open(const std::string& url, std::string& error)
    {
        std::vector<uint8_t> bytes;
        std::string resolved;
        if (!m_fetcher.fetch(url, std::string(), 0, 0, bytes, resolved)) {
            error = "HLS: elenco non scaricato";
            return false;
        }
        auto text = lines(bytes);
        if (text.empty() || !startsWith(text[0], "#EXTM3U")) {
            error = "HLS: non e' un elenco";
            return false;
        }
        std::string audioReference;
        if (isMaster(text)) {
            MasterPlaylist master = parseMaster(text);
            const Variant* variant = chooseVariant(master);
            if (!variant) {
                error = "HLS: elenco principale senza varianti";
                return false;
            }
            m_description = "variante " + std::to_string(variant->width) + "x" + std::to_string(variant->height) + " "
                + std::to_string(variant->bandwidth / 1000) + " kbit/s di " + std::to_string(master.variants.size());
            if (!variant->audioGroup.empty()) {
                const Rendition* chosen = nullptr;
                for (auto& r : master.audio) {
                    if (r.group != variant->audioGroup)
                        continue;
                    if (!chosen || (r.isDefault && !chosen->isDefault))
                        chosen = &r;
                }
                if (chosen)
                    audioReference = chosen->reference; // empty: the sound is in the variant's own segments
            }
            if (!m_main.load(resolved, variant->reference, error))
                return false;
        } else if (!m_main.adopt(bytes, resolved, error))
            return false;
        if (!m_main.start(error))
            return false;
        if (!audioReference.empty()) {
            m_audio = std::make_unique<HlsStream>(m_fetcher, m_offsets);
            std::string audioError;
            if (!m_audio->load(resolved, audioReference, audioError) || !m_audio->start(audioError) || !m_audio->hasAudio) {
                error = audioError;
                m_audio = nullptr;
            }
        }
        if (m_audio && !live() && std::llabs(m_audio->ownOffset() - m_main.ownOffset()) < 1000000) {
            long long common = std::min(m_audio->ownOffset(), m_main.ownOffset());
            m_main.rebase(common);
            m_audio->rebase(common);
            for (auto& entry : m_offsets)
                entry.second = common;
        }
        if (m_main.hasVideo) {
            m_tracks.push_back(m_main.videoTrack);
            m_sources.push_back({ &m_main, true });
        }
        if (m_audio) {
            m_tracks.push_back(m_audio->audioTrack);
            m_sources.push_back({ m_audio.get(), false });
        } else if (m_main.hasAudio) {
            m_tracks.push_back(m_main.audioTrack);
            m_sources.push_back({ &m_main, false });
        }
        m_durationUs = m_main.durationUs();
        for (auto& track : m_tracks)
            track.durationUs = m_durationUs;
        if (error.empty())
            error = m_main.note;
        if (!m_description.empty())
            error = error.empty() ? m_description : m_description + "; " + error;
        return true;
    }

    const char* container() const final { return m_main.container(); }
    bool live() const final { return m_main.live(); }
    long long bufferedEndUs() const final { return m_audio ? std::min(m_main.loadedEndUs(), m_audio->loadedEndUs()) : m_main.loadedEndUs(); }

    bool read(size_t index, Sample& sample) final
    {
        if (index >= m_sources.size())
            return false;
        HlsStream& stream = *m_sources[index].stream;
        auto& queue = m_sources[index].video ? stream.video : stream.audio;
        while (queue.empty()) {
            if (!stream.loadNext()) {
                m_failed = stream.failed;
                return false;
            }
        }
        sample = std::move(queue.front());
        queue.pop_front();
        // What a stream holds for a track nobody reads (sound doubled by a play list of its own) is not kept.
        if (m_audio && &stream == &m_main)
            m_main.audio.clear();
        return true;
    }

    void seek(long long timeUs) final
    {
        if (live())
            return;
        if (timeUs < 0)
            timeUs = 0;
        m_main.seek(timeUs);
        if (m_audio)
            m_audio->seek(timeUs);
    }

private:
    struct Source {
        HlsStream* stream;
        bool video;
    };

    Fetcher& m_fetcher;
    std::map<uint64_t, long long> m_offsets;
    HlsStream m_main;
    std::unique_ptr<HlsStream> m_audio;
    std::vector<Source> m_sources;
    std::string m_description;
};

// A transport stream on its own (rare on the web): read whole, as one segment.
class TsFileDemuxer final : public Demuxer {
public:
    bool open(ByteSource& source, std::string& error)
    {
        long long total = source.size();
        if (total <= 0 || static_cast<uint64_t>(total) > maxSegmentBytes) {
            error = "flusso di trasporto senza lunghezza o troppo grande";
            return false;
        }
        TsParser parser;
        std::vector<uint8_t> block(256 * 188);
        for (uint64_t at = 0; at < static_cast<uint64_t>(total); at += block.size()) {
            size_t size = static_cast<size_t>(std::min<uint64_t>(block.size(), static_cast<uint64_t>(total) - at));
            if (!source.read(at, block.data(), size)) {
                error = "flusso di trasporto illeggibile";
                return false;
            }
            parser.feed(block.data(), size);
        }
        parser.flush();
        std::vector<Sample> video, audio;
        parser.take(video, audio);
        long long first = 0;
        bool any = false;
        for (auto* list : { &video, &audio }) {
            for (auto& sample : *list) {
                if (!any || sample.ptsUs < first)
                    first = sample.ptsUs;
                any = true;
            }
        }
        for (auto* list : { &video, &audio }) {
            for (auto& sample : *list) {
                sample.ptsUs -= first;
                sample.dtsUs -= first;
                m_durationUs = std::max(m_durationUs, sample.ptsUs + sample.durationUs);
            }
        }
        if (parser.hasVideo && !video.empty()) {
            m_tracks.push_back(parser.videoTrack);
            m_samples.push_back(std::move(video));
        }
        if (parser.hasAudio && !audio.empty()) {
            m_tracks.push_back(parser.audioTrack);
            m_samples.push_back(std::move(audio));
        }
        m_next.assign(m_tracks.size(), 0);
        for (auto& track : m_tracks)
            track.durationUs = m_durationUs;
        if (m_tracks.empty()) {
            error = parser.unsupported.empty() ? "flusso di trasporto senza tracce riproducibili" : parser.unsupported + " senza decoder sul telefono";
            return false;
        }
        return true;
    }

    const char* container() const final { return "mpeg-ts"; }

    bool read(size_t index, Sample& sample) final
    {
        if (index >= m_samples.size() || m_next[index] >= m_samples[index].size())
            return false;
        sample = m_samples[index][m_next[index]++];
        return true;
    }

    void seek(long long timeUs) final
    {
        for (size_t t = 0; t < m_samples.size(); ++t) {
            auto& samples = m_samples[t];
            size_t found = 0;
            if (m_tracks[t].video) {
                bool any = false;
                for (size_t i = 0; i < samples.size(); ++i) {
                    if (!samples[i].key)
                        continue;
                    if (any && samples[i].ptsUs > timeUs)
                        break;
                    found = i;
                    any = true;
                }
            } else {
                while (found < samples.size() && samples[found].ptsUs + samples[found].durationUs <= timeUs)
                    ++found;
            }
            m_next[t] = found;
        }
    }

private:
    std::vector<std::vector<Sample>> m_samples;
    std::vector<size_t> m_next;
};

} // namespace

std::unique_ptr<Demuxer> openHls(Fetcher& fetcher, const std::string& url, std::string& error)
{
    auto demuxer = std::make_unique<HlsDemuxer>(fetcher);
    if (!demuxer->open(url, error))
        return nullptr;
    return demuxer;
}

std::unique_ptr<Demuxer> openTransportStream(ByteSource& source, std::string& error)
{
    auto demuxer = std::make_unique<TsFileDemuxer>();
    if (!demuxer->open(source, error))
        return nullptr;
    return demuxer;
}

} // namespace Media
} // namespace Lumia
