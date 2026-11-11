#include "config.h"
#include "MediaDecoder.h"

#include "Engine.h"
#include <mutex>
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>

namespace Lumia {

namespace {

template<typename T> class Com {
    WTF_MAKE_NONCOPYABLE(Com);
public:
    Com() = default;
    ~Com() { reset(); }
    T* get() const { return m_p; }
    T* operator->() const { return m_p; }
    explicit operator bool() const { return m_p; }
    T** operator&()
    {
        reset();
        return &m_p;
    }
    void reset()
    {
        if (m_p)
            m_p->Release();
        m_p = nullptr;
    }

private:
    T* m_p { nullptr };
};

// The decoders by class, when the enumeration finds none: AAC (MSAUDDECMFT), MP3 (MP3DMOD), H.264.
const GUID clsidAacDecoder = { 0x32d186a7, 0x218f, 0x4c75, { 0x88, 0x76, 0xdd, 0x77, 0x27, 0x3a, 0x89, 0x99 } };
const GUID clsidMp3Decoder = { 0xbbeea841, 0x0a63, 0x4f52, { 0xa7, 0xab, 0xa9, 0xb3, 0xa8, 0x4e, 0xd3, 0x8a } };
const GUID clsidH264Decoder = { 0x62ce7e72, 0x4c71, 0x4d20, { 0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d } };

constexpr unsigned fourcc(char a, char b, char c, char d)
{
    return static_cast<unsigned>(static_cast<uint8_t>(a)) | (static_cast<unsigned>(static_cast<uint8_t>(b)) << 8)
        | (static_cast<unsigned>(static_cast<uint8_t>(c)) << 16) | (static_cast<unsigned>(static_cast<uint8_t>(d)) << 24);
}
constexpr unsigned layoutNV12 = fourcc('N', 'V', '1', '2');
constexpr unsigned layoutIYUV = fourcc('I', 'Y', 'U', 'V');
constexpr unsigned layoutI420 = fourcc('I', '4', '2', '0');
constexpr unsigned layoutYV12 = fourcc('Y', 'V', '1', '2');

bool startMediaFoundation()
{
    static std::once_flag once;
    static HRESULT result = E_FAIL;
    std::call_once(once, [] {
        result = MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(result))
            log("media: MFStartup 0x%08X", static_cast<unsigned>(result));
    });
    return SUCCEEDED(result);
}

// The system's decoder for the type: the first synchronous one of the list (an app container cannot create the H.264
// decoder by class, the list's own activation object can: PLAN §64.2), else by class.
IMFTransform* createTransform(const GUID& category, const GUID& major, const GUID& subtype, const GUID& fallback, String& name)
{
    IMFTransform* transform = nullptr;
    MFT_REGISTER_TYPE_INFO input = { major, subtype };
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(category, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER, &input, nullptr, &activates, &count);
    if (SUCCEEDED(hr)) {
        for (UINT32 i = 0; i < count; ++i) {
            if (!transform && SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform))) && transform) {
                WCHAR* friendly = nullptr;
                UINT32 length = 0;
                if (SUCCEEDED(activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &length)) && friendly) {
                    name = String(std::span<const char16_t> { reinterpret_cast<const char16_t*>(friendly), length });
                    CoTaskMemFree(friendly);
                }
            }
            activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }
    if (!transform) {
        hr = CoCreateInstance(fallback, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform));
        if (FAILED(hr)) {
            name = makeString("nessun decoder (0x"_s, hex(static_cast<unsigned>(hr), 8), ')');
            return nullptr;
        }
        name = "decoder per classe"_s;
    }
    return transform;
}

bool makeSample(std::span<const uint8_t> bytes, long long ptsUs, long long durationUs, Com<IMFSample>& sample)
{
    Com<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(bytes.size()), &buffer)))
        return false;
    BYTE* data = nullptr;
    if (FAILED(buffer->Lock(&data, nullptr, nullptr)))
        return false;
    memcpy(data, bytes.data(), bytes.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(bytes.size()));
    if (FAILED(MFCreateSample(&sample)))
        return false;
    sample->AddBuffer(buffer.get());
    sample->SetSampleTime(ptsUs * 10);
    if (durationUs > 0)
        sample->SetSampleDuration(durationUs * 10);
    return true;
}

} // namespace

