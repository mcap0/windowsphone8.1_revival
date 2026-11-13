// Demuxers of the Lumia port's media pipeline (PLAN §73.2 item 6, §75): MP4 (progressive and fragmented), MP3 and
// ADTS AAC. Plain C++ over a blocking byte source, no WebCore: the same code is tested on the PC
// (engine/port/tests/demuxtest.cpp against ffprobe's packet lists).
//
// Samples come out in the form the phone's Media Foundation decoders take: H.264 as Annex-B (start codes; the SPS and
// PPS are in Track::config, to be sent before a key frame), AAC as raw frames (Track::config = AudioSpecificConfig),
// MP3 as whole frames.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Lumia {
namespace Media {

class ByteSource {
public:
    virtual ~ByteSource() { }
    // Size of the whole resource in bytes, -1 while unknown.
    virtual long long size() = 0;
    // Copies `size` bytes at `offset`, waiting for the network if it has to. False: stopped, failed, or past the end.
    virtual bool read(unsigned long long offset, void* out, size_t size) = 0;
};

// Whole resources by address, for a stream made of many (HLS: play lists, segments, keys). Blocking, like ByteSource.
class Fetcher {
public:
    virtual ~Fetcher() { }
    // `reference` is resolved against `base` (the address of the document that names it). `length` 0 = the whole
    // resource, else that byte range. `resolved` gets the address fetched, after redirects. False: stopped or failed.
    virtual bool fetch(const std::string& base, const std::string& reference, uint64_t offset, uint64_t length,
        std::vector<uint8_t>& out, std::string& resolved) = 0;
    // A hint: this will be asked for next (the engine starts the download; fetch() then finds it).
    virtual void prefetch(const std::string& /* base */, const std::string& /* reference */, uint64_t /* offset */, uint64_t /* length */) { }
    // Sleeps (a live play list with nothing new). False: stopped.
    virtual bool wait(int milliseconds) = 0;
};

enum class Codec : uint8_t { Unknown, H264, AAC, MP3 };

struct Track {
    bool video { false };
    Codec codec { Codec::Unknown };
    std::string name;            // the container's name of the codec ("avc1", "mp4a", "mp3", ...), for the log
    int width { 0 }, height { 0 };
    int sampleRate { 0 }, channels { 0 }; // as declared; the decoder's output type has the last word (HE-AAC)
    std::vector<uint8_t> config;
    long long durationUs { 0 };
};

struct Sample {
    long long ptsUs { 0 }, dtsUs { 0 }, durationUs { 0 };
    bool key { false };
    std::vector<uint8_t> data;
};

class Demuxer {
public:
    // Reads what the container keeps up front (moov, the first frames). Null with `error` set when the bytes are not
    // a format we play, or no track has a codec the phone decodes.
    // `fetcher` and `url` (the resource's own address): needed by HLS only.
    static std::unique_ptr<Demuxer> open(ByteSource&, std::string& error, Fetcher* = nullptr, const std::string& url = std::string());
    virtual ~Demuxer() { }

    virtual const char* container() const = 0;
    const std::vector<Track>& tracks() const { return m_tracks; }
    // 0 = unknown (a stream without a length).
    long long durationUs() const { return m_durationUs; }

    // Next sample of the track in decode order. False at the end of the track, or when the source failed
    // (failed() tells them apart).
    virtual bool read(size_t track, Sample&) = 0;
    bool failed() const { return m_failed; }

    // Moves every track: video to the last key frame at or before the time, audio to the frame that contains it.
    virtual void seek(long long timeUs) = 0;

    // A stream with no end (a live HLS play list): no duration, no seeking.
    virtual bool live() const { return false; }
    // Nothing to read from the track right now, and it is not its end (samples pushed by the page: Media Source).
    virtual bool pending(size_t /* track */) { return false; }
    // Key frames already carry their parameter sets (they may change on the way).
    virtual bool configInBand() const { return false; }
    // How far the stream is held in memory ahead of what was read, when the bytes do not say (HLS). -1: unknown.
    virtual long long bufferedEndUs() const { return -1; }

protected:
    std::vector<Track> m_tracks;
    long long m_durationUs { 0 };
    bool m_failed { false };
};

// Media Source Extensions: the bytes of a SourceBuffer as the page appends them, in pieces of any size. Fragmented
// MP4 only (an initialization segment, ftyp + moov, then media segments, moof + mdat).
class StreamParser {
public:
    static std::unique_ptr<StreamParser> create();
    virtual ~StreamParser() { }

    struct Client {
        virtual ~Client() { }
        // An initialization segment: the tracks (all of them, playable or not: Track::codec Unknown) and the
        // duration it declares (0 = none).
        virtual void initialization(const std::vector<Track>&, const std::vector<uint32_t>& ids, long long durationUs) = 0;
        // One sample of a media segment, `track` an index into the last initialization's list.
        virtual void sample(size_t track, Sample&&) = 0;
    };

    // False with `error`: the bytes are not what the format allows (the SourceBuffer's append error).
    virtual bool append(const uint8_t*, size_t, Client&, std::string& error) = 0;
    // abort(): the unfinished box is dropped; the next bytes start a new segment.
    virtual void reset() = 0;
};

// Hls.cpp
std::unique_ptr<Demuxer> openHls(Fetcher&, const std::string& url, std::string& error);
std::unique_ptr<Demuxer> openTransportStream(ByteSource&, std::string& error);

// Shared by the demuxers (Demux.cpp).
struct FrameInfo {
    uint32_t size { 0 };     // the whole frame
    uint32_t skip { 0 };     // header bytes the decoder does not want
    int samples { 0 };
    int rate { 0 };
    int channels { 0 };
    int bitrate { 0 };       // MP3, bits per second
    int version { 0 };       // MP3: 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5
    int profile { 0 };       // ADTS: audio object type
    int rateIndex { 0 };
    int channelConfig { 0 };
};
bool parseMp3Header(const uint8_t* fourBytes, FrameInfo&);
bool parseAdtsHeader(const uint8_t* sevenBytes, FrameInfo&);
// Width and height an H.264 sequence parameter set declares (the NAL unit, header byte first). False: not parsed.
bool parseH264Size(const uint8_t* sps, size_t size, int& width, int& height);

} // namespace Media
} // namespace Lumia
