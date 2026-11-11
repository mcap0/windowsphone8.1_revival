// SPDX-License-Identifier: GPL-3.0-or-later
// MPEG-TS demuxer for the HLS segments of YouTube (pure C++11, no WinRT): H.264 video (Annex B access units)
// and AAC audio (ADTS frames split into raw AAC frames). Used by the MediaStreamSource of Perle 8.1.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace perle {

// One H.264 access unit (one PES packet). Data is Annex B (start codes), with SPS and PPS in front of key frames.
struct VideoAccessUnit {
    int64_t pts90k;   // presentation time, 90 kHz, relative to the first PES of the segment
    int64_t dts90k;   // decoding time, same origin
    int64_t dur90k;   // until the next access unit (the last one repeats the previous duration)
    bool key;         // contains an IDR slice
    std::vector<uint8_t> annexB;
};

// One raw AAC frame (ADTS header removed), presentation time in 90 kHz.
struct AudioFrame {
    int64_t pts90k;
    std::vector<uint8_t> aac;
};

struct TsSegmentData {
    bool hasVideo = false;
    bool hasAudio = false;
    std::vector<VideoAccessUnit> video;
    std::vector<AudioFrame> audio;
    std::vector<uint8_t> spsAnnexB;   // SPS and PPS with start codes: the sequence header of the stream
    std::vector<uint8_t> ppsAnnexB;
    std::vector<uint8_t> audioSpecificConfig; // 2 bytes, AudioSpecificConfig of the AAC stream
    int audioSampleRate = 0;
    int audioChannels = 0;
    // All times above are relative to baseRaw: the 33-bit 90 kHz value (DTS of the first video access unit in a
    // TS segment, or the ID3 transport timestamp of an AAC segment). Same clock for audio and video.
    int64_t baseRaw = 0;
    std::string error;                // empty when the segment parsed
};

// Parses one complete TS segment. Returns false (and sets error) when no PAT/PMT or no PES was found.
bool ParseTsSegment(const uint8_t* data, size_t size, TsSegmentData& out);

// Parses one AAC segment of YouTube's HLS audio renditions: ADTS frames after an ID3 tag whose PRIV frame
// "com.apple.streaming.transportStreamTimestamp" gives the base time.
bool ParseAacSegment(const uint8_t* data, size_t size, TsSegmentData& out);

// Finds the H.264 SPS width and height (cropped), 0 when the SPS is not usable.
bool ParseSpsSize(const std::vector<uint8_t>& spsAnnexB, int& width, int& height);

}  // namespace perle