std::unique_ptr<MediaDecoder> MediaDecoder::create(const Media::Track& track, String& error)
{
    if (!startMediaFoundation()) {
        error = "Media Foundation non parte"_s;
        return nullptr;
    }
    std::unique_ptr<MediaDecoder> decoder { new MediaDecoder };
    if (!decoder->setUp(track, error))
        return nullptr;
    return decoder;
}

MediaDecoder::~MediaDecoder()
{
    if (m_transform) {
        if (m_started) {
            m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        }
        m_transform->Release();
    }
}

bool MediaDecoder::setUp(const Media::Track& track, String& error)
{
    m_video = track.video;
    String name;
    Com<IMFMediaType> type;
    if (FAILED(MFCreateMediaType(&type))) {
        error = "MFCreateMediaType"_s;
        return false;
    }
    HRESULT hr = E_FAIL;
    if (track.codec == Media::Codec::H264) {
        m_transform = createTransform(MFT_CATEGORY_VIDEO_DECODER, MFMediaType_Video, MFVideoFormat_H264, clsidH264Decoder, name);
        if (!m_transform) {
            error = makeString("H.264: "_s, name);
            return false;
        }
        m_declaredWidth = track.width;
        m_declaredHeight = track.height;
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        if (track.width > 0 && track.height > 0)
            MFSetAttributeSize(type.get(), MF_MT_FRAME_SIZE, track.width, track.height);
        type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_MixedInterlaceOrProgressive);
        hr = m_transform->SetInputType(0, type.get(), 0);
        if (FAILED(hr)) {
            type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            hr = m_transform->SetInputType(0, type.get(), 0);
        }
    } else if (track.codec == Media::Codec::AAC) {
        m_transform = createTransform(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio, MFAudioFormat_AAC, clsidAacDecoder, name);
        if (!m_transform) {
            error = makeString("AAC: "_s, name);
            return false;
        }
        // The format block of an AAC wave format after its WAVEFORMATEX: payload type 0 (raw frames), profile and
        // level unknown (0xFE), then the AudioSpecificConfig.
        Vector<uint8_t> user(FillWith { }, 12 + track.config.size(), static_cast<uint8_t>(0));
        user[2] = 0xFE;
        if (!track.config.empty())
            memcpy(user.mutableSpan().data() + 12, track.config.data(), track.config.size());
        // With spectral band replication the stream's own rate is half the one it plays at: either may be the one
        // the decoder wants to be told.
        int coreRate = 0;
        if (track.config.size() >= 2) {
            static const int rates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350 };
            unsigned index = ((track.config[0] & 7) << 1) | (track.config[1] >> 7);
            if (index < 13)
                coreRate = rates[index];
        }
        int rates[2] = { track.sampleRate, coreRate };
        for (int rate : rates) {
            if (rate <= 0)
                continue;
            type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
            type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0xFE);
            type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
            type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, track.channels > 0 ? track.channels : 2);
            type->SetBlob(MF_MT_USER_DATA, user.span().data(), static_cast<UINT32>(user.size()));
            hr = m_transform->SetInputType(0, type.get(), 0);
            if (SUCCEEDED(hr))
                break;
        }
    } else if (track.codec == Media::Codec::MP3) {
        m_transform = createTransform(MFT_CATEGORY_AUDIO_DECODER, MFMediaType_Audio, MFAudioFormat_MP3, clsidMp3Decoder, name);
        if (!m_transform) {
            error = makeString("MP3: "_s, name);
            return false;
        }
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, track.sampleRate);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, track.channels > 0 ? track.channels : 2);
        hr = m_transform->SetInputType(0, type.get(), 0);
    } else {
        error = "codec non previsto"_s;
        return false;
    }
    if (FAILED(hr)) {
        error = makeString(String::fromUTF8(track.name.c_str()), ": il decoder rifiuta il formato (0x"_s, hex(static_cast<unsigned>(hr), 8), ')');
        return false;
    }
    // H.264: the output types exist once the decoder has read the stream's parameter sets (pull() sets them then).
    selectOutputType();
    if (!m_video && !m_outputSet) {
        error = makeString(String::fromUTF8(track.name.c_str()), ": nessuna uscita PCM a 16 bit"_s);
        return false;
    }
    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    m_started = true;
    m_description = name;
    return true;
}

