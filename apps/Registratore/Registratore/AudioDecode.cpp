#include "pch.h"
#include "AudioDecode.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace
{
    void le16(std::string& o, unsigned v) { o += static_cast<char>(v & 255); o += static_cast<char>((v >> 8) & 255); }
    void le32(std::string& o, unsigned v) { for (int i = 0; i < 4; ++i) o += static_cast<char>((v >> (8 * i)) & 255); }

    std::string Fail(const char* what, HRESULT hr)
    {
        char b[96];
        sprintf_s(b, "%s failed 0x%08X", what, static_cast<unsigned>(hr));
        return b;
    }
}

bool DecodeToPcmWav(const std::wstring& path, std::string& wav, std::string& error, int* peakPercent, double* seconds,
                    int* meanPercent, std::vector<int>* slicePeaks)
{
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr)) { error = Fail("MFStartup", hr); return false; }
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader);
    if (FAILED(hr)) { error = Fail("MFCreateSourceReaderFromURL", hr); MFShutdown(); return false; }
    reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    hr = reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want.Get());
    if (FAILED(hr)) { error = Fail("SetCurrentMediaType(PCM)", hr); reader.Reset(); MFShutdown(); return false; }
    ComPtr<IMFMediaType> got;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &got);
    UINT32 rate = 0, channels = 0;
    got->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
    got->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
    if (!rate || !channels) { error = "no audio format"; reader.Reset(); MFShutdown(); return false; }

    std::string pcm;
    int peak = 0;
    unsigned long long sum = 0, count = 0;
    size_t sliceLen = rate * channels / 4 > 0 ? rate * channels / 4 : 1;
    size_t inSlice = 0;
    int slicePeak = 0;
    for (;;)
    {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, &sample);
        if (FAILED(hr)) { error = Fail("ReadSample", hr); break; }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (!sample) continue;
        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
        BYTE* data = nullptr;
        DWORD len = 0;
        if (SUCCEEDED(buf->Lock(&data, nullptr, &len)))
        {
            pcm.append(reinterpret_cast<const char*>(data), len);
            const int16_t* s = reinterpret_cast<const int16_t*>(data);
            for (DWORD i = 0; i < len / 2; ++i)
            {
                int v = s[i] < 0 ? -s[i] : s[i];
                if (v > peak) peak = v;
                if (v > slicePeak) slicePeak = v;
                sum += v;
                ++count;
                if (++inSlice == sliceLen)
                {
                    if (slicePeaks) slicePeaks->push_back(slicePeak * 100 / 32767);
                    inSlice = 0;
                    slicePeak = 0;
                }
            }
            buf->Unlock();
        }
    }
    if (slicePeaks && inSlice > 0) slicePeaks->push_back(slicePeak * 100 / 32767);
    reader.Reset();
    MFShutdown();
    if (pcm.empty()) { if (error.empty()) error = "no audio samples"; return false; }

    wav.clear();
    wav += "RIFF"; le32(wav, 36 + static_cast<unsigned>(pcm.size())); wav += "WAVE";
    wav += "fmt "; le32(wav, 16); le16(wav, 1); le16(wav, channels); le32(wav, rate); le32(wav, rate * channels * 2);
    le16(wav, channels * 2); le16(wav, 16);
    wav += "data"; le32(wav, static_cast<unsigned>(pcm.size()));
    wav += pcm;
    if (peakPercent) *peakPercent = peak * 100 / 32767;
    if (meanPercent) *meanPercent = count ? static_cast<int>(sum * 100 / count / 32767) : 0;
    if (seconds) *seconds = static_cast<double>(pcm.size()) / (rate * channels * 2);
    return true;
}
