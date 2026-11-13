// Web Audio's output and input files (PLAN §73.2 item 6, stage M4).
//
// Output: the graph is rendered a quantum (128 frames) at a time on a thread of this file, or on the AudioWorklet's
// thread when the page has one, through WebCore's AudioDestinationResampler (the FIFO between the two), and played
// by an XAudio2 source voice, the same AudioOutput the media elements use. The voice takes the context's sample rate
// and XAudio2 converts to the device's.
//
// Files (decodeAudioData, and the HRTF impulse responses of the panner): WAV read here; MP3, AAC and MP4 through the
// media pipeline's demuxers and the phone's Media Foundation decoders.

#include "config.h"

#if ENABLE(WEB_AUDIO)

#include "AudioOutput.h"
#include "Demux.h"
#include "Engine.h"
#include "MediaDecoder.h"
#include "AudioBus.h"
#include "AudioDestinationResampler.h"
#include "AudioFileReader.h"
#include "AudioUtilities.h"
#include <wtf/FileSystem.h>
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Scope.h>
#include <wtf/Threading.h>
#include <wtf/text/MakeString.h>

#include <objbase.h>

namespace WebCore {

namespace {

// What the context runs at when the page does not ask for a rate: most sounds a page decodes are 44.1 kHz, which then
// need no conversion, and the graph costs 8% less than at the device's 48 kHz.
constexpr float defaultSampleRate = 44100;
// Frames given to the voice at a time, and how many such buffers it holds ahead: 3 x 512 at 44.1 kHz = 35 ms between
// the graph and the ear, and that much margin for a render that comes late.
constexpr size_t framesPerDeviceBuffer = 512;
constexpr unsigned buffersAhead = 3;

class AudioDestinationWP81 final : public AudioDestinationResampler, public ThreadSafeRefCounted<AudioDestinationWP81, WTF::DestructionThread::Main> {
public:
    explicit AudioDestinationWP81(const CreationOptions& options)
        : AudioDestinationResampler(options, options.sampleRate)
        , m_channels(std::max(1u, options.numberOfOutputChannels))
    {
        m_planes.grow(m_channels);
        for (auto& plane : m_planes)
            plane.grow(framesPerDeviceBuffer);
    }

    ~AudioDestinationWP81()
    {
        stopThread();
    }

    void ref() const final { ThreadSafeRefCounted::ref(); }
    void deref() const final { ThreadSafeRefCounted::deref(); }

private:
    void startRendering(CompletionHandler<void(bool)>&& completionHandler) final
    {
        ASSERT(isMainThread());
        bool success = true;
        if (!m_thread) {
            String error;
            if (!m_output.open(static_cast<int>(sampleRate()), m_channels, error)) {
                Lumia::log("audio web: %s", error.utf8().data());
                success = false;
            } else {
                m_output.start();
                m_stop = false;
                m_thread = Thread::create("Web Audio output"_s, [this, protectedThis = Ref { *this }] {
                    run();
                }, ThreadType::Audio, Thread::QOS::UserInteractive);
            }
        }
        if (success)
            setIsPlaying(true);
        callOnMainThread([completionHandler = WTF::move(completionHandler), success]() mutable {
            completionHandler(success);
        });
    }

    void stopRendering(CompletionHandler<void(bool)>&& completionHandler) final
    {
        ASSERT(isMainThread());
        stopThread();
        setIsPlaying(false);
        callOnMainThread([completionHandler = WTF::move(completionHandler)]() mutable {
            completionHandler(true);
        });
    }

    void stopThread()
    {
        if (!m_thread)
            return;
        m_stop = true;
        m_thread->waitForCompletion();
        m_thread = nullptr;
        m_output.close();
    }

    // The device's side: keeps the voice fed.
    void run()
    {
        uint64_t frames = 0;
        while (!m_stop) {
            if (m_output.queuedBuffers() >= buffersAhead) {
                // Woken when the voice finishes a buffer (12 ms of sound at 44.1 kHz), not by polling.
                m_output.waitForBufferEnd(30);
                continue;
            }
            for (unsigned channel = 0; channel < m_channels; ++channel)
                m_outputBus->setChannelMemory(channel, m_planes[channel].mutableSpan());
            // What is rendered already comes out of the FIFO (silence where it ran dry); the graph is then asked for
            // as much again, here or on the worklet's thread.
            auto framesToRender = pullRendered(framesPerDeviceBuffer);
            render(static_cast<double>(frames) / sampleRate(), MonotonicTime::now(), framesToRender);
            frames += framesPerDeviceBuffer;

            Vector<int16_t> samples(framesPerDeviceBuffer * m_channels);
            for (unsigned channel = 0; channel < m_channels; ++channel) {
                auto& plane = m_planes[channel];
                for (size_t frame = 0; frame < framesPerDeviceBuffer; ++frame) {
                    float value = plane[frame] * 32767.0f;
                    samples[frame * m_channels + channel] = static_cast<int16_t>(std::clamp(value, -32768.0f, 32767.0f));
                }
            }
            m_output.submit(WTF::move(samples));
        }
    }