bool MediaDecoder::selectOutputType()
{
    Com<IMFMediaType> chosen;
    unsigned chosenLayout = 0;
    int rank = 0;
    for (DWORD i = 0;; ++i) {
        Com<IMFMediaType> type;
        if (FAILED(m_transform->GetOutputAvailableType(0, i, &type)))
            break;
        GUID subtype = { };
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (!m_video) {
            UINT32 bits = 0;
            type->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits);
            if (subtype == MFAudioFormat_PCM && bits == 16) {
                type->AddRef();
                *&chosen = type.get();
                break;
            }
            continue;
        }
        // Three planes as they are wanted first, then the two-plane layout every decoder has.
        unsigned layout = subtype.Data1;
        int r = layout == layoutIYUV || layout == layoutI420 ? 3 : layout == layoutNV12 ? 2 : layout == layoutYV12 ? 1 : 0;
        if (r > rank) {
            rank = r;
            chosenLayout = layout;
            type->AddRef();
            *&chosen = type.get();
        }
    }
    if (!chosen)
        return false;
    if (FAILED(m_transform->SetOutputType(0, chosen.get(), 0)))
        return false;
    if (!m_video) {
        UINT32 rate = 0, channels = 0;
        chosen->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        chosen->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        if (!rate || !channels)
            return false;
        m_rate = static_cast<int>(rate);
        m_channels = static_cast<int>(channels);
        m_outputSet = true;
        return true;
    }
    UINT32 width = 0, height = 0;
    MFGetAttributeSize(chosen.get(), MF_MT_FRAME_SIZE, &width, &height);
    if (!width || !height)
        return false;
    UINT32 stride = 0;
    if (FAILED(chosen->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) || static_cast<INT32>(stride) <= 0)
        stride = width;
    m_layout = chosenLayout;
    m_frameWidth = static_cast<int>(width);
    m_frameHeight = static_cast<int>(height);
    m_stride = static_cast<int>(stride);
    // The part of the coded frame to show.
    m_cropX = m_cropY = 0;
    m_width = m_declaredWidth > 0 ? std::min(m_declaredWidth, m_frameWidth) : m_frameWidth;
    m_height = m_declaredHeight > 0 ? std::min(m_declaredHeight, m_frameHeight) : m_frameHeight;
    MFVideoArea area = { };
    if (SUCCEEDED(chosen->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))
        && area.Area.cx > 0 && area.Area.cy > 0 && area.OffsetX.value >= 0 && area.OffsetY.value >= 0
        && area.OffsetX.value + area.Area.cx <= m_frameWidth && area.OffsetY.value + area.Area.cy <= m_frameHeight) {
        m_cropX = area.OffsetX.value & ~1;
        m_cropY = area.OffsetY.value & ~1;
        m_width = area.Area.cx;
        m_height = area.Area.cy;
    }
    bool first = !m_outputSet;
    m_outputSet = true;
    if (first)
        log("media: H.264 in uscita %c%c%c%c %dx%d (riga %d), mostrati %dx%d", static_cast<char>(m_layout), static_cast<char>(m_layout >> 8),
            static_cast<char>(m_layout >> 16), static_cast<char>(m_layout >> 24), m_frameWidth, m_frameHeight, m_stride, m_width, m_height);
    return true;
}

bool MediaDecoder::decode(std::span<const uint8_t> bytes, long long ptsUs, long long durationUs)
{
    if (!m_transform || bytes.empty())
        return false;
    Com<IMFSample> sample;
    if (!makeSample(bytes, ptsUs, durationUs, sample))
        return false;
    HRESULT hr = m_transform->ProcessInput(0, sample.get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        pull();
        hr = m_transform->ProcessInput(0, sample.get(), 0);
    }
    if (FAILED(hr)) {
        if (++m_failures == 1 || !(m_failures % 100))
            log("media: %s rifiuta un campione (0x%08X, %u volte)", m_video ? "il decoder video" : "il decoder audio", static_cast<unsigned>(hr), m_failures);
        return false;
    }
    pull();
    return true;
}

