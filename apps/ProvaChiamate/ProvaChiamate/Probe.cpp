#include "pch.h"
#include "Probe.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/implements.h>

using namespace Microsoft::WRL;
using namespace Windows::Media::Devices;
using namespace Windows::Phone::Media::Devices;

namespace
{
    const GUID FloatSubtype = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

    std::wstring Hr(const wchar_t* what, HRESULT hr)
    {
        wchar_t b[96];
        swprintf_s(b, L"%s 0x%08X", what, static_cast<unsigned>(hr));
        return b;
    }

    class Done : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler>
    {
    public:
        HANDLE event = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
        STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation*) { SetEvent(event); return S_OK; }
    };

    // Activates an IAudioClient2 on a device and waits for it (worker thread only).
    HRESULT Activate(Platform::String^ id, ComPtr<IAudioClient2>& out)
    {
        ComPtr<Done> done = Make<Done>();
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        HRESULT hr = ActivateAudioInterfaceAsync(id->Data(), __uuidof(IAudioClient2), nullptr, done.Get(), &op);
        if (FAILED(hr)) return hr;
        WaitForSingleObjectEx(done->event, 5000, FALSE);
        CloseHandle(done->event);
        HRESULT act = E_FAIL;
        ComPtr<IUnknown> unk;
        hr = op->GetActivateResult(&act, &unk);
        if (SUCCEEDED(hr)) hr = act;
        if (SUCCEEDED(hr)) hr = unk.As(&out);
        return hr;
    }

    bool IsFloat(const WAVEFORMATEX* f)
    {
        return f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat, FloatSubtype));
    }

    const wchar_t* EndpointName(int e)
    {
        switch (e)
        {
        case 0: return L"default";
        case 1: return L"earpiece";
        case 2: return L"speakerphone";
        case 3: return L"bluetooth";
        default: return L"?";
        }
    }
}

