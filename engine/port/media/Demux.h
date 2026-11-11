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
    static std::unique_ptr<Demuxer> open(ByteSource&, std::string& error);
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

protected:
    std::vector<Track> m_tracks;
    long long m_durationUs { 0 };
    bool m_failed { false };
};

} // namespace Media
} // namespace Lumia
