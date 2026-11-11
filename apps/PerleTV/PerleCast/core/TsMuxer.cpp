#include "TsMuxer.h"

#include <algorithm>

namespace PerleCast {

TsMuxer::TsMuxer(const PacketCallback& callback, int64_t seekOffset90k)
    : writer_(callback), tablesWritten_(false), lastTables_(0), offset_(seekOffset90k)
{
}

void TsMuxer::PushVideo(const uint8_t* data, size_t length, int64_t pts90k, int64_t dts90k, bool keyframe)
{
    Sample sample;
    sample.bytes = DropTrailingSei(data, length);
    sample.pts = std::max<int64_t>(0, pts90k - offset_) + 126000;
    sample.dts = std::max<int64_t>(0, dts90k - offset_) + 126000;
    sample.keyframe = keyframe;
    video_.push_back(sample);
    EmitReady(false);
}

void TsMuxer::PushAudio(const uint8_t* data, size_t length, int64_t pts90k)
{
    Sample sample;
    sample.bytes.assign(data, data + length);
    sample.pts = std::max<int64_t>(0, pts90k - offset_) + 126000;
    sample.dts = sample.pts;
    sample.keyframe = false;
    audio_.push_back(sample);
    EmitReady(false);
}

void TsMuxer::EmitReady(bool drain)
{
    if (!tablesWritten_ && (!video_.empty() || !audio_.empty())) {
        writer_.WriteSdt();
        writer_.WriteTables();
        tablesWritten_ = true;
    }
    while (!video_.empty() && !audio_.empty()) {
        if (video_.front().dts <= audio_.front().pts) {
            EmitVideo(video_.front());
            video_.pop_front();
        } else {
            EmitAudio(audio_.front());
            audio_.pop_front();
        }
    }
    if (drain) {
        while (!video_.empty()) { EmitVideo(video_.front()); video_.pop_front(); }
        while (!audio_.empty()) { EmitAudio(audio_.front()); audio_.pop_front(); }
    }
}

// MPEG-TS wants a PCR at least every 100 ms and the tables repeated: with a PCR only on keyframes (every ~6 s)
// and PAT/PMT once, the Samsung stayed in "Transition not available" at some seek points, depending on the
// content (tools/tvlab: same segments remuxed by ffmpeg played). PCR on every video frame, tables every 100 ms.
void TsMuxer::RepeatTables(int64_t dts)
{
    if (dts - lastTables_ < 9000) return;
    writer_.WriteTables();
    lastTables_ = dts;
}

void TsMuxer::EmitVideo(const Sample& sample)
{
    RepeatTables(sample.dts);
    writer_.WriteVideo(&sample.bytes[0], sample.bytes.size(), sample.pts, sample.dts, sample.keyframe, true);
}

void TsMuxer::EmitAudio(const Sample& sample)
{
    RepeatTables(sample.pts);
    writer_.WriteAudio(&sample.bytes[0], sample.bytes.size(), sample.pts);
}

void TsMuxer::Finish()
{
    EmitReady(true);
}

// YouTube's HLS puts an SEI after each segment's first keyframe, in its own PES without a timestamp; the demuxer
// joins it to the keyframe, so the access unit ended with SEI after the slices. The Samsung then stayed in
// "Transition not available" at some seek points (depending on that SEI; tools/tvlab: ffmpeg, which moves it,
// played the same frames). The SEI is optional metadata: dropped.
std::vector<uint8_t> TsMuxer::DropTrailingSei(const uint8_t* annexB, size_t length)
{
    std::vector<uint8_t> result;
    result.reserve(length);
    bool vcl = false;
    size_t i = 0;
    while (i < length) {
        // the next start code (3 or 4 bytes) after this NAL's start code
        size_t start = i;
        size_t header = i;
        while (header + 2 < length && !(annexB[header] == 0 && annexB[header + 1] == 0 && annexB[header + 2] == 1)) ++header;
        if (header + 3 >= length) { result.insert(result.end(), annexB + start, annexB + length); break; }
        size_t nal = header + 3;
        size_t next = nal + 1;
        while (next + 2 < length && !(annexB[next] == 0 && annexB[next + 1] == 0 && (annexB[next + 2] == 1 ||
               (annexB[next + 2] == 0 && next + 3 < length && annexB[next + 3] == 1)))) ++next;
        if (next + 2 >= length) next = length;
        int type = annexB[nal] & 0x1F;
        if (type >= 1 && type <= 5) vcl = true;
        if (!(type == 6 && vcl)) result.insert(result.end(), annexB + start, annexB + next);
        i = next;
    }
    return result;
}

std::vector<uint8_t> TsMuxer::AddParameterSets(const uint8_t* annexB, size_t length,
    const std::vector<std::vector<uint8_t> >& sps, const std::vector<std::vector<uint8_t> >& pps, bool keyframe)
{
    std::vector<uint8_t> result;
    if (keyframe) {
        bool hasSps = false, hasPps = false;
        for (size_t i = 0; i + 4 < length; ++i) {
            if (annexB[i] == 0 && annexB[i + 1] == 0 && annexB[i + 2] == 0 && annexB[i + 3] == 1) {
                int type = annexB[i + 4] & 0x1F;
                hasSps = hasSps || type == 7;
                hasPps = hasPps || type == 8;
            }
        }
        const uint8_t start[] = {0,0,0,1};
        if (!hasSps) for (size_t i = 0; i < sps.size(); ++i) {
            result.insert(result.end(), start, start + 4);
            result.insert(result.end(), sps[i].begin(), sps[i].end());
        }
        if (!hasPps) for (size_t i = 0; i < pps.size(); ++i) {
            result.insert(result.end(), start, start + 4);
            result.insert(result.end(), pps[i].begin(), pps[i].end());
        }
    }
    result.insert(result.end(), annexB, annexB + length);
    return result;
}

} // namespace PerleCast