void MediaDecoder::drain()
{
    if (!m_transform)
        return;
    m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    pull();
}

void MediaDecoder::flush()
{
    if (m_transform)
        m_transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_audio.clear();
    m_pictures.clear();
}

void MediaDecoder::pull()
{
    for (int round = 0; round < 64; ++round) {
        if (!m_outputSet)
            selectOutputType();
        MFT_OUTPUT_STREAM_INFO info = { };
        m_transform->GetOutputStreamInfo(0, &info);
        bool provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
        MFT_OUTPUT_DATA_BUFFER output = { };
        Com<IMFSample> own;
        if (!provides) {
            DWORD size = info.cbSize;
            if (!size) {
                int w = (std::max(m_frameWidth, m_declaredWidth) + 15) & ~15;
                int h = (std::max(m_frameHeight, m_declaredHeight) + 31) & ~15;
                size = m_video ? static_cast<DWORD>(w * h * 3 / 2) : 64 * 1024;
            }
            Com<IMFMediaBuffer> buffer;
            if (FAILED(MFCreateMemoryBuffer(size, &buffer)) || FAILED(MFCreateSample(&own)))
                return;
            own->AddBuffer(buffer.get());
            output.pSample = own.get();
        }
        DWORD status = 0;
        HRESULT hr = m_transform->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents)
            output.pEvents->Release();
        IMFSample* produced = provides ? output.pSample : nullptr;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            if (produced)
                produced->Release();
            m_outputSet = false;
            if (!selectOutputType())
                return; // not known yet: more input first
            continue;
        }
        if (FAILED(hr)) {
            if (produced)
                produced->Release();
            if (hr != MF_E_TRANSFORM_NEED_MORE_INPUT && (++m_failures == 1 || !(m_failures % 100)))
                log("media: %s non produce (0x%08X, %u volte)", m_video ? "il decoder video" : "il decoder audio", static_cast<unsigned>(hr), m_failures);
            return;
        }
        if (IMFSample* sample = output.pSample) {
            if (m_video)
                takePicture(sample);
            else
                takeAudio(sample);
        }
        if (produced)
            produced->Release();
    }
}

void MediaDecoder::takeAudio(IMFSample* sample)
{
    Com<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        return;
    BYTE* data = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length)))
        return;
    DecodedAudio audio;
    audio.rate = m_rate;
    audio.channels = m_channels;
    size_t count = length / sizeof(int16_t);
    count -= count % m_channels;
    audio.samples.grow(count);
    memcpy(audio.samples.mutableSpan().data(), data, count * sizeof(int16_t));
    buffer->Unlock();
    LONGLONG time = 0;
    audio.ptsUs = SUCCEEDED(sample->GetSampleTime(&time)) ? time / 10 : m_nextAudioUs;
    m_nextAudioUs = audio.ptsUs + static_cast<long long>(count / m_channels) * 1000000 / m_rate;
    if (count)
        m_audio.append(WTF::move(audio));
}

