#include "pch.h"
#include "Capture.h"
#include "Core/Audio.h"
#include "Logger.h"

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/implements.h>

using namespace Microsoft::WRL;

namespace {

volatile LONG g_stop;
Capture::ChunkFn g_stream;  // streaming listener (Vosk), set by SetStream
bool g_streamEnds;          // the listener ends the utterance (StopNow)
volatile LONG g_muteMs;     // MuteStart, taken by the next Run
// long recording: set by StartLong before activation, read by Run
Capture::ChunkFn g_chunk;
unsigned g_chunkSamples;
// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT without ksmedia.h
const GUID kFloatSubtype = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
std::wstring g_format;

class Activation : public RuntimeClass<RuntimeClassFlags<ClassicCom>, FtmBase, IActivateAudioInterfaceCompletionHandler> {
public:
    STDMETHOD(ActivateCompleted)(IActivateAudioInterfaceAsyncOperation* op);
    Capture::LevelFn level;
    Capture::DoneFn done;
};

std::wstring Hr(const wchar_t* what, HRESULT hr) {
    wchar_t b[96];
    swprintf_s(b, L"%s 0x%08X", what, static_cast<unsigned>(hr));
    return b;
}

void Run(ComPtr<IAudioClient> client, Capture::LevelFn level, Capture::DoneFn done) {
    std::vector<int16_t> pcm;
    std::wstring error;
    bool heard = false;
    HRESULT hr;

    WAVEFORMATEX* fmt = nullptr;
    hr = client->GetMixFormat(&fmt);
    if (FAILED(hr)) {
        done(pcm, false, Hr(L"GetMixFormat", hr));
        return;
    }
    bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                   (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                    IsEqualGUID(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat, kFloatSubtype));
    int channels = fmt->nChannels, rate = fmt->nSamplesPerSec, bits = fmt->wBitsPerSample;
    wchar_t f[96];
    swprintf_s(f, L"%d Hz, %d canali, %d bit%s", rate, channels, bits, isFloat ? L" float" : L"");
    g_format = f;
    Logger::Write(L"cattura: " + g_format);

    HANDLE ready = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, fmt, nullptr);
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(ready);
    ComPtr<IAudioCaptureClient> cap;
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        CoTaskMemFree(fmt);
        CloseHandle(ready);
        done(pcm, false, Hr(L"IAudioClient", hr));
        return;
    }

    assist::Vad vad;
    // the Lumia's microphone gives a quiet voice 150-300 rms against a room up to ~270: energy can neither find
    // the end nor tell "nothing said" (it ended a spoken sentence at 8 s as silence), so it does not end at all
    if (g_stream && g_streamEnds) vad.EndOnlyAtLimit();
    Capture::ChunkFn chunk = g_chunk;
    Capture::ChunkFn stream = g_stream;
    bool longRec = static_cast<bool>(chunk) && g_chunkSamples > 0;
    double phase = 0;
    size_t fed = 0;
    const size_t muteSamples = static_cast<size_t>(InterlockedExchange(&g_muteMs, 0)) * 16;
    size_t muted = 0;
    unsigned long long started = GetTickCount64();
    bool finished = false;
    while (!finished) {
        WaitForSingleObjectEx(ready, 200, FALSE);
        UINT32 packet = 0;
        while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                std::vector<int16_t> zeros(frames * channels, 0);
                assist::AppendMono16k(&zeros[0], frames, channels, rate, pcm, phase);
            } else if (isFloat && bits == 32) {
                assist::AppendMono16k(reinterpret_cast<const float*>(data), frames, channels, rate, pcm, phase);
            } else if (bits == 16) {
                assist::AppendMono16k(reinterpret_cast<const int16_t*>(data), frames, channels, rate, pcm, phase);
            }
            cap->ReleaseBuffer(frames);
        }
        while (muted < muteSamples && muted < pcm.size()) pcm[muted++] = 0;
        if (pcm.size() > fed) {
            if (stream) stream(std::vector<int16_t>(pcm.begin() + fed, pcm.end()));
            bool end = vad.Feed(&pcm[fed], pcm.size() - fed);
            fed = pcm.size();
            level(vad.Level());
            if (!longRec) finished = end;
        }
        if (longRec && pcm.size() >= g_chunkSamples) {
            std::vector<int16_t> part(pcm.begin(), pcm.begin() + g_chunkSamples);
            pcm.erase(pcm.begin(), pcm.begin() + g_chunkSamples);
            fed = pcm.size();
            chunk(part);
        }
        if (g_stop) finished = true;
        if (!longRec && GetTickCount64() - started > 26000) finished = true;
    }
    heard = longRec ? !pcm.empty() : vad.HeardSpeech() || (g_stop && pcm.size() > 8000);
    Logger::Write(L"cattura: fine, voce " + std::to_wstring(heard ? 1 : 0));
    if (!longRec) Logger::WriteUtf8(("cattura vad: " + vad.Trace()).c_str());
    client->Stop();
    CoTaskMemFree(fmt);
    CloseHandle(ready);
    done(pcm, heard, error);
}

}  // namespace

