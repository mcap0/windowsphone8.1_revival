#if defined(LW_BUILDING)
#include "config.h"
#endif
#include "Demux.h"

#include <algorithm>
#include <cstring>

namespace Lumia {
namespace Media {

namespace {

uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t be32(const uint8_t* p) { return (static_cast<uint32_t>(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }

constexpr uint32_t tag(const char (&s)[5])
{
    return (static_cast<uint32_t>(static_cast<uint8_t>(s[0])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(s[1])) << 16)
        | (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(s[3]));
}

std::string tagName(uint32_t t)
{
    std::string s;
    for (int shift = 24; shift >= 0; shift -= 8) {
        char c = static_cast<char>(t >> shift);
        s += (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

// value / timescale seconds, in microseconds, without overflowing on long media.
long long toUs(long long value, long long timescale)
{
    if (timescale <= 0)
        return 0;
    return (value / timescale) * 1000000 + (value % timescale) * 1000000 / timescale;
}

// ---------------------------------------------------------------------------------------------------------------
// AAC

const int aacRates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };

struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t bit { 0 };
    uint32_t get(int count)
    {
        uint32_t value = 0;
        while (count--) {
            uint32_t b = 0;
            if (bit / 8 < size)
                b = (data[bit / 8] >> (7 - bit % 8)) & 1;
            value = (value << 1) | b;
            ++bit;
        }
        return value;
    }
};

// Output rate and channels an AudioSpecificConfig declares (explicit SBR/PS signalling included).
void describeAudioSpecificConfig(const std::vector<uint8_t>& config, int& rate, int& channels)
{
    if (config.size() < 2)
        return;
    BitReader r { config.data(), config.size() };
    auto objectType = [&] {
        uint32_t t = r.get(5);
        return t == 31 ? 32 + r.get(6) : t;
    };
    auto frequency = [&]() -> int {
        uint32_t i = r.get(4);
        if (i == 15)
            return static_cast<int>(r.get(24));
        return i < 13 ? aacRates[i] : 0;
    };
    uint32_t type = objectType();
    int f = frequency();
    uint32_t ch = r.get(4);
    if (type == 5 || type == 29) {
        int extension = frequency();
        if (extension)
            f = extension;
    }
    if (f)
        rate = f;
    if (ch)
        channels = ch == 7 ? 8 : static_cast<int>(ch);
    if (type == 29)
        channels = 2; // parametric stereo: a mono core decoded to two channels
}

// ---------------------------------------------------------------------------------------------------------------
// MP4

struct Box {
    uint32_t type { 0 };
    const uint8_t* data { nullptr }; // payload
    size_t size { 0 };
};

// Walks the boxes inside [p, end). False at the end, or at a box that does not fit.
bool nextBox(const uint8_t*& p, const uint8_t* end, Box& box)
{
    if (end - p < 8)
        return false;
    uint64_t size = be32(p);
    size_t header = 8;
    if (size == 1) {
        if (end - p < 16)
            return false;
        size = be64(p + 8);
        header = 16;
    } else if (!size)
        size = static_cast<uint64_t>(end - p);
    if (size < header || size > static_cast<uint64_t>(end - p))
        return false;
    box.type = be32(p + 4);
    box.data = p + header;
    box.size = static_cast<size_t>(size) - header;
    p += size;
    return true;
}

bool findBox(const Box& parent, uint32_t type, Box& out, size_t skip = 0)
{
    if (parent.size < skip)
        return false;
    const uint8_t* p = parent.data + skip;
    const uint8_t* end = parent.data + parent.size;
    Box box;
    while (nextBox(p, end, box)) {
        if (box.type == type) {
            out = box;
            return true;
        }
    }
    return false;
}

struct Entry {
    uint64_t offset;
    int64_t dts;        // track timescale
    uint32_t size;
    int32_t cts;        // composition offset
    uint32_t duration;
    bool key;
};

struct Mp4Track {
    uint32_t id { 0 };
    uint32_t timescale { 0 };
    int64_t mediaTime { 0 };   // edit list: the media time shown at the track's start (track timescale)
    long long delayUs { 0 };   // edit list: an empty edit before it
    int nalLength { 0 };       // H.264: bytes of each NAL length (avcC)
    bool video { false };
    std::vector<Entry> table;
    size_t next { 0 };
    // Fragments.
    uint32_t defaultDuration { 0 }, defaultSize { 0 }, defaultFlags { 0 };
    int64_t nextDts { 0 };
    // A fragmented file whose first samples are in moov (ffmpeg's frag_keyframe without empty_moov): those samples,
    // put back when the fragments are read again from the start.
    std::vector<Entry> initial;
    int64_t initialNextDts { 0 };

    long long pts(const Entry& e) const { return toUs(e.dts + e.cts - mediaTime, timescale) + delayUs; }
    long long dts(const Entry& e) const { return toUs(e.dts - mediaTime, timescale) + delayUs; }
};

// Length-prefixed NAL units (ISO 14496-15) to start codes. Four-byte lengths are rewritten in place.
void toAnnexB(std::vector<uint8_t>& data, int nalLength)
{
    if (nalLength == 4) {
        size_t p = 0;
        while (p + 4 <= data.size()) {
            uint32_t length = be32(&data[p]);
            data[p] = 0;
            data[p + 1] = 0;
            data[p + 2] = 0;
            data[p + 3] = 1;
            if (length > data.size() - p - 4) {
                data.resize(p); // a broken unit: what came before it is still good
                return;
            }
            p += 4 + length;
        }
        return;
    }
    std::vector<uint8_t> out;
    out.reserve(data.size() + 32);
    size_t p = 0;
    while (p + nalLength <= data.size()) {
        uint32_t length = 0;
        for (int i = 0; i < nalLength; ++i)
            length = (length << 8) | data[p + i];
        p += nalLength;
        if (length > data.size() - p)
            break;
        static const uint8_t start[4] = { 0, 0, 0, 1 };
        out.insert(out.end(), start, start + 4);
        out.insert(out.end(), data.begin() + p, data.begin() + p + length);
        p += length;
    }
    data.swap(out);
}

class Mp4Demuxer final : public Demuxer {
public:
    explicit Mp4Demuxer(ByteSource& source)
        : m_source(source)
    {
    }

    const char* container() const final { return m_fragmented ? "mp4 a frammenti" : "mp4"; }

    bool open(std::string& error)
    {
        // Top level: moov may come after mdat (a file not made for the web): the boxes before it are skipped by
        // their headers alone.
        uint64_t offset = 0;
        std::vector<uint8_t> moov;
        for (int guard = 0; guard < 10000; ++guard) {
            uint32_t type = 0;
            uint64_t size = 0;
            size_t header = 0;
            if (!boxHeader(offset, type, size, header)) {
                error = "mp4 senza moov";
                return false;
            }
            if (type == tag("moov")) {
                if (size > 256 * 1024 * 1024) {
                    error = "moov troppo grande";
                    return false;
                }
                moov.resize(static_cast<size_t>(size - header));
                if (!m_source.read(offset + header, moov.data(), moov.size())) {
                    error = "moov illeggibile";
                    return false;
                }
                offset += size;
                break;
            }
            offset += size;
        }
        if (!parseMoov(moov, error))
            return false;
        if (m_tracks.empty()) {
            error = error.empty() ? "nessuna traccia riproducibile" : error;
            return false;
        }
        error.clear();

        if (m_fragmented) {
            // Up to the first moof; a segment index on the way.
            for (int guard = 0; guard < 64; ++guard) {
                uint32_t type = 0;
                uint64_t size = 0;
                size_t header = 0;
                if (!boxHeader(offset, type, size, header))
                    break;
                if (type == tag("moof"))
                    break;
                if (type == tag("sidx") && size < 16 * 1024 * 1024) {
                    std::vector<uint8_t> sidx(static_cast<size_t>(size - header));
                    if (m_source.read(offset + header, sidx.data(), sidx.size()))
                        parseSidx(sidx, offset + size);
                }
                offset += size;
            }
            m_firstFragment = m_scan = offset;
            if (m_index.empty())
                readMfra();
            calibrateIndex();
            if (!m_durationUs && !m_index.empty())
                m_durationUs = endOfLastFragment();
            if (!m_durationUs)
                m_durationUs = m_moovSamplesEndUs; // no index: what moov holds, as a plain file would say
        }
        for (size_t i = 0; i < m_tracks.size(); ++i) {
            if (!m_tracks[i].durationUs)
                m_tracks[i].durationUs = m_durationUs;
        }
        return true;
    }

    bool read(size_t index, Sample& sample) final
    {
        if (index >= m_states.size())
            return false;
        auto& t = m_states[index];
        while (t.next >= t.table.size()) {
            if (!m_fragmented || !loadNextFragment())
                return false;
        }
        const Entry& e = t.table[t.next];
        sample.data.resize(e.size);
        if (e.size && !m_source.read(e.offset, sample.data.data(), e.size)) {
            m_failed = true;
            return false;
        }
        sample.ptsUs = t.pts(e);
        sample.dtsUs = t.dts(e);
        sample.durationUs = toUs(e.duration, t.timescale);
        sample.key = e.key;
        if (t.nalLength)
            toAnnexB(sample.data, t.nalLength);
        ++t.next;
        if (m_fragmented && t.next >= 2048) {
            t.table.erase(t.table.begin(), t.table.begin() + t.next);
            t.next = 0;
        }
        return true;
    }

    void seek(long long timeUs) final
    {
        if (timeUs < 0)
            timeUs = 0;
        if (m_fragmented) {
            // The fragment that holds the time for each indexed track (each its own fragments when a fragment holds
            // one track only): the earliest of them.
            uint64_t from = m_firstFragment;
            if (!m_trackIndex.empty()) {
                from = UINT64_MAX;
                for (auto& [id, points] : m_trackIndex) {
                    uint64_t trackFrom = m_firstFragment;
                    for (auto& point : points) {
                        if (point.timeUs > timeUs)
                            break;
                        trackFrom = point.offset;
                    }
                    from = std::min(from, trackFrom);
                }
            } else {
                for (auto& point : m_index) {
                    if (point.timeUs > timeUs)
                        break;
                    from = point.offset;
                }
            }
            if (from == m_firstFragment)
                rewindFragments();
            else {
                m_scan = from;
                m_ended = false;
                for (auto& t : m_states) {
                    t.table.clear();
                    t.next = 0;
                    t.nextDts = 0;
                }
            }
            // Fragments are read until every track holds the time (without an index, from the first one): in a file
            // whose fragments each hold one track (ffmpeg's separate_moof, DASH-style packagers) the other tracks'
            // fragments for it are further on. Once one track holds it, at most 16 more: a track that ended early.
            int further = 0;
            for (;;) {
                bool any = false, all = true;
                for (auto& t : m_states) {
                    bool holds = !t.table.empty() && t.pts(t.table.back()) + toUs(t.table.back().duration, t.timescale) > timeUs;
                    any = any || holds;
                    all = all && holds;
                }
                if (all || (any && ++further > 16) || !loadNextFragment())
                    break;
            }
        }
        for (auto& t : m_states)
            position(t, timeUs);
        if (m_fragmented) {
            for (auto& t : m_states) {
                t.table.erase(t.table.begin(), t.table.begin() + t.next);
                t.next = 0;
            }
        }
    }

    // The tracks at the first fragment again, with the samples moov holds in front of it.
    void rewindFragments()
    {
        m_scan = m_firstFragment;
        m_ended = false;
        for (auto& t : m_states) {
            t.table = t.initial;
            t.next = 0;
            t.nextDts = t.initialNextDts;
        }
    }

    // For the stream parser: the source now holds one fragment (moof + mdat) from its first byte.
    void restartAtFragment()
    {
        m_scan = 0;
        m_ended = false;
        for (auto& t : m_states) {
            t.table.clear();
            t.next = 0;
        }
    }
    bool fragmented() const { return m_fragmented; }

    // The earliest presentation time of the file, from the sample tables (a fragmented file: its first fragment,
    // read now; the tracks are back at their start after). Composition offsets put the earliest picture among the
    // first few in decode order.
    long long firstPresentationUs()
    {
        bool empty = true;
        for (auto& t : m_states)
            empty = empty && t.table.empty();
        if (m_fragmented && empty) {
            // A fragment may hold one track only: up to 8, until every track has its first samples.
            for (int i = 0; i < 8 && loadNextFragment(); ++i) {
                bool all = true;
                for (auto& t : m_states)
                    all = all && !t.table.empty();
                if (all)
                    break;
            }
            if (m_failed)
                return 0;
        }
        bool any = false;
        long long first = 0;
        for (auto& t : m_states) {
            for (size_t i = 0; i < t.table.size() && i < 32; ++i) {
                long long pts = t.pts(t.table[i]);
                if (!any || pts < first)
                    first = pts;
                any = true;
            }
        }
        if (m_fragmented)
            rewindFragments(); // back to the start: read() loads the first fragment again
        return any ? first : 0;
    }

    std::vector<uint32_t> trackIds() const
    {
        std::vector<uint32_t> ids;
        for (auto& t : m_states)
            ids.push_back(t.id);
        return ids;
    }

private:
    struct IndexPoint {
        long long timeUs;
        uint64_t offset;
    };

    bool boxHeader(uint64_t offset, uint32_t& type, uint64_t& size, size_t& header)
    {
        long long total = m_source.size();
        if (total >= 0 && offset + 8 > static_cast<uint64_t>(total))
            return false;
        uint8_t h[16];
        if (!m_source.read(offset, h, 8))
            return false;
        size = be32(h);
        type = be32(h + 4);
        header = 8;
        if (size == 1) {
            if (!m_source.read(offset + 8, h + 8, 8))
                return false;
            size = be64(h + 8);
            header = 16;
        } else if (!size) {
            if (total < 0)
                return false;
            size = static_cast<uint64_t>(total) - offset;
        }
        return size >= header;
    }

    static void position(Mp4Track& t, long long timeUs)
    {
        size_t found = 0;
        if (t.video) {
            // Key frames are in presentation order among themselves: the last one not after the time.
            bool any = false;
            for (size_t i = 0; i < t.table.size(); ++i) {
                if (!t.table[i].key)
                    continue;
                if (any && t.pts(t.table[i]) > timeUs)
                    break;
                found = i;
                any = true;
            }
        } else {
            size_t lo = 0, hi = t.table.size();
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                const Entry& e = t.table[mid];
                if (t.pts(e) + toUs(e.duration, t.timescale) > timeUs)
                    hi = mid;
                else
                    lo = mid + 1;
            }
            found = lo;
        }
        t.next = found;
    }

    bool parseMoov(const std::vector<uint8_t>& moov, std::string& error)
    {
        Box root { tag("moov"), moov.data(), moov.size() };
        uint32_t movieTimescale = 0;
        Box mvhd;
        if (findBox(root, tag("mvhd"), mvhd) && mvhd.size >= 20) {
            if (mvhd.data[0] == 1 && mvhd.size >= 32) {
                movieTimescale = be32(mvhd.data + 20);
                m_durationUs = toUs(static_cast<long long>(be64(mvhd.data + 24)), movieTimescale);
            } else {
                movieTimescale = be32(mvhd.data + 12);
                uint32_t d = be32(mvhd.data + 16);
                m_durationUs = d == 0xFFFFFFFFu ? 0 : toUs(d, movieTimescale);
            }
        }
        Box mvex;
        bool hasMvex = findBox(root, tag("mvex"), mvex);

        const uint8_t* p = root.data;
        const uint8_t* end = root.data + root.size;
        Box box;
        bool anySamples = false;
        while (nextBox(p, end, box)) {
            if (box.type != tag("trak"))
                continue;
            Track track;
            Mp4Track state;
            std::string why;
            if (!parseTrak(box, movieTimescale, track, state, why)) {
                if (!why.empty())
                    error = error.empty() ? why : error + ", " + why;
                continue;
            }
            // One track of each kind: the first.
            bool duplicate = false;
            for (auto& existing : m_tracks)
                duplicate = duplicate || existing.video == track.video;
            if (duplicate)
                continue;
            if (hasMvex) {
                const uint8_t* q = mvex.data;
                const uint8_t* qEnd = mvex.data + mvex.size;
                Box trex;
                while (nextBox(q, qEnd, trex)) {
                    if (trex.type == tag("trex") && trex.size >= 24 && be32(trex.data + 4) == state.id) {
                        state.defaultDuration = be32(trex.data + 12);
                        state.defaultSize = be32(trex.data + 16);
                        state.defaultFlags = be32(trex.data + 20);
                    }
                }
            }
            anySamples = anySamples || !state.table.empty();
            m_tracks.push_back(std::move(track));
            m_states.push_back(std::move(state));
        }
        // mvex: fragments follow, also when moov holds the first samples (then its durations count only those).
        m_fragmented = hasMvex;
        if (hasMvex && anySamples) {
            for (size_t i = 0; i < m_states.size(); ++i) {
                auto& t = m_states[i];
                if (!t.table.empty()) {
                    const Entry& last = t.table.back();
                    t.initialNextDts = last.dts + last.duration;
                    m_moovSamplesEndUs = std::max(m_moovSamplesEndUs, t.pts(last) + toUs(last.duration, t.timescale));
                }
                t.initial = t.table;
                t.nextDts = t.initialNextDts;
                m_tracks[i].durationUs = 0;
            }
            m_durationUs = 0;
        }
        if (hasMvex) {
            Box mehd;
            if (findBox(mvex, tag("mehd"), mehd) && mehd.size >= 8) {
                long long d = mehd.data[0] == 1 && mehd.size >= 12 ? static_cast<long long>(be64(mehd.data + 4)) : be32(mehd.data + 4);
                if (d > 0)
                    m_durationUs = toUs(d, movieTimescale);
            }
        }
        if (!m_fragmented) {
            long long longest = 0;
            for (auto& t : m_tracks)
                longest = std::max(longest, t.durationUs);
            if (!m_durationUs)
                m_durationUs = longest;
        }
        return true;
    }

    bool parseTrak(const Box& trak, uint32_t movieTimescale, Track& track, Mp4Track& state, std::string& why)
    {
        Box tkhd, mdia, mdhd, hdlr, minf, stbl, stsd;
        if (findBox(trak, tag("tkhd"), tkhd) && tkhd.size >= 24)
            state.id = be32(tkhd.data + (tkhd.data[0] == 1 ? 20 : 12));
        if (!findBox(trak, tag("mdia"), mdia) || !findBox(mdia, tag("mdhd"), mdhd) || !findBox(mdia, tag("hdlr"), hdlr)
            || !findBox(mdia, tag("minf"), minf) || !findBox(minf, tag("stbl"), stbl) || !findBox(stbl, tag("stsd"), stsd))
            return false;
        if (hdlr.size < 12 || mdhd.size < 20)
            return false;
        uint32_t handler = be32(hdlr.data + 8);
        if (handler != tag("vide") && handler != tag("soun"))
            return false;
        track.video = state.video = handler == tag("vide");
        long long mediaDuration = 0;
        if (mdhd.data[0] == 1 && mdhd.size >= 32) {
            state.timescale = be32(mdhd.data + 20);
            mediaDuration = static_cast<long long>(be64(mdhd.data + 24));
        } else {
            state.timescale = be32(mdhd.data + 12);
            uint32_t d = be32(mdhd.data + 16);
            mediaDuration = d == 0xFFFFFFFFu ? 0 : d;
        }
        if (!state.timescale)
            return false;
        track.durationUs = toUs(mediaDuration, state.timescale);

        // The sample description: the first entry.
        if (stsd.size < 16)
            return false;
        const uint8_t* p = stsd.data + 8;
        const uint8_t* end = stsd.data + stsd.size;
        Box entry;
        if (!nextBox(p, end, entry))
            return false;
        track.name = tagName(entry.type);
        if (track.video) {
            if (entry.size < 78)
                return false;
            track.width = be16(entry.data + 24);
            track.height = be16(entry.data + 26);
            Box avcC;
            if ((entry.type == tag("avc1") || entry.type == tag("avc3")) && findBox(entry, tag("avcC"), avcC, 78) && avcC.size >= 7) {
                track.codec = Codec::H264;
                state.nalLength = (avcC.data[4] & 3) + 1;
                // SPS and PPS with start codes.
                const uint8_t* q = avcC.data + 5;
                const uint8_t* qEnd = avcC.data + avcC.size;
                for (int kind = 0; kind < 2 && q < qEnd; ++kind) {
                    int count = *q++ & (kind ? 0xFF : 0x1F);
                    for (int i = 0; i < count && qEnd - q >= 2; ++i) {
                        size_t length = be16(q);
                        q += 2;
                        if (length > static_cast<size_t>(qEnd - q))
                            break;
                        static const uint8_t start[4] = { 0, 0, 0, 1 };
                        track.config.insert(track.config.end(), start, start + 4);
                        track.config.insert(track.config.end(), q, q + length);
                        q += length;
                    }
                }
            } else {
                why = "video " + track.name + " non decodificabile dal telefono";
                return false;
            }
        } else {
            if (entry.size < 28)
                return false;
            int version = be16(entry.data + 8);
            track.channels = be16(entry.data + 16);
            track.sampleRate = static_cast<int>(be32(entry.data + 24) >> 16);
            size_t skip = version == 1 ? 28 + 16 : version == 2 ? 28 + 36 : 28;
            Box esds;
            if (entry.type == tag("mp4a") && findBox(entry, tag("esds"), esds, skip) && esds.size > 4) {
                int objectType = 0;
                parseEsds(esds.data + 4, esds.size - 4, objectType, track.config);
                if (objectType == 0x40 || objectType == 0x66 || objectType == 0x67 || objectType == 0x68) {
                    track.codec = Codec::AAC;
                    describeAudioSpecificConfig(track.config, track.sampleRate, track.channels);
                } else if (objectType == 0x6B || objectType == 0x69) {
                    track.codec = Codec::MP3;
                    track.config.clear();
                } else {
                    why = "audio mp4a tipo " + std::to_string(objectType) + " non decodificabile dal telefono";
                    return false;
                }
            } else if (entry.type == tag(".mp3"))
                track.codec = Codec::MP3;
            else {
                why = "audio " + track.name + " non decodificabile dal telefono";
                return false;
            }
        }

        // Edit list: an empty edit delays the track, the first real one says which media time starts it.
        Box edts, elst;
        if (findBox(trak, tag("edts"), edts) && findBox(edts, tag("elst"), elst) && elst.size >= 8) {
            int version = elst.data[0];
            uint32_t count = be32(elst.data + 4);
            const uint8_t* q = elst.data + 8;
            size_t each = version == 1 ? 20 : 12;
            for (uint32_t i = 0; i < count && static_cast<size_t>(elst.data + elst.size - q) >= each; ++i, q += each) {
                long long segment = version == 1 ? static_cast<long long>(be64(q)) : be32(q);
                long long mediaTime = version == 1 ? static_cast<long long>(be64(q + 8)) : static_cast<int32_t>(be32(q + 4));
                if (mediaTime < 0) {
                    state.delayUs += toUs(segment, movieTimescale);
                    continue;
                }
                state.mediaTime = mediaTime;
                break;
            }
        }
        return buildTable(stbl, state);
    }

    // MPEG-4 descriptors: ES_Descriptor > DecoderConfigDescriptor (object type) > DecoderSpecificInfo.
    static void parseEsds(const uint8_t* p, size_t size, int& objectType, std::vector<uint8_t>& specific)
    {
        const uint8_t* end = p + size;
        auto descriptor = [&](int& kind, size_t& length) {
            if (end - p < 2)
                return false;
            kind = *p++;
            length = 0;
            for (int i = 0; i < 4 && p < end; ++i) {
                uint8_t b = *p++;
                length = (length << 7) | (b & 0x7F);
                if (!(b & 0x80))
                    break;
            }
            return length <= static_cast<size_t>(end - p);
        };
        int kind = 0;
        size_t length = 0;
        if (!descriptor(kind, length) || kind != 3 || length < 3)
            return;
        int flags = p[2];
        p += 3;
        if (flags & 0x80)
            p += 2;
        if (flags & 0x40) {
            if (p < end)
                p += 1 + *p;
        }
        if (flags & 0x20)
            p += 2;
        if (p >= end || !descriptor(kind, length) || kind != 4 || length < 13)
            return;
        objectType = p[0];
        p += 13;
        if (p >= end || !descriptor(kind, length) || kind != 5)
            return;
        specific.assign(p, p + length);
    }

    static bool buildTable(const Box& stbl, Mp4Track& state)
    {
        Box stts, ctts, stss, stsc, stsz, stco;
        bool hasStts = findBox(stbl, tag("stts"), stts) && stts.size >= 8;
        bool hasCtts = findBox(stbl, tag("ctts"), ctts) && ctts.size >= 8;
        bool hasStss = findBox(stbl, tag("stss"), stss) && stss.size >= 8;
        bool hasStsc = findBox(stbl, tag("stsc"), stsc) && stsc.size >= 8;
        bool compact = false;
        bool hasStsz = findBox(stbl, tag("stsz"), stsz) && stsz.size >= 12;
        if (!hasStsz) {
            hasStsz = findBox(stbl, tag("stz2"), stsz) && stsz.size >= 12;
            compact = hasStsz;
        }
        bool wide = false;
        bool hasStco = findBox(stbl, tag("stco"), stco) && stco.size >= 8;
        if (!hasStco) {
            hasStco = findBox(stbl, tag("co64"), stco) && stco.size >= 8;
            wide = hasStco;
        }
        if (!hasStts || !hasStsc || !hasStsz || !hasStco)
            return true; // a fragmented file's empty tables are missing in some writers

        uint32_t uniform = compact ? 0 : be32(stsz.data + 4);
        uint32_t fieldBits = compact ? stsz.data[7] : 32;
        uint32_t count = be32(stsz.data + 8);
        if (count > 20 * 1000 * 1000)
            return false;
        if (!uniform) {
            uint64_t need = (static_cast<uint64_t>(count) * fieldBits + 7) / 8;
            if (need > stsz.size - 12)
                return false;
        }
        auto sizeOf = [&](uint32_t i) -> uint32_t {
            if (uniform)
                return uniform;
            const uint8_t* base = stsz.data + 12;
            switch (fieldBits) {
            case 4:
                return (base[i / 2] >> (i % 2 ? 0 : 4)) & 15;
            case 8:
                return base[i];
            case 16:
                return be16(base + 2 * i);
            default:
                return be32(base + 4 * i);
            }
        };

        uint32_t chunkCount = be32(stco.data + 4);
        if (static_cast<uint64_t>(chunkCount) * (wide ? 8 : 4) > stco.size - 8)
            return false;
        uint32_t stscCount = be32(stsc.data + 4);
        if (static_cast<uint64_t>(stscCount) * 12 > stsc.size - 8)
            return false;
        uint32_t sttsCount = be32(stts.data + 4);
        if (static_cast<uint64_t>(sttsCount) * 8 > stts.size - 8)
            return false;
        uint32_t cttsCount = hasCtts ? be32(ctts.data + 4) : 0;
        if (hasCtts && static_cast<uint64_t>(cttsCount) * 8 > ctts.size - 8)
            return false;
        uint32_t stssCount = hasStss ? be32(stss.data + 4) : 0;
        if (hasStss && static_cast<uint64_t>(stssCount) * 4 > stss.size - 8)
            return false;

        state.table.resize(count);
        // Offsets: chunks, and how many samples each holds.
        uint32_t sample = 0;
        uint32_t run = 0;
        for (uint32_t chunk = 0; chunk < chunkCount && sample < count; ++chunk) {
            while (run + 1 < stscCount && be32(stsc.data + 8 + 12 * (run + 1)) <= chunk + 1)
                ++run;
            uint32_t perChunk = stscCount ? be32(stsc.data + 8 + 12 * run + 4) : 0;
            uint64_t offset = wide ? be64(stco.data + 8 + 8 * chunk) : be32(stco.data + 8 + 4 * chunk);
            for (uint32_t i = 0; i < perChunk && sample < count; ++i, ++sample) {
                Entry& e = state.table[sample];
                e.offset = offset;
                e.size = sizeOf(sample);
                e.cts = 0;
                e.key = !hasStss;
                offset += e.size;
            }
        }
        state.table.resize(sample);
        count = sample;
        // Decode times.
        int64_t dts = 0;
        sample = 0;
        for (uint32_t i = 0; i < sttsCount && sample < count; ++i) {
            uint32_t n = be32(stts.data + 8 + 8 * i);
            uint32_t delta = be32(stts.data + 8 + 8 * i + 4);
            for (uint32_t k = 0; k < n && sample < count; ++k, ++sample) {
                state.table[sample].dts = dts;
                state.table[sample].duration = delta;
                dts += delta;
            }
        }
        for (; sample < count; ++sample) {
            state.table[sample].dts = dts;
            state.table[sample].duration = 0;
        }
        // Composition offsets.
        sample = 0;
        for (uint32_t i = 0; i < cttsCount && sample < count; ++i) {
            uint32_t n = be32(ctts.data + 8 + 8 * i);
            int32_t offset = static_cast<int32_t>(be32(ctts.data + 8 + 8 * i + 4));
            for (uint32_t k = 0; k < n && sample < count; ++k, ++sample)
                state.table[sample].cts = offset;
        }
        for (uint32_t i = 0; i < stssCount; ++i) {
            uint32_t number = be32(stss.data + 8 + 4 * i);
            if (number >= 1 && number <= count)
                state.table[number - 1].key = true;
        }
        state.nextDts = dts;
        return true;
    }

    // ---- fragments

    Mp4Track* trackById(uint32_t id)
    {
        for (auto& t : m_states) {
            if (t.id == id)
                return &t;
        }
        return nullptr;
    }

    // Reads boxes from m_scan up to and including the next moof, whose samples join the tracks' tables.
    bool loadNextFragment()
    {
        if (m_ended)
            return false;
        for (int guard = 0; guard < 4096; ++guard) {
            uint32_t type = 0;
            uint64_t size = 0;
            size_t header = 0;
            if (!boxHeader(m_scan, type, size, header))
                break;
            uint64_t at = m_scan;
            m_scan += size;
            if (type != tag("moof"))
                continue;
            if (size > 64 * 1024 * 1024)
                break;
            std::vector<uint8_t> moof(static_cast<size_t>(size - header));
            if (!m_source.read(at + header, moof.data(), moof.size())) {
                m_failed = true;
                break;
            }
            parseMoof(moof, at, nullptr);
            return true;
        }
        m_ended = true;
        return false;
    }

    // `endUs`: only the end time of the fragment is wanted, the tables are left alone.
    // `startOf`/`startUs`: only the earliest time of that track in the fragment is wanted.
    void parseMoof(const std::vector<uint8_t>& moof, uint64_t moofOffset, long long* endUs, uint32_t startOf = 0, long long* startUs = nullptr)
    {
        const uint8_t* p = moof.data();
        const uint8_t* end = p + moof.size();
        Box traf;
        uint64_t runningOffset = moofOffset;
        while (nextBox(p, end, traf)) {
            if (traf.type != tag("traf"))
                continue;
            Box tfhd;
            if (!findBox(traf, tag("tfhd"), tfhd) || tfhd.size < 8)
                continue;
            uint32_t flags = be32(tfhd.data) & 0xFFFFFF;
            Mp4Track* t = trackById(be32(tfhd.data + 4));
            const uint8_t* q = tfhd.data + 8;
            const uint8_t* qEnd = tfhd.data + tfhd.size;
            auto take32 = [&](uint32_t& out) {
                if (qEnd - q >= 4) {
                    out = be32(q);
                    q += 4;
                }
            };
            uint64_t base = (flags & 0x20000) ? moofOffset : runningOffset;
            if ((flags & 1) && qEnd - q >= 8) {
                base = be64(q);
                q += 8;
            }
            uint32_t ignored = 0;
            uint32_t defaultDuration = t ? t->defaultDuration : 0, defaultSize = t ? t->defaultSize : 0, defaultFlags = t ? t->defaultFlags : 0;
            if (flags & 2)
                take32(ignored);
            if (flags & 8)
                take32(defaultDuration);
            if (flags & 0x10)
                take32(defaultSize);
            if (flags & 0x20)
                take32(defaultFlags);
            int64_t dts = t ? t->nextDts : 0;
            Box tfdt;
            if (findBox(traf, tag("tfdt"), tfdt) && tfdt.size >= 8)
                dts = tfdt.data[0] == 1 && tfdt.size >= 12 ? static_cast<int64_t>(be64(tfdt.data + 4)) : be32(tfdt.data + 4);

            const uint8_t* r = traf.data;
            const uint8_t* rEnd = traf.data + traf.size;
            Box trun;
            uint64_t offset = base;
            while (nextBox(r, rEnd, trun)) {
                if (trun.type != tag("trun") || trun.size < 8)
                    continue;
                uint32_t f = be32(trun.data) & 0xFFFFFF;
                int version = trun.data[0];
                uint32_t count = be32(trun.data + 4);
                const uint8_t* s = trun.data + 8;
                const uint8_t* sEnd = trun.data + trun.size;
                if ((f & 1) && sEnd - s >= 4) {
                    offset = base + static_cast<int32_t>(be32(s));
                    s += 4;
                }
                uint32_t firstFlags = defaultFlags;
                bool hasFirst = false;
                if ((f & 4) && sEnd - s >= 4) {
                    firstFlags = be32(s);
                    hasFirst = true;
                    s += 4;
                }
                size_t each = ((f & 0x100) ? 4 : 0) + ((f & 0x200) ? 4 : 0) + ((f & 0x400) ? 4 : 0) + ((f & 0x800) ? 4 : 0);
                if (each && count > static_cast<size_t>(sEnd - s) / each)
                    count = static_cast<uint32_t>(static_cast<size_t>(sEnd - s) / each);
                if (!each && count > 1000000)
                    count = 0;
                for (uint32_t i = 0; i < count; ++i) {
                    Entry e;
                    e.duration = defaultDuration;
                    e.size = defaultSize;
                    uint32_t sampleFlags = (!i && hasFirst) ? firstFlags : defaultFlags;
                    e.cts = 0;
                    if (f & 0x100) {
                        e.duration = be32(s);
                        s += 4;
                    }
                    if (f & 0x200) {
                        e.size = be32(s);
                        s += 4;
                    }
                    if (f & 0x400) {
                        sampleFlags = be32(s);
                        s += 4;
                    }
                    if (f & 0x800) {
                        e.cts = static_cast<int32_t>(be32(s));
                        s += 4;
                    }
                    (void)version;
                    e.offset = offset;
                    e.dts = dts;
                    e.key = !(t && t->video) || !(sampleFlags & 0x10000);
                    offset += e.size;
                    dts += e.duration;
                    if (t && startUs) {
                        if (t->id == startOf && (*startUs < 0 || t->pts(e) < *startUs))
                            *startUs = t->pts(e);
                    } else if (t && endUs)
                        *endUs = std::max(*endUs, t->pts(e) + toUs(e.duration, t->timescale));
                    else if (t)
                        t->table.push_back(e);
                }
            }
            runningOffset = offset;
            if (t && !endUs && !startUs)
                t->nextDts = dts;
        }
    }

    void parseSidx(const std::vector<uint8_t>& sidx, uint64_t after)
    {
        if (sidx.size() < 24)
            return;
        const uint8_t* p = sidx.data();
        int version = p[0];
        uint32_t timescale = be32(p + 8);
        uint64_t time = 0, first = 0;
        size_t at = 12;
        if (version == 0) {
            time = be32(p + 12);
            first = be32(p + 16);
            at = 20;
        } else {
            if (sidx.size() < 32)
                return;
            time = be64(p + 12);
            first = be64(p + 20);
            at = 28;
        }
        uint32_t count = be16(p + at + 2);
        at += 4;
        uint64_t offset = after + first;
        std::vector<IndexPoint> points;
        for (uint32_t i = 0; i < count && at + 12 <= sidx.size(); ++i, at += 12) {
            uint32_t reference = be32(p + at);
            if (reference & 0x80000000u)
                return; // an index of indexes: not followed
            points.push_back({ toUs(static_cast<long long>(time), timescale), offset });
            offset += reference & 0x7FFFFFFF;
            time += be32(p + at + 4);
        }
        // ffmpeg's "dash" files put a one-entry sidx before every fragment: an index only when it reaches the end of
        // the file (an mfra may follow it).
        long long total = m_source.size();
        if (points.empty() || total < 0 || offset + (static_cast<uint64_t>(total) - after) / 10 < static_cast<uint64_t>(total))
            return;
        // One sidx per track (ffmpeg's global_sidx) or one for the file: the video's is the main index.
        Mp4Track* t = trackById(be32(p + 4));
        if (t) {
            auto known = std::find_if(m_trackIndex.begin(), m_trackIndex.end(), [&](auto& entry) { return entry.first == t->id; });
            if (known != m_trackIndex.end())
                known->second = points;
            else
                m_trackIndex.push_back({ t->id, points });
        }
        if (!m_index.empty() && m_indexTrack && !(t && t->video))
            return;
        m_indexTrack = t ? t->id : 0;
        m_index.swap(points);
        if (!m_durationUs)
            m_durationUs = toUs(static_cast<long long>(time), timescale);
    }

    // ffmpeg's index at the end of the file (mfra: time and offset of each moof), found through its last 16 bytes.
    void readMfra()
    {
        long long total = m_source.size();
        if (total < 32)
            return;
        uint8_t tail[16];
        if (!m_source.read(total - 16, tail, 16) || be32(tail + 4) != tag("mfro"))
            return;
        uint32_t size = be32(tail + 12);
        if (size < 32 || size > 16 * 1024 * 1024 || size > static_cast<uint64_t>(total))
            return;
        std::vector<uint8_t> mfra(size);
        if (!m_source.read(total - size, mfra.data(), size) || be32(mfra.data() + 4) != tag("mfra"))
            return;
        Box root { tag("mfra"), mfra.data() + 8, mfra.size() - 8 };
        const uint8_t* p = root.data;
        const uint8_t* end = root.data + root.size;
        Box tfra;
        // The video track's, else the first.
        std::vector<IndexPoint> best;
        while (nextBox(p, end, tfra)) {
            if (tfra.type != tag("tfra") || tfra.size < 16)
                continue;
            int version = tfra.data[0];
            Mp4Track* t = trackById(be32(tfra.data + 4));
            if (!t)
                continue;
            uint32_t lengths = be32(tfra.data + 8);
            uint32_t count = be32(tfra.data + 12);
            size_t each = (version == 1 ? 16 : 8) + ((lengths >> 4) & 3) + 1 + ((lengths >> 2) & 3) + 1 + (lengths & 3) + 1;
            const uint8_t* q = tfra.data + 16;
            std::vector<IndexPoint> points;
            for (uint32_t i = 0; i < count && static_cast<size_t>(tfra.data + tfra.size - q) >= each; ++i, q += each) {
                long long time = version == 1 ? static_cast<long long>(be64(q)) : be32(q);
                uint64_t offset = version == 1 ? be64(q + 8) : be32(q + 4);
                long long us = toUs(time - t->mediaTime, t->timescale) + t->delayUs;
                if (!points.empty() && points.back().offset == offset)
                    continue;
                points.push_back({ us, offset });
            }
            m_trackIndex.push_back({ t->id, points });
            if (best.empty() || t->video) {
                best.swap(points);
                m_indexTrack = t->id;
            }
        }
        m_index.swap(best);
    }

    // An index whose times are not those of the fragments it points to (ffmpeg's -output_ts_offset moves the mfra
    // times, not the tfdt ones): the fragments' own times win, a track's index moves by the difference in its first
    // indexed fragment.
    void calibrateIndex()
    {
        for (auto& [id, points] : m_trackIndex) {
            long long shift = indexShift(id, points);
            for (auto& point : points)
                point.timeUs += shift;
            if (id == m_indexTrack) {
                for (auto& point : m_index)
                    point.timeUs += shift;
            }
        }
    }

    long long indexShift(uint32_t trackId, const std::vector<IndexPoint>& points)
    {
        if (points.empty())
            return 0;
        uint32_t type = 0;
        uint64_t size = 0;
        size_t header = 0;
        uint64_t at = points.front().offset;
        if (!boxHeader(at, type, size, header) || type != tag("moof") || size > 64 * 1024 * 1024)
            return 0;
        std::vector<uint8_t> moof(static_cast<size_t>(size - header));
        if (!m_source.read(at + header, moof.data(), moof.size()))
            return 0;
        long long startUs = -1;
        parseMoof(moof, at, nullptr, trackId, &startUs);
        if (startUs < 0)
            return 0;
        long long shift = startUs - points.front().timeUs;
        return shift > -100000 && shift < 100000 ? 0 : shift; // composition offsets and rounding stay
    }

    long long endOfLastFragment()
    {
        uint32_t type = 0;
        uint64_t size = 0;
        size_t header = 0;
        uint64_t at = m_index.back().offset;
        if (!boxHeader(at, type, size, header) || type != tag("moof") || size > 64 * 1024 * 1024)
            return 0;
        std::vector<uint8_t> moof(static_cast<size_t>(size - header));
        if (!m_source.read(at + header, moof.data(), moof.size()))
            return 0;
        long long endUs = 0;
        parseMoof(moof, at, &endUs);
        return endUs;
    }

    ByteSource& m_source;
    std::vector<Mp4Track> m_states;
    bool m_fragmented { false };
    bool m_ended { false };
    long long m_moovSamplesEndUs { 0 };
    uint32_t m_indexTrack { 0 };
    // Each indexed track's own points (mfra: a tfra per track; a sidx per track): m_index is one of them.
    std::vector<std::pair<uint32_t, std::vector<IndexPoint>>> m_trackIndex;
    uint64_t m_firstFragment { 0 };
    uint64_t m_scan { 0 };
    std::vector<IndexPoint> m_index;
};

// ---------------------------------------------------------------------------------------------------------------
// MP3 and ADTS: a run of self-describing frames.

} // namespace

bool parseMp3Header(const uint8_t* h, FrameInfo& out)
{
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
        return false;
    int version = (h[1] >> 3) & 3;
    int layer = (h[1] >> 1) & 3;
    if (version == 1 || layer != 1) // layer III only
        return false;
    int bitrateIndex = h[2] >> 4, rateIndex = (h[2] >> 2) & 3, padding = (h[2] >> 1) & 1;
    if (!bitrateIndex || bitrateIndex == 15 || rateIndex == 3)
        return false;
    static const int v1[15] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
    static const int v2[15] = { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 };
    static const int rates[3] = { 44100, 48000, 32000 };
    out.version = version;
    out.bitrate = (version == 3 ? v1 : v2)[bitrateIndex] * 1000;
    out.rate = rates[rateIndex] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    out.samples = version == 3 ? 1152 : 576;
    out.channels = (h[3] >> 6) == 3 ? 1 : 2;
    out.size = static_cast<uint32_t>(out.samples / 8 * out.bitrate / out.rate + padding);
    out.skip = 0;
    return out.size > 4;
}

bool parseAdtsHeader(const uint8_t* h, FrameInfo& out)
{
    if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0)
        return false;
    out.profile = (h[2] >> 6) + 1;
    out.rateIndex = (h[2] >> 2) & 15;
    out.channelConfig = ((h[2] & 1) << 2) | (h[3] >> 6);
    if (out.rateIndex > 12)
        return false;
    out.size = (static_cast<uint32_t>(h[3] & 3) << 11) | (h[4] << 3) | (h[5] >> 5);
    out.skip = (h[1] & 1) ? 7 : 9;
    out.samples = 1024 * ((h[6] & 3) + 1);
    out.rate = aacRates[out.rateIndex];
    out.channels = out.channelConfig == 7 ? 8 : out.channelConfig;
    return out.size > out.skip;
}

namespace {

class FramedDemuxer final : public Demuxer {
public:
    FramedDemuxer(ByteSource& source, bool adts)
        : m_source(source)
        , m_adts(adts)
    {
    }

