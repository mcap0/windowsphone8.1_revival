// SPDX-License-Identifier: GPL-3.0-or-later
#include "HlsSegment.h"
#include "Media/TsDemux.h"
#include <string>

using namespace Perle81::Native;
using namespace Platform;
using namespace Windows::Media::Core;
using namespace Windows::Storage::Streams;

namespace {

int64 Ticks(int64_t raw90k) { return raw90k * 1000 / 9; }

IBuffer^ ToBuffer(const uint8_t* p, size_t n)
{
    DataWriter^ w = ref new DataWriter();
    w->WriteBytes(ArrayReference<unsigned char>(const_cast<uint8_t*>(p), static_cast<unsigned int>(n)));
    return w->DetachBuffer();
}

String^ Wide(const std::string& s) { return ref new String(std::wstring(s.begin(), s.end()).c_str()); }

int SampleRateIndex(int rate)
{
    static const int rates[] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
    for (int i = 0; i < 13; i++) if (rates[i] == rate) return i;
    return 4;
}

}  // namespace

HlsSegment::HlsSegment()
    : error_(""), width_(0), height_(0), sampleRate_(0), channels_(0), start_(0), end_(0),
      samples_(ref new Collections::Vector<MediaStreamSample^>())
{
}

HlsSegment^ HlsSegment::ParseVideo(const Array<unsigned char>^ data)
{
    HlsSegment^ seg = ref new HlsSegment();
    perle::TsSegmentData d;
    if (!perle::ParseTsSegment(data->Data, data->Length, d) || d.video.empty()) {
        seg->error_ = Wide(d.error.empty() ? std::string("nessun video nel segmento") : d.error);
        return seg;
    }
    perle::ParseSpsSize(d.spsAnnexB, seg->width_, seg->height_);
    bool first = true;
    for (const perle::VideoAccessUnit& au : d.video) {
        std::vector<uint8_t> bytes;
        // the decoder needs SPS and PPS before every key frame (they may be missing after a seek)
        if (au.key) {
            bytes.insert(bytes.end(), d.spsAnnexB.begin(), d.spsAnnexB.end());
            bytes.insert(bytes.end(), d.ppsAnnexB.begin(), d.ppsAnnexB.end());
        }
        bytes.insert(bytes.end(), au.annexB.begin(), au.annexB.end());
        Windows::Foundation::TimeSpan pts, dts, dur;
        pts.Duration = Ticks(d.baseRaw + au.pts90k);
        dts.Duration = Ticks(d.baseRaw + au.dts90k);
        dur.Duration = Ticks(au.dur90k);
        MediaStreamSample^ s = MediaStreamSample::CreateFromBuffer(ToBuffer(bytes.data(), bytes.size()), pts);
        s->DecodeTimestamp = dts;
        s->Duration = dur;
        s->KeyFrame = au.key;
        seg->samples_->Append(s);
        if (first || pts.Duration < seg->start_) seg->start_ = pts.Duration;
        if (pts.Duration + dur.Duration > seg->end_) seg->end_ = pts.Duration + dur.Duration;
        first = false;
    }
    return seg;
}

HlsSegment^ HlsSegment::ParseAudio(const Array<unsigned char>^ data)
{
    HlsSegment^ seg = ref new HlsSegment();
    perle::TsSegmentData d;
    bool ok = perle::ParseAacSegment(data->Data, data->Length, d);
    if (!ok || d.audio.empty()) {
        // some videos carry audio in TS segments too
        d = perle::TsSegmentData();
        ok = perle::ParseTsSegment(data->Data, data->Length, d);
    }
    if (!ok || d.audio.empty() || d.audioSampleRate <= 0) {
        seg->error_ = Wide(d.error.empty() ? std::string("nessun audio nel segmento") : d.error);
        return seg;
    }
    seg->sampleRate_ = d.audioSampleRate;
    seg->channels_ = d.audioChannels;
    int objectType = d.audioSpecificConfig.size() >= 1 ? (d.audioSpecificConfig[0] >> 3) : 2;
    if (objectType < 1 || objectType > 4) objectType = 2;  // ADTS can only say Main/LC/SSR/LTP: HE-AAC goes as LC
    int freq = SampleRateIndex(d.audioSampleRate);
    int64 frameTicks = 1024LL * 10000000LL / d.audioSampleRate;
    for (size_t i = 0; i < d.audio.size(); i++) {
        const perle::AudioFrame& f = d.audio[i];
        size_t len = f.aac.size() + 7;
        uint8_t h[7];
        h[0] = 0xFF;
        h[1] = 0xF1;  // MPEG-4, layer 0, no CRC
        h[2] = static_cast<uint8_t>(((objectType - 1) << 6) | (freq << 2) | ((d.audioChannels >> 2) & 1));
        h[3] = static_cast<uint8_t>(((d.audioChannels & 3) << 6) | ((len >> 11) & 3));
        h[4] = static_cast<uint8_t>((len >> 3) & 0xFF);
        h[5] = static_cast<uint8_t>(((len & 7) << 5) | 0x1F);
        h[6] = 0xFC;
        std::vector<uint8_t> bytes(h, h + 7);
        bytes.insert(bytes.end(), f.aac.begin(), f.aac.end());
        Windows::Foundation::TimeSpan pts, dur;
        pts.Duration = Ticks(d.baseRaw + f.pts90k);
        dur.Duration = frameTicks;
        MediaStreamSample^ s = MediaStreamSample::CreateFromBuffer(ToBuffer(bytes.data(), bytes.size()), pts);
        s->Duration = dur;
        s->KeyFrame = true;
        seg->samples_->Append(s);
        if (i == 0) seg->start_ = pts.Duration;
        seg->end_ = pts.Duration + dur.Duration;
    }
    return seg;
}
