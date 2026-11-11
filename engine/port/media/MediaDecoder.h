// The phone's own decoders as Media Foundation transforms (PLAN §75): AAC (MSAUDDECMFT), MP3 (MP3DMOD), H.264
// (the system's decoder MFT, on the CPU: its pictures are copied out as I420 for the compositor's textures).
// One object per track, used on the pipeline's thread only.

#pragma once

#include "Demux.h"
#include <span>
#include <wtf/Ref.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

struct IMFMediaType;
struct IMFSample;
struct IMFTransform;

namespace Lumia {

struct DecodedAudio {
    long long ptsUs { 0 };
    int rate { 0 };
    int channels { 0 };
    Vector<int16_t> samples; // interleaved
};

// One decoded picture: three tight planes (Y, then U and V at half size, rounded up).
class VideoPicture final : public ThreadSafeRefCounted<VideoPicture> {
public:
    static Ref<VideoPicture> create() { return adoptRef(*new VideoPicture); }

    int width { 0 }, height { 0 };
    long long ptsUs { 0 }, durationUs { 0 };
    bool bt709 { false };
    Vector<uint8_t> bytes;

    int chromaWidth() const { return (width + 1) / 2; }
    int chromaHeight() const { return (height + 1) / 2; }
    const uint8_t* y() const { return bytes.span().data(); }
    const uint8_t* u() const { return y() + static_cast<size_t>(width) * height; }
    const uint8_t* v() const { return u() + static_cast<size_t>(chromaWidth()) * chromaHeight(); }

private:
    VideoPicture() = default;
};

class MediaDecoder {
    WTF_MAKE_NONCOPYABLE(MediaDecoder);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(MediaDecoder);
public:
    // Null with `error` when the phone has no decoder for the track or it refuses the format.
    static std::unique_ptr<MediaDecoder> create(const Media::Track&, String& error);
    ~MediaDecoder();

    // One sample in; what the decoder gives back joins audio() or pictures(). False when it refused the input.
    bool decode(std::span<const uint8_t>, long long ptsUs, long long durationUs);
    // End of the stream: what the decoder still holds comes out.
    void drain();
    // A seek: everything inside is dropped.
    void flush();

    Vector<DecodedAudio>& audio() { return m_audio; }
    Vector<Ref<VideoPicture>>& pictures() { return m_pictures; }
    const String& description() const { return m_description; }

private:
    MediaDecoder() = default;
    bool setUp(const Media::Track&, String& error);
    bool selectOutputType();
    void pull();
    void takeAudio(IMFSample*);
    void takePicture(IMFSample*);

    IMFTransform* m_transform { nullptr };
    bool m_video { false };
    bool m_outputSet { false };
    bool m_started { false };
    String m_description;
    Vector<DecodedAudio> m_audio;
    Vector<Ref<VideoPicture>> m_pictures;
    // Audio output type.
    int m_rate { 0 }, m_channels { 0 };
    long long m_nextAudioUs { 0 };
    // Video output type: the coded frame, its row length, the part to show.
    unsigned m_layout { 0 }; // fourcc
    int m_frameWidth { 0 }, m_frameHeight { 0 }, m_stride { 0 };
    int m_cropX { 0 }, m_cropY { 0 }, m_width { 0 }, m_height { 0 };
    int m_declaredWidth { 0 }, m_declaredHeight { 0 };
    unsigned m_failures { 0 };
};

} // namespace Lumia