    const char* container() const final { return m_adts ? "aac adts" : "mp3"; }

    bool open(std::string& error)
    {
        long long total = m_source.size();
        uint64_t start = 0;
        uint8_t h[10];
        // ID3v2 in front (there can be more than one).
        while (m_source.read(start, h, 10) && !memcmp(h, "ID3", 3))
            start += 10 + ((h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F)) + ((h[5] & 0x10) ? 10 : 0);
        FrameInfo first;
        if (!sync(start, first, 128 * 1024)) {
            error = m_adts ? "aac: nessun fotogramma" : "mp3: nessun fotogramma";
            return false;
        }
        m_dataStart = m_position = start;
        m_dataEnd = total > 0 ? static_cast<uint64_t>(total) : 0;
        if (m_dataEnd >= 128) {
            uint8_t id3v1[3];
            if (m_source.read(m_dataEnd - 128, id3v1, 3) && !memcmp(id3v1, "TAG", 3))
                m_dataEnd -= 128;
        }

        Track track;
        track.sampleRate = first.rate;
        track.channels = first.channels;
        m_rate = first.rate;
        if (m_adts) {
            track.codec = Codec::AAC;
            track.name = "aac";
            unsigned config = (first.profile << 11) | (first.rateIndex << 7) | (first.channelConfig << 3);
            track.config = { static_cast<uint8_t>(config >> 8), static_cast<uint8_t>(config) };
            // No header says how long the file is: the first frames' average size, over the whole of it.
            uint64_t at = start;
            uint64_t bytes = 0;
            int frames = 0;
            FrameInfo info;
            while (frames < 64 && m_source.read(at, h, 7) && parseAdtsHeader(h, info)) {
                bytes += info.size;
                at += info.size;
                ++frames;
            }
            if (frames && m_dataEnd > m_dataStart)
                m_durationUs = toUs(static_cast<long long>((m_dataEnd - m_dataStart) * frames / bytes) * first.samples, first.rate);
        } else {
            track.codec = Codec::MP3;
            track.name = "mp3";
            // Xing/Info in the first frame: frame and byte counts, and a table of 100 seek points.
            std::vector<uint8_t> frame(first.size);
            if (m_source.read(start, frame.data(), frame.size())) {
                size_t side = first.version == 3 ? (first.channels == 1 ? 17 : 32) : (first.channels == 1 ? 9 : 17);
                size_t at = 4 + side;
                if (at + 8 <= frame.size() && (!memcmp(&frame[at], "Xing", 4) || !memcmp(&frame[at], "Info", 4))) {
                    uint32_t flags = be32(&frame[at + 4]);
                    at += 8;
                    uint32_t frames = 0;
                    if ((flags & 1) && at + 4 <= frame.size()) {
                        frames = be32(&frame[at]);
                        at += 4;
                    }
                    if ((flags & 2) && at + 4 <= frame.size()) {
                        m_xingBytes = be32(&frame[at]);
                        at += 4;
                    }
                    if ((flags & 4) && at + 100 <= frame.size()) {
                        m_toc.assign(frame.begin() + at, frame.begin() + at + 100);
                        at += 100;
                    }
                    if (frames)
                        m_durationUs = toUs(static_cast<long long>(frames) * first.samples, first.rate);
                    // The tag's frame holds no audio.
                    m_dataStart = m_position = start + first.size;
                }
            }
            if (!m_durationUs && m_dataEnd > m_dataStart && first.bitrate)
                m_durationUs = static_cast<long long>(m_dataEnd - m_dataStart) * 8000000 / first.bitrate;
        }
        track.durationUs = m_durationUs;
        m_tracks.push_back(std::move(track));
        return true;
    }