    const unsigned m_channels;
    Lumia::AudioOutput m_output;
    Vector<Vector<float>> m_planes;
    RefPtr<Thread> m_thread;
    std::atomic<bool> m_stop { false };
};

// ---------------------------------------------------------------------------------------------------------------
// Files.

unsigned readLE(std::span<const uint8_t> data, size_t offset, unsigned bytes)
{
    unsigned value = 0;
    for (unsigned i = 0; i < bytes; ++i)
        value |= static_cast<unsigned>(data[offset + i]) << (8 * i);
    return value;
}

// RIFF/WAVE with PCM (8, 16, 24, 32 bit) or float (32, 64 bit) samples, plain or "extensible".
RefPtr<AudioBus> readWAV(std::span<const uint8_t> data)
{
    if (data.size() < 12 || memcmp(data.data(), "RIFF", 4) || memcmp(data.data() + 8, "WAVE", 4))
        return nullptr;
    unsigned format = 0, channels = 0, rate = 0, blockAlign = 0, bits = 0;
    std::span<const uint8_t> samples;
    for (size_t offset = 12; offset + 8 <= data.size();) {
        size_t size = readLE(data, offset + 4, 4);
        size_t body = offset + 8;
        size_t available = std::min(size, data.size() - body);
        if (!memcmp(data.data() + offset, "fmt ", 4) && available >= 16) {
            format = readLE(data, body, 2);
            channels = readLE(data, body + 2, 2);
            rate = readLE(data, body + 4, 4);
            blockAlign = readLE(data, body + 12, 2);
            bits = readLE(data, body + 14, 2);
            // WAVE_FORMAT_EXTENSIBLE: the real format is the first two bytes of the sub-format GUID.
            if (format == 0xFFFE && available >= 26)
                format = readLE(data, body + 24, 2);
        } else if (!memcmp(data.data() + offset, "data", 4)) {
            samples = data.subspan(body, available);
            break;
        }
        offset = body + size + (size & 1);
    }
    bool isFloat = format == 3;
    if ((format != 1 && !isFloat) || !channels || channels > 32 || !rate || !bits || bits % 8 || samples.empty())
        return nullptr;
    unsigned bytes = bits / 8;
    if ((isFloat && bytes != 4 && bytes != 8) || (!isFloat && bytes > 4))
        return nullptr;
    if (blockAlign < bytes * channels)
        blockAlign = bytes * channels;
    size_t frames = samples.size() / blockAlign;
    if (!frames)
        return nullptr;

    Ref bus = AudioBus::create(channels, frames);
    bus->setSampleRate(rate);
    for (unsigned channel = 0; channel < channels; ++channel) {
        auto out = bus->channel(channel)->mutableSpan();
        for (size_t frame = 0; frame < frames; ++frame) {
            auto* p = samples.data() + frame * blockAlign + channel * bytes;
            float value;
            if (isFloat) {
                if (bytes == 4) {
                    float f;
                    memcpy(&f, p, 4);
                    value = f;
                } else {
                    double d;
                    memcpy(&d, p, 8);
                    value = static_cast<float>(d);
                }
            } else if (bytes == 1)
                value = (static_cast<int>(p[0]) - 128) / 128.0f; // 8 bit is unsigned
            else {
                int32_t sample = 0;
                for (unsigned i = 0; i < bytes; ++i)
                    sample |= static_cast<int32_t>(p[i]) << (8 * i);
                // Sign of the top byte.
                unsigned shift = 32 - bits;
                sample = static_cast<int32_t>(static_cast<uint32_t>(sample) << shift) >> shift;
                value = sample / static_cast<float>(1u << (bits - 1));
            }
            out[frame] = value;
        }
    }
    return bus;
}

class MemorySource final : public Lumia::Media::ByteSource {
public:
    explicit MemorySource(std::span<const uint8_t> data)
        : m_data(data)
    {
    }

private:
    long long size() final { return static_cast<long long>(m_data.size()); }
    bool read(unsigned long long offset, void* out, size_t size) final
    {
        if (offset > m_data.size() || size > m_data.size() - offset)
            return false;
        memcpy(out, m_data.data() + offset, size);
        return true;
    }

