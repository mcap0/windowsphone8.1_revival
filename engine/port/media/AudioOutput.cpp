#include "config.h"
#include "AudioOutput.h"

#include "Engine.h"
#include <windows.h>
#include <mmreg.h>
// The SDK's header creates the engine by loading xaudio2_9.dll by name, which the phone has not: its XAudio2Create is
// set aside and the phone's own export (XAUDIO2_8.DLL, sysroot/phone/xaudio2_8.lib) is declared in its place. The
// interfaces are the same in both versions.
#define XAudio2Create XAudio2CreateOfTheSDK
#include <xaudio2.h>
#undef XAudio2Create
#include <wtf/HexNumber.h>
#include <wtf/Seconds.h>
#include <wtf/text/MakeString.h>

extern "C" __declspec(dllimport) HRESULT __stdcall XAudio2Create(IXAudio2**, UINT32 flags, UINT32 processor);

namespace Lumia {

namespace {

Lock engineLock;
IXAudio2* engine WTF_GUARDED_BY_LOCK(engineLock);
IXAudio2MasteringVoice* masteringVoice WTF_GUARDED_BY_LOCK(engineLock);

// The process's engine and its output, made at the first sound and kept.
IXAudio2* ensureEngine(String& error)
{
    Locker locker { engineLock };
    if (engine && masteringVoice)
        return engine;
    HRESULT hr = S_OK;
    if (!engine)
        hr = XAudio2Create(&engine, 0, 1 /* XAUDIO2_DEFAULT_PROCESSOR of 2.8 */);
    if (SUCCEEDED(hr)) {
        // AudioCategory_ForegroundOnlyMedia (1): a media app in front; the phone's own music pauses for it.
        hr = engine->CreateMasteringVoice(&masteringVoice, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr, static_cast<AUDIO_STREAM_CATEGORY>(1));
        if (FAILED(hr))
            hr = engine->CreateMasteringVoice(&masteringVoice, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr, static_cast<AUDIO_STREAM_CATEGORY>(0));
    }
    if (FAILED(hr)) {
        error = makeString("XAudio2 0x"_s, hex(static_cast<unsigned>(hr), 8));
        return nullptr;
    }
    return engine;
}

} // namespace

// Buffers belong to the voice from submit to the end of their playing (or their flush). The end of each one sets the
// event a feeder waits on (waitForBufferEnd).
struct AudioOutput::Callback final : IXAudio2VoiceCallback {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(Callback);
public:
    Callback()
        : bufferEnded(CreateEventExW(nullptr, nullptr, 0, EVENT_ALL_ACCESS))
    {
    }
    ~Callback()
    {
        if (bufferEnded)
            CloseHandle(bufferEnded);
    }
    void __stdcall OnVoiceProcessingPassStart(UINT32) final { }
    void __stdcall OnVoiceProcessingPassEnd() final { }
    void __stdcall OnStreamEnd() final { }
    void __stdcall OnBufferStart(void*) final { }
    void __stdcall OnBufferEnd(void* context) final
    {
        delete static_cast<Vector<int16_t>*>(context);
        if (bufferEnded)
            SetEvent(bufferEnded);
    }
    void __stdcall OnLoopEnd(void*) final { }
    void __stdcall OnVoiceError(void*, HRESULT) final { }

    HANDLE bufferEnded;
};

AudioOutput::~AudioOutput()
{
    // The voice goes first: DestroyVoice waits for the callbacks that use m_callback.
    close();
    delete m_callback;
}

void AudioOutput::waitForBufferEnd(unsigned milliseconds)
{
    if (!m_callback || !m_callback->bufferEnded) {
        WTF::sleep(Seconds::fromMilliseconds(std::min(milliseconds, 2u)));
        return;
    }
    WaitForSingleObjectEx(m_callback->bufferEnded, milliseconds, FALSE);
}

bool AudioOutput::open(int rate, int channels, String& error)
{
    close();
    IXAudio2* xaudio = ensureEngine(error);
    if (!xaudio)
        return false;
    WAVEFORMATEX format = { };
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = static_cast<WORD>(channels);
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = static_cast<WORD>(channels * 2);
    format.nAvgBytesPerSec = rate * format.nBlockAlign;
    if (!m_callback)
        m_callback = new Callback;
    IXAudio2SourceVoice* voice = nullptr;
    HRESULT hr = xaudio->CreateSourceVoice(&voice, &format, 0, 4.0f, m_callback, nullptr, nullptr);
    if (FAILED(hr)) {
        error = makeString("voce XAudio2 "_s, rate, " Hz "_s, channels, " canali: 0x"_s, hex(static_cast<unsigned>(hr), 8));
        return false;
    }
    Locker locker { m_lock };
    m_voice = voice;
    m_rate = rate;
    m_channels = channels;
    m_submitted = 0;
    m_voice->SetVolume(m_volume);
    m_voice->SetFrequencyRatio(static_cast<float>(m_playbackRate));
    if (m_running)
        m_voice->Start(0);
    return true;
}

void AudioOutput::close()
{
    IXAudio2SourceVoice* voice = nullptr;
    {
        Locker locker { m_lock };
        voice = std::exchange(m_voice, nullptr);
        m_submitted = 0;
    }
    if (!voice)
        return;
    voice->Stop(0);
    voice->FlushSourceBuffers();
    // Waits for the callbacks of the buffers still queued.
    voice->DestroyVoice();
}

bool AudioOutput::isOpen() const
{
    Locker locker { m_lock };
    return m_voice;
}

void AudioOutput::submit(Vector<int16_t>&& samples)
{
    if (samples.isEmpty())
        return;
    Locker locker { m_lock };
    if (!m_voice)
        return;
    auto* owned = new Vector<int16_t>(WTF::move(samples));
    XAUDIO2_BUFFER buffer = { };
    buffer.AudioBytes = static_cast<UINT32>(owned->size() * sizeof(int16_t));
    buffer.pAudioData = reinterpret_cast<const BYTE*>(owned->span().data());
    buffer.pContext = owned;
    if (FAILED(m_voice->SubmitSourceBuffer(&buffer))) {
        delete owned;
        return;
    }
    m_submitted += owned->size() / m_channels;
}

void AudioOutput::start()
{
    Locker locker { m_lock };
    m_running = true;
    if (m_voice)
        m_voice->Start(0);
}

void AudioOutput::stop()
{
    Locker locker { m_lock };
    m_running = false;
    if (m_voice)
        m_voice->Stop(0);
}

void AudioOutput::setVolume(float volume)
{
    Locker locker { m_lock };
    m_volume = volume;
    if (m_voice)
        m_voice->SetVolume(volume);
}

void AudioOutput::setRate(double rate)
{
    Locker locker { m_lock };
    m_playbackRate = std::clamp(rate, 0.25, 4.0);
    if (m_voice)
        m_voice->SetFrequencyRatio(static_cast<float>(m_playbackRate));
}

uint64_t AudioOutput::submittedFrames() const
{
    Locker locker { m_lock };
    return m_submitted;
}

uint64_t AudioOutput::playedFrames() const
{
    Locker locker { m_lock };
    if (!m_voice)
        return 0;
    XAUDIO2_VOICE_STATE state = { };
    m_voice->GetState(&state, 0);
    return state.SamplesPlayed;
}

unsigned AudioOutput::queuedBuffers() const
{
    Locker locker { m_lock };
    if (!m_voice)
        return 0;
    XAUDIO2_VOICE_STATE state = { };
    m_voice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    return state.BuffersQueued;
}

} // namespace Lumia