    bool read(size_t index, Sample& sample) final
    {
        if (index)
            return false;
        FrameInfo info;
        if (!sync(m_position, info, 64 * 1024))
            return false;
        if (m_dataEnd && m_position + info.size > m_dataEnd)
            return false;
        sample.data.resize(info.size - info.skip);
        if (!m_source.read(m_position + info.skip, sample.data.data(), sample.data.size())) {
            m_failed = m_dataEnd && m_position + info.size <= m_dataEnd;
            return false;
        }
        m_position += info.size;
        sample.ptsUs = sample.dtsUs = m_baseUs + toUs(m_samples, m_rate);
        sample.durationUs = toUs(info.samples, m_rate);
        sample.key = true;
        m_samples += info.samples;
        return true;
    }

    void seek(long long timeUs) final
    {
        if (timeUs <= 0 || !m_durationUs || m_dataEnd <= m_dataStart) {
            m_position = m_dataStart;
            m_baseUs = 0;
            m_samples = 0;
            return;
        }
        if (timeUs > m_durationUs)
            timeUs = m_durationUs;
        double fraction = static_cast<double>(timeUs) / static_cast<double>(m_durationUs);
        uint64_t span = m_dataEnd - m_dataStart;
        uint64_t target;
        if (m_toc.size() == 100 && m_xingBytes) {
            double slot = fraction * 100;
            int i = std::min(99, static_cast<int>(slot));
            double a = m_toc[i], b = i < 99 ? m_toc[i + 1] : 256;
            target = m_dataStart + static_cast<uint64_t>((a + (b - a) * (slot - i)) / 256 * m_xingBytes);
        } else
            target = m_dataStart + static_cast<uint64_t>(fraction * static_cast<double>(span));
        if (target >= m_dataEnd)
            target = m_dataEnd;
        FrameInfo info;
        m_position = target;
        sync(m_position, info, 64 * 1024);
        m_baseUs = timeUs;
        m_samples = 0;
    }

private:
    bool header(uint64_t at, FrameInfo& info)
    {
        uint8_t h[7];
        if (m_dataEnd && at + 7 > m_dataEnd)
            return false;
        if (!m_source.read(at, h, 7))
            return false;
        return m_adts ? parseAdtsHeader(h, info) : parseMp3Header(h, info);
    }