    std::span<const uint8_t> m_data;
};

// The first audio track of an MP3, ADTS or MP4 file, decoded whole.
RefPtr<AudioBus> decodeCompressed(std::span<const uint8_t> data)
{
    // The decoders are COM objects; this runs on WebCore's audio decoding thread (or the HRTF loader's).
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto uninitialize = makeScopeExit([com] {
        if (SUCCEEDED(com))
            CoUninitialize();
    });

    MemorySource source(data);
    std::string error;
    auto demuxer = Lumia::Media::Demuxer::open(source, error);
    if (!demuxer) {
        Lumia::log("audio web: file non letto: %s", error.c_str());
        return nullptr;
    }
    size_t trackIndex = notFound;
    for (size_t i = 0; i < demuxer->tracks().size(); ++i) {
        if (!demuxer->tracks()[i].video) {
            trackIndex = i;
            break;
        }
    }
    if (trackIndex == notFound)
        return nullptr;

    String decoderError;
    auto decoder = Lumia::MediaDecoder::create(demuxer->tracks()[trackIndex], decoderError);
    if (!decoder) {
        Lumia::log("audio web: nessun decoder: %s", decoderError.utf8().data());
        return nullptr;
    }

    int rate = 0, channels = 0;
    Vector<int16_t> pcm;
    auto take = [&] {
        for (auto& chunk : decoder->audio()) {
            if (!rate) {
                rate = chunk.rate;
                channels = chunk.channels;
            }
            // A stream that changes format on the way (rare): what follows the change is left out.
            if (chunk.rate == rate && chunk.channels == channels)
                pcm.appendVector(chunk.samples);
        }
        decoder->audio().clear();
    };
    Lumia::Media::Sample sample;
    while (demuxer->read(trackIndex, sample)) {
        if (!decoder->decode(std::span<const uint8_t> { sample.data.data(), sample.data.size() }, sample.ptsUs, sample.durationUs))
            break;
        take();
    }
    decoder->drain();
    take();
    if (rate <= 0 || channels <= 0 || pcm.isEmpty())
        return nullptr;

    size_t frames = pcm.size() / channels;
    Ref bus = AudioBus::create(channels, frames);
    bus->setSampleRate(rate);
    for (int channel = 0; channel < channels; ++channel) {
        auto out = bus->channel(channel)->mutableSpan();
        for (size_t frame = 0; frame < frames; ++frame)
            out[frame] = pcm[frame * channels + channel] / 32768.0f;
    }
    return bus;
}

} // namespace

Ref<AudioDestination> AudioDestination::create(const CreationOptions& options)
{
    return adoptRef(*new AudioDestinationWP81(options));
}

float AudioDestination::hardwareSampleRate()
{
    return defaultSampleRate;
}

// 0: the destination is stereo and cannot be asked for more.
unsigned long AudioDestination::maxChannelCount()
{
    return 0;
}

RefPtr<AudioBus> createBusFromInMemoryAudioFile(std::span<const uint8_t> data, bool mixToMono, float sampleRate)
{
    RefPtr bus = readWAV(data);
    if (!bus)
        bus = decodeCompressed(data);
    if (!bus)
        return nullptr;
    // 0: the file's own rate.
    if (sampleRate && bus->sampleRate() != sampleRate)
        return AudioBus::createBySampleRateConverting(*bus, mixToMono, sampleRate);
    if (mixToMono && bus->numberOfChannels() > 1)
        return AudioBus::createByMixingToMono(*bus);
    return bus;
}

// The panner's impulse responses: <package>\audio\<name>.wav (tools/browser copies WebCore's
// platform/audio/resources/Composite.wav there). Without the file the HRTF panner is silent, as on any port that
// ships none; "equalpower", the default model, needs nothing.
RefPtr<AudioBus> AudioBus::loadPlatformResource(const char* name, float sampleRate)
{
    auto* resourceDirectory = Lumia::config().resource_directory;
    if (!resourceDirectory)
        return nullptr;
    auto path = FileSystem::pathByAppendingComponent(FileSystem::pathByAppendingComponent(String(resourceDirectory), "audio"_s), makeString(String::fromUTF8(name), ".wav"_s));
    auto data = FileSystem::readEntireFile(path);
    if (!data) {
        Lumia::log("audio web: manca %s", path.utf8().data());
        return nullptr;
    }
    return createBusFromInMemoryAudioFile(data->span(), false, sampleRate);
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