void Probe::Duplex(int category, int endpoint, int seconds, LogFn log)
{
    wchar_t line[256];
    Platform::String^ renderId = MediaDevice::GetDefaultAudioRenderId(AudioDeviceRole::Communications);
    Platform::String^ captureId = MediaDevice::GetDefaultAudioCaptureId(AudioDeviceRole::Communications);
    log(L"render " + std::wstring(renderId->Data()));
    log(L"capture " + std::wstring(captureId->Data()));
    ComPtr<IAudioClient2> render, capture;
    HRESULT hr = Activate(renderId, render);
    if (FAILED(hr)) { log(Hr(L"render activate", hr)); return; }
    hr = Activate(captureId, capture);
    if (FAILED(hr)) { log(Hr(L"capture activate", hr)); return; }

    AudioClientProperties props = {};
    props.cbSize = sizeof props;
    props.bIsOffload = FALSE;
    props.eCategory = static_cast<AUDIO_STREAM_CATEGORY>(category);
    hr = render->SetClientProperties(&props);
    log(Hr(L"render SetClientProperties", hr));
    hr = capture->SetClientProperties(&props);
    log(Hr(L"capture SetClientProperties", hr));

    WAVEFORMATEX *rf = nullptr, *cf = nullptr;
    render->GetMixFormat(&rf);
    capture->GetMixFormat(&cf);
    swprintf_s(line, L"render %d Hz %d ch %d bit%s; capture %d Hz %d ch %d bit%s", rf->nSamplesPerSec, rf->nChannels,
               rf->wBitsPerSample, IsFloat(rf) ? L" float" : L"", cf->nSamplesPerSec, cf->nChannels, cf->wBitsPerSample,
               IsFloat(cf) ? L" float" : L"");
    log(line);

    HANDLE rEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    HANDLE cEvent = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    hr = render->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, rf, nullptr);
    if (SUCCEEDED(hr)) hr = render->SetEventHandle(rEvent);
    if (FAILED(hr)) { log(Hr(L"render init", hr)); return; }
    hr = capture->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, cf, nullptr);
    if (SUCCEEDED(hr)) hr = capture->SetEventHandle(cEvent);
    if (FAILED(hr)) { log(Hr(L"capture init", hr)); return; }
    ComPtr<IAudioRenderClient> rc;
    ComPtr<IAudioCaptureClient> cc;
    render->GetService(IID_PPV_ARGS(&rc));
    capture->GetService(IID_PPV_ARGS(&cc));
    UINT32 bufFrames = 0;
    render->GetBufferSize(&bufFrames);

    hr = render->Start();
    log(Hr(L"render start", hr));
    hr = capture->Start();
    log(Hr(L"capture start", hr));

    // the stream has to run before the endpoint changes (MS blog "audio routing in WP8.1")
    try
    {
        AudioRoutingManager^ arm = AudioRoutingManager::GetDefault();
        auto avail = arm->AvailableAudioEndpoints;
        swprintf_s(line, L"routing: available 0x%X, now %s", static_cast<unsigned>(avail),
                   EndpointName(static_cast<int>(arm->GetAudioEndpoint())));
        log(line);
        if (endpoint >= 0)
        {
            arm->SetAudioEndpoint(static_cast<AudioRoutingEndpoint>(endpoint));
            swprintf_s(line, L"routing: set %s, now %s", EndpointName(endpoint),
                       EndpointName(static_cast<int>(arm->GetAudioEndpoint())));
            log(line);
        }
    }
    catch (Platform::Exception^ e)
    {
        log(Hr(L"AudioRoutingManager", e->HResult));
    }

    // Tone: 1 kHz, 0.4 amplitude, 500 ms on / 500 ms off. Capture rms by 500 ms window; the first 100 ms of each
    // window are skipped (output-to-input latency).
    const int rRate = rf->nSamplesPerSec, rCh = rf->nChannels, cRate = cf->nSamplesPerSec, cCh = cf->nChannels;
    const bool rFloat = IsFloat(rf), cFloat = IsFloat(cf);
    long long rPos = 0, cPos = 0;
    double sumOn = 0, sumOff = 0;
    long long nOn = 0, nOff = 0;
    ULONGLONG end = GetTickCount64() + seconds * 1000ULL;
    HANDLE events[2] = { rEvent, cEvent };
    while (GetTickCount64() < end)
    {
        WaitForMultipleObjectsEx(2, events, FALSE, 100, FALSE);
        UINT32 padding = 0;
        render->GetCurrentPadding(&padding);
        UINT32 n = bufFrames - padding;
        BYTE* d = nullptr;
        if (n > 0 && SUCCEEDED(rc->GetBuffer(n, &d)))
        {
            for (UINT32 i = 0; i < n; ++i, ++rPos)
            {
                bool on = (rPos * 1000 / rRate) % 1000 < 500;
                double v = on ? 0.4 * sin(2 * 3.14159265358979 * 1000.0 * rPos / rRate) : 0.0;
                for (int c = 0; c < rCh; ++c)
                {
                    if (rFloat) reinterpret_cast<float*>(d)[i * rCh + c] = static_cast<float>(v);
                    else reinterpret_cast<short*>(d)[i * rCh + c] = static_cast<short>(v * 32767);
                }
            }
            rc->ReleaseBuffer(n, 0);
        }
        UINT32 size = 0;
        while (SUCCEEDED(cc->GetNextPacketSize(&size)) && size > 0)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            for (UINT32 i = 0; i < frames; ++i, ++cPos)
            {
                double v = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    v = cFloat ? reinterpret_cast<const float*>(data)[i * cCh] : reinterpret_cast<const short*>(data)[i * cCh] / 32768.0;
                long long ms = cPos * 1000 / cRate;
                if (ms < 1000) continue;   // the first second settles (AEC adapts)
                long long inWin = ms % 500;
                if (inWin < 100) continue;
                bool on = (ms % 1000) < 500;
                if (on) { sumOn += v * v; ++nOn; } else { sumOff += v * v; ++nOff; }
            }
            cc->ReleaseBuffer(frames);
        }
    }
    render->Stop();
    capture->Stop();
    double rmsOn = nOn ? sqrt(sumOn / nOn) : 0, rmsOff = nOff ? sqrt(sumOff / nOff) : 0;
    double db = rmsOff > 0 && rmsOn > 0 ? 20 * log10(rmsOn / rmsOff) : 0;
    swprintf_s(line, L"RESULT category %d endpoint %s: mic rms tone %.5f, silence %.5f, tone/silence %+.1f dB",
               category, endpoint >= 0 ? EndpointName(endpoint) : L"(unchanged)", rmsOn, rmsOff, db);
    log(line);
    CoTaskMemFree(rf);
    CoTaskMemFree(cf);
    CloseHandle(rEvent);
    CloseHandle(cEvent);
}