    // Moves `at` forward to the next frame header that is followed by another of the same stream.
    bool sync(uint64_t& at, FrameInfo& info, size_t limit)
    {
        if (header(at, info)) {
            FrameInfo following;
            uint64_t next = at + info.size;
            if ((m_dataEnd && next + 7 > m_dataEnd) || (header(next, following) && following.rate == info.rate))
                return true;
        }
        std::vector<uint8_t> window;
        uint64_t base = at;
        size_t scanned = 0;
        while (scanned < limit) {
            size_t want = 4096;
            if (m_dataEnd) {
                if (base + scanned + 8 > m_dataEnd)
                    return false;
                want = static_cast<size_t>(std::min<uint64_t>(want, m_dataEnd - (base + scanned)));
            }
            window.resize(want);
            if (!m_source.read(base + scanned, window.data(), want)) {
                // A shorter tail when the size is unknown.
                return false;
            }
            for (size_t i = 0; i + 7 <= want; ++i) {
                if (window[i] != 0xFF)
                    continue;
                FrameInfo candidate, following;
                bool ok = m_adts ? parseAdtsHeader(&window[i], candidate) : parseMp3Header(&window[i], candidate);
                if (!ok)
                    continue;
                uint64_t here = base + scanned + i;
                uint64_t next = here + candidate.size;
                if ((m_dataEnd && next + 7 > m_dataEnd && next <= m_dataEnd) || (header(next, following) && following.rate == candidate.rate)) {
                    at = here;
                    info = candidate;
                    return true;
                }
            }
            scanned += want > 6 ? want - 6 : want;
        }
        return false;
    }