void MediaDecoder::takePicture(IMFSample* sample)
{
    if (!m_outputSet || m_width <= 0 || m_height <= 0)
        return;
    Com<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        return;
    BYTE* data = nullptr;
    DWORD length = 0;
    buffer->GetCurrentLength(&length);
    LONG pitch = m_stride;
    Com<IMF2DBuffer> buffer2D;
    bool locked2D = false;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(&buffer2D)))) {
        BYTE* first = nullptr;
        LONG rowPitch = 0;
        if (SUCCEEDED(buffer2D->Lock2D(&first, &rowPitch))) {
            if (rowPitch > 0 && first) {
                data = first;
                pitch = rowPitch;
                locked2D = true;
            } else
                buffer2D->Unlock2D();
        }
    }
    if (!locked2D && FAILED(buffer->Lock(&data, nullptr, &length)))
        return;

    // The chroma follows the luma rows; a decoder that pads the frame's height says so only through the length.
    size_t rows = m_frameHeight;
    size_t whole = static_cast<size_t>(pitch) * rows * 3 / 2;
    if (length > whole + pitch)
        rows = static_cast<size_t>(length) * 2 / (3 * static_cast<size_t>(pitch));
    whole = static_cast<size_t>(pitch) * rows * 3 / 2;
    bool fits = length >= whole && m_cropX + m_width <= pitch && static_cast<size_t>(m_cropY + m_height) <= rows;
    if (fits) {
        auto picture = VideoPicture::create();
        picture->width = m_width;
        picture->height = m_height;
        picture->bt709 = m_height >= 720;
        int cw = picture->chromaWidth(), ch = picture->chromaHeight();
        picture->bytes.grow(static_cast<size_t>(m_width) * m_height + 2 * static_cast<size_t>(cw) * ch);
        uint8_t* out = picture->bytes.mutableSpan().data();
        for (int row = 0; row < m_height; ++row)
            memcpy(out + static_cast<size_t>(row) * m_width, data + static_cast<size_t>(m_cropY + row) * pitch + m_cropX, m_width);
        uint8_t* outU = out + static_cast<size_t>(m_width) * m_height;
        uint8_t* outV = outU + static_cast<size_t>(cw) * ch;
        const uint8_t* chroma = data + static_cast<size_t>(pitch) * rows;
        // The last chroma column or row of an odd size repeats the one before when the source has none.
        int sourceColumns = (pitch - m_cropX) / 2;
        int sourceRows = static_cast<int>(rows / 2) - m_cropY / 2;
        if (m_layout == layoutNV12) {
            for (int row = 0; row < ch; ++row) {
                const uint8_t* in = chroma + static_cast<size_t>(m_cropY / 2 + std::min(row, sourceRows - 1)) * pitch + m_cropX;
                uint8_t* u = outU + static_cast<size_t>(row) * cw;
                uint8_t* v = outV + static_cast<size_t>(row) * cw;
                for (int x = 0; x < cw; ++x) {
                    int column = std::min(x, sourceColumns - 1);
                    u[x] = in[2 * column];
                    v[x] = in[2 * column + 1];
                }
            }
        } else {
            size_t halfPitch = pitch / 2;
            const uint8_t* first = chroma;
            const uint8_t* second = chroma + halfPitch * (rows / 2);
            const uint8_t* inU = m_layout == layoutYV12 ? second : first;
            const uint8_t* inV = m_layout == layoutYV12 ? first : second;
            int copy = std::min(cw, static_cast<int>(halfPitch) - m_cropX / 2);
            for (int row = 0; row < ch; ++row) {
                size_t at = static_cast<size_t>(m_cropY / 2 + std::min(row, sourceRows - 1)) * halfPitch + m_cropX / 2;
                memcpy(outU + static_cast<size_t>(row) * cw, inU + at, copy);
                memcpy(outV + static_cast<size_t>(row) * cw, inV + at, copy);
                if (copy < cw) {
                    outU[static_cast<size_t>(row) * cw + copy] = outU[static_cast<size_t>(row) * cw + copy - 1];
                    outV[static_cast<size_t>(row) * cw + copy] = outV[static_cast<size_t>(row) * cw + copy - 1];
                }
            }
        }
        LONGLONG time = 0, duration = 0;
        if (SUCCEEDED(sample->GetSampleTime(&time)))
            picture->ptsUs = time / 10;
        if (SUCCEEDED(sample->GetSampleDuration(&duration)))
            picture->durationUs = duration / 10;
        m_pictures.append(WTF::move(picture));
    } else if (++m_failures == 1)
        log("media: fotogramma di %lu byte, attesi %zu (riga %ld, %zu righe)", static_cast<unsigned long>(length), whole, static_cast<long>(pitch), rows);
    if (locked2D)
        buffer2D->Unlock2D();
    else
        buffer->Unlock();
}

} // namespace Lumia
