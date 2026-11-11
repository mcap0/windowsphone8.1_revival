#ifndef PERLECAST_TSMUXER_H
#define PERLECAST_TSMUXER_H

#include "TsWriter.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace PerleCast {

class TsMuxer {
public:
    typedef TsWriter::PacketCallback PacketCallback;

    explicit TsMuxer(const PacketCallback& callback, int64_t seekOffset90k = 0);
    void PushVideo(const uint8_t* data, size_t length, int64_t pts90k, int64_t dts90k, bool keyframe);
    void PushAudio(const uint8_t* adts, size_t length, int64_t pts90k);
    void Finish();
    // Drops SEI NAL units that come after the picture data of an access unit (not allowed by H.264)
    static std::vector<uint8_t> DropTrailingSei(const uint8_t* annexB, size_t length);
    static std::vector<uint8_t> AddParameterSets(const uint8_t* annexB, size_t length,
                                                  const std::vector<std::vector<uint8_t> >& sps,
                                                  const std::vector<std::vector<uint8_t> >& pps,
                                                  bool keyframe);

private:
    struct Sample {
        std::vector<uint8_t> bytes;
        int64_t pts;
        int64_t dts;
        bool keyframe;
    };
    TsWriter writer_;
    std::deque<Sample> video_;
    std::deque<Sample> audio_;
    bool tablesWritten_;
    int64_t lastTables_;   // dts of the last PAT/PMT: repeated every 100 ms
    int64_t offset_;
    void EmitReady(bool drain);
    void RepeatTables(int64_t dts);
    void EmitVideo(const Sample& sample);
    void EmitAudio(const Sample& sample);
};

} // namespace PerleCast

#endif