    ByteSource& m_source;
    bool m_adts;
    uint64_t m_dataStart { 0 }, m_dataEnd { 0 }, m_position { 0 };
    int m_rate { 0 };
    long long m_baseUs { 0 };
    long long m_samples { 0 };
    uint32_t m_xingBytes { 0 };
    std::vector<uint8_t> m_toc;
};

// ---------------------------------------------------------------------------------------------------------------
// Media Source: boxes as they come.

class OwnedBytes final : public ByteSource {
public:
    std::vector<uint8_t> bytes;
    long long size() final { return static_cast<long long>(bytes.size()); }
    bool read(unsigned long long offset, void* out, size_t size) final
    {
        if (offset > bytes.size() || size > bytes.size() - offset)
            return false;
        memcpy(out, bytes.data() + offset, size);
        return true;
    }
};

class Mp4StreamParser final : public StreamParser {
public:
    bool append(const uint8_t* data, size_t size, Client& client, std::string& error) final
    {
        m_pending.insert(m_pending.end(), data, data + size);
        size_t at = 0;
        bool ok = true;
        while (m_pending.size() - at >= 8) {
            const uint8_t* p = m_pending.data() + at;
            uint64_t boxSize = be32(p);
            uint32_t type = be32(p + 4);
            size_t header = 8;
            if (boxSize == 1) {
                if (m_pending.size() - at < 16)
                    break;
                boxSize = be64(p + 8);
                header = 16;
            }
            bool known = false;
            for (uint32_t t : { tag("ftyp"), tag("styp"), tag("moov"), tag("moof"), tag("mdat"), tag("sidx"), tag("ssix"), tag("free"),
                     tag("skip"), tag("emsg"), tag("prft"), tag("uuid"), tag("mfra"), tag("meta"), tag("pdin") })
                known = known || t == type;
            if (boxSize < header || !known || boxSize > 512ull * 1024 * 1024) {
                error = !known ? "mp4: box '" + tagName(type) + "' fuori posto" : "mp4: box '" + tagName(type) + "' di lunghezza sbagliata";
                ok = false;
                break;
            }
            if (m_pending.size() - at < boxSize)
                break;
            if (!box(type, p, static_cast<size_t>(boxSize), client, error)) {
                ok = false;
                break;
            }
            at += static_cast<size_t>(boxSize);
        }
        if (!ok) {
            m_pending.clear();
            m_fragment.clear();
            return false;
        }
        m_pending.erase(m_pending.begin(), m_pending.begin() + static_cast<long>(at));
        return true;
    }

