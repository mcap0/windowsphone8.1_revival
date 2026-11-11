#include "pch.h"
#include "Audio.h"
#include "opus.h"
#include <algorithm>
#include <climits>

namespace
{
    const int Rate = 48000, Channels = 2, Frame = 960;   // 20 ms
    const int MinTarget = 2, MaxTarget = 15;              // 40-300 ms
    const int Ring = 24;                                  // > MaxTarget + 3 (silence has its own buffer)
}

Audio::Audio() : m_master(nullptr), m_voice(nullptr), m_decoder(nullptr), m_next(0), m_dropped(0), m_target(MinTarget)
{
}

Audio::~Audio()
{
    Stop();
}

bool Audio::Start(std::string& err)
{
    int e = 0;
    m_decoder = opus_decoder_create(Rate, Channels, &e);
    if (!m_decoder)
    {
        err = "opus_decoder_create " + std::to_string(e);
        return false;
    }
    HRESULT hr = XAudio2Create(&m_xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr))
        hr = m_xaudio->CreateMasteringVoice(&m_master);
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = Channels;
    format.nSamplesPerSec = Rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = Channels * 2;
    format.nAvgBytesPerSec = Rate * format.nBlockAlign;
    if (SUCCEEDED(hr))
        hr = m_xaudio->CreateSourceVoice(&m_voice, &format);
    if (SUCCEEDED(hr))
        hr = m_voice->Start();
    if (FAILED(hr))
    {
        char text[48];
        sprintf_s(text, "XAudio2 0x%08X", static_cast<unsigned>(hr));
        err = text;
        Stop();
        return false;
    }
    m_buffers.assign(Ring, std::vector<short>(Frame * 6 * Channels)); // up to 120 ms per packet
    m_silence.assign(Frame * Channels, 0);
    return true;
}

void Audio::Submit(const short* pcm, int samples)
{
    auto& buffer = m_buffers[m_next];
    m_next = (m_next + 1) % m_buffers.size();
    memcpy(buffer.data(), pcm, samples * Channels * sizeof(short));
    XAUDIO2_BUFFER b = {};
    b.AudioBytes = samples * Channels * sizeof(short);
    b.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());
    m_voice->SubmitSourceBuffer(&b);
}

void Audio::Packet(const unsigned char* data, size_t size, long long arrivalUs, long long serverUs)
{
    if (!m_voice || !m_decoder)
        return;
    // jitter: spread of (arrival - server time) over the last 2 s; the clocks' offset cancels out
    m_delays.push_back(std::make_pair(arrivalUs, arrivalUs - serverUs));
    while (!m_delays.empty() && arrivalUs - m_delays.front().first > 2000000)
        m_delays.pop_front();
    long long lo = LLONG_MAX, hi = LLONG_MIN;
    for (auto& d : m_delays)
    {
        lo = std::min(lo, d.second);
        hi = std::max(hi, d.second);
    }
    int target = static_cast<int>((hi - lo + 10000) / 20000) + 1;
    m_target = std::max(MinTarget, std::min(MaxTarget, target));
    XAUDIO2_VOICE_STATE state;
    m_voice->GetState(&state);
    if (static_cast<int>(state.BuffersQueued) > m_target + 3)
    {
        ++m_dropped; // too far behind: this packet would only add delay
        return;
    }
    static std::vector<short> pcm(Frame * 6 * Channels);
    int samples = opus_decode(m_decoder, data, static_cast<opus_int32>(size), pcm.data(), Frame * 6, 0);
    if (samples <= 0)
        return;
    if (state.BuffersQueued == 0)
    {
        // after a gap: a cushion as deep as the jitter seen, so the next late packet does not leave a hole again
        for (int i = 1; i < m_target; ++i)
        {
            XAUDIO2_BUFFER b = {};
            b.AudioBytes = Frame * Channels * sizeof(short);
            b.pAudioData = reinterpret_cast<const BYTE*>(m_silence.data());
            m_voice->SubmitSourceBuffer(&b);
        }
    }
    Submit(pcm.data(), samples);
}

void Audio::Stop()
{
    if (m_voice)
    {
        m_voice->DestroyVoice();
        m_voice = nullptr;
    }
    if (m_master)
    {
        m_master->DestroyVoice();
        m_master = nullptr;
    }
    m_xaudio = nullptr;
    if (m_decoder)
    {
        opus_decoder_destroy(m_decoder);
        m_decoder = nullptr;
    }
}
