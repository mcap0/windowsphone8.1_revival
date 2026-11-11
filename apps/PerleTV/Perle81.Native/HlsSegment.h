// SPDX-License-Identifier: GPL-3.0-or-later
// One demuxed HLS segment of YouTube as ready-made MediaStreamSource samples: H.264 access units (Annex B) from a
// TS video segment, or ADTS AAC frames from a packed-audio segment. Times are the stream clock (90 kHz raw in TS,
// starting at 0 for the first segment) converted to 100 ns units.
#pragma once
#include <collection.h>

namespace Perle81 {
namespace Native {

public ref class HlsSegment sealed
{
public:
    static HlsSegment^ ParseVideo(const Platform::Array<unsigned char>^ data);
    static HlsSegment^ ParseAudio(const Platform::Array<unsigned char>^ data);

    property bool Ok { bool get() { return error_->IsEmpty(); } }
    property Platform::String^ Error { Platform::String^ get() { return error_; } }
    property int Width { int get() { return width_; } }
    property int Height { int get() { return height_; } }
    property int SampleRate { int get() { return sampleRate_; } }
    property int Channels { int get() { return channels_; } }
    // Start of the first sample and end of the last one, 100 ns units.
    property int64 StartTicks { int64 get() { return start_; } }
    property int64 EndTicks { int64 get() { return end_; } }
    property Windows::Foundation::Collections::IVector<Windows::Media::Core::MediaStreamSample^>^ Samples {
        Windows::Foundation::Collections::IVector<Windows::Media::Core::MediaStreamSample^>^ get() { return samples_; }
    }

private:
    HlsSegment();
    Platform::String^ error_;
    int width_, height_, sampleRate_, channels_;
    int64 start_, end_;
    Platform::Collections::Vector<Windows::Media::Core::MediaStreamSample^>^ samples_;
};

}  // namespace Native
}  // namespace Perle81