    void reset() final
    {
        m_pending.clear();
        m_fragment.clear();
    }

private:
    bool box(uint32_t type, const uint8_t* data, size_t size, Client& client, std::string& error)
    {
        if (type == tag("moov")) {
            m_fragment.clear();
            m_source.bytes.assign(data, data + size);
            auto demuxer = std::make_unique<Mp4Demuxer>(m_source);
            if (!demuxer->open(error))
                return false;
            if (!demuxer->fragmented()) {
                error = "mp4 non a frammenti in un SourceBuffer";
                return false;
            }
            m_demuxer = std::move(demuxer);
            client.initialization(m_demuxer->tracks(), m_demuxer->trackIds(), m_demuxer->durationUs());
            return true;
        }
        if (type == tag("moof")) {
            if (!m_demuxer) {
                error = "mp4: frammento prima del segmento di inizializzazione";
                return false;
            }
            m_fragment.assign(data, data + size);
            return true;
        }
        if (type == tag("mdat") && m_demuxer && !m_fragment.empty()) {
            m_source.bytes.swap(m_fragment);
            m_fragment.clear();
            m_source.bytes.insert(m_source.bytes.end(), data, data + size);
            m_demuxer->restartAtFragment();
            for (size_t i = 0; i < m_demuxer->tracks().size(); ++i) {
                Sample sample;
                while (m_demuxer->read(i, sample))
                    client.sample(i, std::move(sample));
            }
            m_source.bytes.clear();
        }
        return true;
    }

