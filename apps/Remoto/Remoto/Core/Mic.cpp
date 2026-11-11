#include "pch.h"
#include "Mic.h"
#include "opus.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/implements.h>

using namespace Microsoft::WRL;

namespace
{
    const int Rate = 48000, Frame = 960;   // 20 ms mono, what the Pixel's injected microphone takes
    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT without ksmedia.h
    const GUID FloatSubtype = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

    std::atomic<int> g_generation(0);   // Stop/Start: a capture whose generation is old ends
    std::atomic<bool> g_running(false);

    std::string Hr(const char* what, HRESULT hr)
    {
        char b[96];
        sprintf_s(b, "%s 0x%08X", what, static_cast<unsigned>(hr));
        return b;
    }

    void Run(ComPtr<IAudioClient> client, int generation, Mic::PacketFn packet, Mic::LogFn log)
    {
        WAVEFORMATEX* fmt = nullptr;
        HRESULT hr = client->GetMixFormat(&fmt);
        if (FAILED(hr))
        {
            log("microfono: " + Hr("GetMixFormat", hr));
            g_running = false;
            return;
        }
        bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                       (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                        IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat, FloatSubtype));
        int channels = fmt->nChannels, rate = fmt->nSamplesPerSec, bits = fmt->wBitsPerSample;
        HANDLE ready = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        // 20 ms buffer: the voice leaves the phone as soon as it is captured
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, fmt, nullptr);
        if (SUCCEEDED(hr))
            hr = client->SetEventHandle(ready);
        ComPtr<IAudioCaptureClient> capture;
        if (SUCCEEDED(hr))
            hr = client->GetService(IID_PPV_ARGS(&capture));
        int err = 0;
        OpusEncoder* encoder = SUCCEEDED(hr) ? opus_encoder_create(Rate, 1, OPUS_APPLICATION_VOIP, &err) : nullptr;
        if (SUCCEEDED(hr) && !encoder)
            hr = E_FAIL;
        if (SUCCEEDED(hr))
        {
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
            opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));   // the Lumia's CPU also decodes video meanwhile
            opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
            hr = client->Start();
        }
        if (FAILED(hr))
        {
            log("microfono: " + Hr("IAudioClient", hr) + " opus " + std::to_string(err));
            if (encoder)
                opus_encoder_destroy(encoder);
            CoTaskMemFree(fmt);
            CloseHandle(ready);
            g_running = false;
            return;
        }
        char line[96];
        sprintf_s(line, "microfono acceso: %d Hz, %d canali, %d bit%s", rate, channels, bits, isFloat ? " float" : "");
        log(line);

        std::vector<short> pcm;           // mono 48 kHz waiting for a whole 20 ms frame
        double phase = 0, step = static_cast<double>(rate) / Rate;
        double last = 0;                  // previous input sample (linear interpolation across packets)
        unsigned char out[1500];
        int packets = 0;
        while (g_generation == generation)
        {
            WaitForSingleObjectEx(ready, 200, FALSE);
            UINT32 size = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&size)) && size > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr)))
                    break;
                for (UINT32 i = 0; i < frames; ++i)
                {
                    double v = 0;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    {
                        for (int c = 0; c < channels; ++c)
                            v += isFloat && bits == 32 ? reinterpret_cast<const float*>(data)[i * channels + c]
                                                       : reinterpret_cast<const short*>(data)[i * channels + c] / 32768.0;
                        v /= channels;
                    }
                    // output samples between the previous input sample and this one (rate -> 48 kHz)
                    while (phase <= 1.0)
                    {
                        double s = (last + (v - last) * phase) * 32767.0;
                        pcm.push_back(static_cast<short>(s > 32767 ? 32767 : s < -32768 ? -32768 : s));
                        phase += step;
                    }
                    phase -= 1.0;
                    last = v;
                }
                capture->ReleaseBuffer(frames);
            }
            size_t at = 0;
            for (; at + Frame <= pcm.size(); at += Frame)
            {
                int n = opus_encode(encoder, &pcm[at], Frame, out, sizeof out);
                if (n > 0)
                {
                    packet(out, static_cast<size_t>(n));
                    ++packets;
                }
            }
            pcm.erase(pcm.begin(), pcm.begin() + at);
        }
        client->Stop();
        opus_encoder_destroy(encoder);
        CoTaskMemFree(fmt);
        CloseHandle(ready);
        log("microfono spento dopo " + std::to_string(packets) + " pacchetti");
        if (g_generation == generation)
            g_running = false;
    }

    class Activation : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler>
    {
    public:
        STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op)
        {
            HRESULT act = E_FAIL;
            ComPtr<IUnknown> unk;
            HRESULT hr = op->GetActivateResult(&act, &unk);
            if (SUCCEEDED(hr))
                hr = act;
            ComPtr<IAudioClient> client;
            if (SUCCEEDED(hr))
                hr = unk.As(&client);
            if (FAILED(hr))
            {
                log("microfono: " + Hr("attivazione", hr));
                g_running = false;
                return S_OK;
            }
            int gen = generation;
            Mic::PacketFn p = packet;
            Mic::LogFn l = log;
            Windows::System::Threading::ThreadPool::RunAsync(
                ref new Windows::System::Threading::WorkItemHandler([client, gen, p, l](Windows::Foundation::IAsyncAction^)
            {
                Run(client, gen, p, l);
            }), Windows::System::Threading::WorkItemPriority::High, Windows::System::Threading::WorkItemOptions::TimeSliced);
            return S_OK;
        }
        int generation;
        Mic::PacketFn packet;
        Mic::LogFn log;
    };
}

void Mic::Start(const std::wstring& deviceId, PacketFn packet, LogFn log)
{
    if (g_running.exchange(true))
        return;
    int gen = ++g_generation;
    std::wstring id = deviceId;
    if (id.empty())
        id = Windows::Media::Devices::MediaDevice::GetDefaultAudioCaptureId(Windows::Media::Devices::AudioDeviceRole::Default)->Data();
    ComPtr<Activation> handler = Make<Activation>();
    handler->generation = gen;
    handler->packet = packet;
    handler->log = log;
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(id.c_str(), __uuidof(IAudioClient), nullptr, handler.Get(), &op);
    if (FAILED(hr))
    {
        log("microfono: " + Hr("ActivateAudioInterfaceAsync", hr));
        g_running = false;
    }
}

void Mic::Stop()
{
    ++g_generation;
    g_running = false;
}

bool Mic::Running()
{
    return g_running;
}