HRESULT Activation::ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) {
    HRESULT act = E_FAIL;
    ComPtr<IUnknown> unk;
    HRESULT hr = op->GetActivateResult(&act, &unk);
    if (SUCCEEDED(hr)) hr = act;
    ComPtr<IAudioClient> client;
    if (SUCCEEDED(hr)) hr = unk.As(&client);
    Capture::LevelFn lv = level;
    Capture::DoneFn dn = done;
    if (FAILED(hr)) {
        dn(std::vector<int16_t>(), false, Hr(L"attivazione microfono", hr));
        return S_OK;
    }
    Windows::System::Threading::ThreadPool::RunAsync(
        ref new Windows::System::Threading::WorkItemHandler([client, lv, dn](Windows::Foundation::IAsyncAction^) {
            Run(client, lv, dn);
        }),
        Windows::System::Threading::WorkItemPriority::High, Windows::System::Threading::WorkItemOptions::TimeSliced);
    return S_OK;
}

void Capture::StartLong(const std::wstring& deviceId, unsigned chunkSamples, LevelFn level, ChunkFn chunk, DoneFn done) {
    Begin(deviceId, level, done, chunk, chunkSamples);
}

void Capture::Start(const std::wstring& deviceId, LevelFn level, DoneFn done) {
    Begin(deviceId, level, done, nullptr, 0);
}

// Called on the UI thread: WASAPI activation for capture must start there (consent and app identity).
void Capture::Begin(const std::wstring& deviceId, LevelFn level, DoneFn done, ChunkFn chunk, unsigned chunkSamples) {
    InterlockedExchange(&g_stop, 0);
    g_chunk = chunk;   // read by Run, after the asynchronous activation
    g_chunkSamples = chunkSamples;
    std::wstring id = deviceId;
    if (id.empty()) {
        Platform::String^ def = Windows::Media::Devices::MediaDevice::GetDefaultAudioCaptureId(
            Windows::Media::Devices::AudioDeviceRole::Default);
        id = def->Data();
    }
    ComPtr<Activation> handler = Make<Activation>();
    handler->level = level;
    handler->done = done;
    ComPtr<IActivateAudioInterfaceAsyncOperation> op;
    HRESULT hr = ActivateAudioInterfaceAsync(id.c_str(), __uuidof(IAudioClient), nullptr, handler.Get(), &op);
    if (FAILED(hr)) done(std::vector<int16_t>(), false, Hr(L"ActivateAudioInterfaceAsync", hr));
}

void Capture::MuteStart(unsigned ms) {
    InterlockedExchange(&g_muteMs, static_cast<LONG>(ms));
}

void Capture::SetStream(ChunkFn fn, bool endsUtterance) {
    g_stream = fn;
    g_streamEnds = fn && endsUtterance;
}

void Capture::StopNow() {
    InterlockedExchange(&g_stop, 1);
}

std::wstring Capture::LastFormat() {
    return g_format;
}