    std::vector<uint8_t> m_pending, m_fragment;
    OwnedBytes m_source;
    std::unique_ptr<Mp4Demuxer> m_demuxer;
};

} // namespace

std::unique_ptr<StreamParser> StreamParser::create()
{
    return std::make_unique<Mp4StreamParser>();
}

// A presentation whose first time is not 0 (the fragments of a live stream saved to a file, an empty edit in front of
// every track): its times are moved to start at 0, as Firefox does (Chrome starts the element's clock at the first
// picture instead). The element then plays at once, and its seek bar covers what is there. Transport streams and HLS
// do the same in Hls.cpp; Media Source keeps the times the page gives (timestampOffset is the page's to set).
class ShiftedDemuxer final : public Demuxer {
public:
    ShiftedDemuxer(std::unique_ptr<Demuxer> inner, long long originUs)
        : m_inner(std::move(inner))
        , m_originUs(originUs)
    {
        m_tracks = m_inner->tracks();
        for (auto& track : m_tracks) {
            if (track.durationUs > originUs)
                track.durationUs -= originUs;
        }
        // An mvhd duration counts from the presentation's 0 (the shift is inside it), a fragment index's end is a
        // time: either way the end moves with the times.
        m_durationUs = m_inner->durationUs() > originUs ? m_inner->durationUs() - originUs : m_inner->durationUs();
    }

