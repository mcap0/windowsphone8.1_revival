#include "pch.h"
#include "Sounds.h"
#include "Logger.h"

#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")

using Microsoft::WRL::ComPtr;

namespace {
const wchar_t* const Files[Sounds::Count] = {L"ascolto.wav", L"fine.wav", L"risposta.wav"};
const wchar_t* const Names[Sounds::Count] = {L"ascolto", L"fine", L"risposta"};

struct Clip {
    WAVEFORMATEX format;
    std::vector<BYTE> data;
    IXAudio2SourceVoice* voice;
};

ComPtr<IXAudio2> g_audio;
IXAudio2MasteringVoice* g_master;
Clip g_clips[Sounds::Count];
bool g_ready, g_tried;

// PCM WAV from the package: "fmt " and "data" chunks.
bool Load(const std::wstring& path, Clip& clip) {
    HANDLE f = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::vector<BYTE> all;
    BYTE buf[16384];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0) all.insert(all.end(), buf, buf + got);
    CloseHandle(f);
    if (all.size() < 12 || memcmp(&all[0], "RIFF", 4) != 0 || memcmp(&all[8], "WAVE", 4) != 0) return false;
    bool fmt = false;
    for (size_t p = 12; p + 8 <= all.size();) {
        DWORD size = *reinterpret_cast<DWORD*>(&all[p + 4]);
        if (p + 8 + size > all.size()) size = static_cast<DWORD>(all.size() - p - 8);
        if (memcmp(&all[p], "fmt ", 4) == 0 && size >= 16) {
            memset(&clip.format, 0, sizeof(clip.format));
            memcpy(&clip.format, &all[p + 8], 16);
            fmt = true;
        } else if (memcmp(&all[p], "data", 4) == 0) {
            clip.data.assign(all.begin() + p + 8, all.begin() + p + 8 + size);
        }
        p += 8 + size + (size & 1);
    }
    return fmt && !clip.data.empty() && clip.format.wFormatTag == WAVE_FORMAT_PCM;
}

bool Ready() {
    if (g_tried) return g_ready;
    g_tried = true;
    ULONGLONG start = GetTickCount64();
    HRESULT hr = XAudio2Create(&g_audio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (SUCCEEDED(hr)) hr = g_audio->CreateMasteringVoice(&g_master);
    if (FAILED(hr)) {
        wchar_t b[64];
        swprintf_s(b, L"suoni: XAudio2 0x%08X", static_cast<unsigned>(hr));
        Logger::Write(b);
        return false;
    }
    std::wstring dir = std::wstring(Windows::ApplicationModel::Package::Current->InstalledLocation->Path->Data()) + L"\\Assets\\";
    for (int i = 0; i < Sounds::Count; ++i) {
        g_clips[i].voice = nullptr;
        if (!Load(dir + Files[i], g_clips[i])) {
            Logger::Write(L"suoni: non carico " + std::wstring(Files[i]));
            continue;
        }
        // the source voices too: the first Play must not create them while the microphone is already opening
        if (FAILED(g_audio->CreateSourceVoice(&g_clips[i].voice, &g_clips[i].format))) {
            g_clips[i].voice = nullptr;
            Logger::Write(L"suoni: voce non creata " + std::wstring(Files[i]));
        }
    }
    g_ready = true;
    Logger::Write(L"suoni pronti in " + std::to_wstring(GetTickCount64() - start) + L" ms");
    return true;
}
}  // namespace

void Sounds::Prepare() { Ready(); }

void Sounds::Play(Id id) {
    if (!Ready() || g_clips[id].data.empty()) return;
    Clip& c = g_clips[id];
    if (c.voice == nullptr && FAILED(g_audio->CreateSourceVoice(&c.voice, &c.format))) {
        c.voice = nullptr;
        return;
    }
    c.voice->Stop();
    c.voice->FlushSourceBuffers();
    XAUDIO2_BUFFER b = {};
    b.Flags = XAUDIO2_END_OF_STREAM;
    b.AudioBytes = static_cast<UINT32>(c.data.size());
    b.pAudioData = &c.data[0];
    c.voice->SubmitSourceBuffer(&b);
    c.voice->Start();
    Logger::Write(std::wstring(L"suono: ") + Names[id]);
}

void Sounds::Stop(Id id) {
    if (!g_ready || g_clips[id].voice == nullptr) return;
    g_clips[id].voice->Stop();
    g_clips[id].voice->FlushSourceBuffers();
}