    const char* container() const final { return m_inner->container(); }
    bool read(size_t track, Sample& sample) final
    {
        if (!m_inner->read(track, sample)) {
            m_failed = m_inner->failed();
            return false;
        }
        sample.ptsUs -= m_originUs;
        sample.dtsUs -= m_originUs;
        return true;
    }
    void seek(long long timeUs) final { m_inner->seek(timeUs + m_originUs); }
    bool live() const final { return m_inner->live(); }
    bool pending(size_t track) final { return m_inner->pending(track); }
    bool configInBand() const final { return m_inner->configInBand(); }
    long long bufferedEndUs() const final
    {
        long long end = m_inner->bufferedEndUs();
        return end < 0 ? end : end - m_originUs;
    }

private:
    std::unique_ptr<Demuxer> m_inner;
    const long long m_originUs;
};

std::unique_ptr<Demuxer> Demuxer::open(ByteSource& source, std::string& error, Fetcher* fetcher, const std::string& url)
{
    uint8_t head[12] = { };
    if (!source.read(0, head, 12)) {
        error = "risorsa vuota o illeggibile";
        return nullptr;
    }
    uint32_t type = be32(head + 4);
    if (type == tag("ftyp") || type == tag("moov") || type == tag("mdat") || type == tag("free") || type == tag("skip")
        || type == tag("styp") || type == tag("moof") || type == tag("wide")) {
        auto demuxer = std::make_unique<Mp4Demuxer>(source);
        if (!demuxer->open(error))
            return nullptr;
        long long origin = demuxer->firstPresentationUs();
        if (demuxer->failed()) {
            error = "mp4: primo frammento illeggibile";
            return nullptr;
        }
        if (origin > 0)
            return std::make_unique<ShiftedDemuxer>(std::move(demuxer), origin);
        return demuxer;
    }
    if (!memcmp(head, "OggS", 4)) {
        error = "ogg: nessun decoder sul telefono";
        return nullptr;
    }
    if (be32(head) == 0x1A45DFA3) {
        error = "webm/matroska: nessun decoder sul telefono";
        return nullptr;
    }
    if (!memcmp(head, "#EXTM3U", 7) || (head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF && !memcmp(head + 3, "#EXTM3U", 7))) {
        if (!fetcher) {
            error = "HLS: nessun modo di scaricare i segmenti";
            return nullptr;
        }
        return openHls(*fetcher, url, error);
    }
    if (head[0] == 0x47) {
        uint8_t next = 0;
        if (source.read(188, &next, 1) && next == 0x47)
            return openTransportStream(source, error);
    }
    if (!memcmp(head, "RIFF", 4) || !memcmp(head, "fLaC", 4)) {
        error = "wav/flac: non ancora";
        return nullptr;
    }
    // ADTS has layer bits 00, MP3 does not; an ID3 tag stands in front of either.
    bool adts = false;
    if (!memcmp(head, "ID3", 3)) {
        uint64_t at = 0;
        uint8_t h[10];
        while (source.read(at, h, 10) && !memcmp(h, "ID3", 3))
            at += 10 + ((h[6] & 0x7F) << 21 | (h[7] & 0x7F) << 14 | (h[8] & 0x7F) << 7 | (h[9] & 0x7F)) + ((h[5] & 0x10) ? 10 : 0);
        adts = h[0] == 0xFF && (h[1] & 0xF6) == 0xF0;
    } else if (head[0] == 0xFF && (head[1] & 0xF6) == 0xF0)
        adts = true;
    else if (!(head[0] == 0xFF && (head[1] & 0xE0) == 0xE0)) {
        // Unknown first bytes: an MP3 with junk in front is still found by the frame search.
    }
    auto demuxer = std::make_unique<FramedDemuxer>(source, adts);
    if (!demuxer->open(error))
        return nullptr;
    return demuxer;
}

} // namespace Media
} // namespace Lumia
