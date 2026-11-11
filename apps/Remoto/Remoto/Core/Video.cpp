#include "pch.h"
#include "Video.h"
#include <windows.ui.xaml.media.dxinterop.h>
#include <mferror.h>
#include <cstdio>

using Microsoft::WRL::ComPtr;

extern "C" void* __imp_MFCreateSample;

namespace
{
    // Microsoft's H.264 decoder MFT (CLSID_CMSH264DecoderMFT). MFTEnumEx is desktop-only on WP8.1, so it is asked for
    // by name; with a D3D11 device manager it decodes through DXVA on the GPU.
    const GUID clsidH264Decoder = { 0x62CE7E72, 0x4C71, 0x4d20, { 0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D } };

    // ID3D10Multithread (d3d10.h is not in the phone SDK): the device is shared with the decoder's own threads.
    MIDL_INTERFACE("9B7E4E00-342C-4106-A19F-4F2704F689F0")
    IMultithreadLocal : public IUnknown
    {
        virtual void STDMETHODCALLTYPE Enter() = 0;
        virtual void STDMETHODCALLTYPE Leave() = 0;
        virtual BOOL STDMETHODCALLTYPE SetMultithreadProtected(BOOL protect) = 0;
        virtual BOOL STDMETHODCALLTYPE GetMultithreadProtected() = 0;
    };

    long long NowUs()
    {
        static LARGE_INTEGER frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return now.QuadPart * 1000000 / frequency.QuadPart;
    }

    std::string Hex(HRESULT hr)
    {
        char text[16];
        sprintf_s(text, "0x%08X", static_cast<unsigned>(hr));
        return text;
    }

    // MFTEnumEx exists in the phone's mfplat.dll but the SDK keeps it for desktop apps: found through the module of a
    // function we import (its import slot points into mfplat.dll). Only to list the decoders the phone has.
    // Desktop-only in the phone SDK too: the same layout and values as mfapi.h / mfobjects.h.
    struct TypeInfo { GUID major; GUID subtype; };
    const UINT32 enumSync = 0x1, enumAsync = 0x2, enumHardware = 0x4, enumLocal = 0x10;
    const GUID friendlyNameAttribute = { 0x314ffbae, 0x5b41, 0x4c95, { 0x9c, 0x19, 0x4e, 0x7d, 0x58, 0x6f, 0xac, 0xe3 } };
    const GUID transformClsidAttribute = { 0x6821c42b, 0x65a4, 0x4e82, { 0x99, 0xbc, 0x9a, 0x88, 0x20, 0x5e, 0xcd, 0x0c } };
    typedef HRESULT(WINAPI* MFTEnumExFn)(GUID, UINT32, const TypeInfo*, const TypeInfo*, IMFActivate***, UINT32*);
    MFTEnumExFn FindMFTEnumEx()
    {
        // The import slot of MFCreateSample holds its address inside mfplat.dll (&MFCreateSample would be our thunk).
        MEMORY_BASIC_INFORMATION info = {};
        if (!VirtualQuery(__imp_MFCreateSample, &info, sizeof info))
            return nullptr;
        return reinterpret_cast<MFTEnumExFn>(GetProcAddress(static_cast<HMODULE>(info.AllocationBase), "MFTEnumEx"));
    }

    unsigned char Clamp(int v) { return static_cast<unsigned char>(v < 0 ? 0 : v > 255 ? 255 : v); }
}

void Video::Fail(const char* what, HRESULT hr)
{
    std::string failure = std::string(what) + " " + Hex(hr);
    if (failure == m_lastFailure)
        return; // once, not at every frame
    m_lastFailure = failure;
    if (log)
        log("video: " + m_lastFailure);
}

bool Video::CreateDevice(Windows::UI::Xaml::Controls::SwapChainPanel^ panel, int width, int height, float scale)
{
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1 };
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_9_1;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &m_device, &got, &m_context);
    bool videoFlag = SUCCEEDED(hr);
    if (FAILED(hr))
    {
        flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &m_device, &got, &m_context);
    }
    if (FAILED(hr))
    {
        Fail("D3D11CreateDevice", hr);
        return false;
    }
    ComPtr<IMultithreadLocal> multithread;
    if (SUCCEEDED(m_device.As(&multithread)))
        multithread->SetMultithreadProtected(TRUE);
    m_device.As(&m_videoDevice);
    m_context.As(&m_videoContext);

    ComPtr<IDXGIDevice> dxgi;
    m_device.As(&dxgi);
    ComPtr<IDXGIAdapter> adapter;
    dxgi->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> factory;
    adapter->GetParent(IID_PPV_ARGS(&factory));
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = factory->CreateSwapChainForComposition(m_device.Get(), &desc, nullptr, &m_swap);
    if (FAILED(hr))
    {
        Fail("CreateSwapChainForComposition", hr);
        return false;
    }
    ComPtr<IDXGISwapChain2> swap2;
    if (SUCCEEDED(m_swap.As(&swap2)) && scale > 0)
    {
        DXGI_MATRIX_3X2_F m = { 1.0f / scale, 0, 0, 1.0f / scale, 0, 0 };
        swap2->SetMatrixTransform(&m);
    }
    // At most one frame queued behind the one on screen: a picture is never shown later than it has to be.
    ComPtr<IDXGIDevice1> dxgi1;
    if (SUCCEEDED(m_device.As(&dxgi1)))
        dxgi1->SetMaximumFrameLatency(1);
    ComPtr<ISwapChainPanelNative> native;
    hr = reinterpret_cast<IUnknown*>(panel)->QueryInterface(IID_PPV_ARGS(&native));
    if (SUCCEEDED(hr))
        hr = native->SetSwapChain(m_swap.Get());
    if (FAILED(hr))
    {
        Fail("SetSwapChain", hr);
        return false;
    }
    m_width = width;
    m_height = height;
    char text[200];
    sprintf_s(text, "D3D FL %x, flag video %s, ID3D11VideoDevice %s, swap chain %dx%d scala %.2f", static_cast<unsigned>(got),
              videoFlag ? "si" : "no", m_videoDevice ? "si" : "no", width, height, scale);
    m_describe = text;
    if (log)
        log(std::string("video: ") + text);
    return true;
}

bool Video::CreateDecoder(int width, int height)
{
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr))
    {
        Fail("MFStartup", hr);
        return false;
    }
    if (auto enumerate = FindMFTEnumEx())
    {
        TypeInfo in = { MFMediaType_Video, MFVideoFormat_H264 };
        const UINT32 kinds[] = { enumSync, enumAsync, enumHardware };
        const char* names[] = { "sync", "async", "hardware" };
        for (int k = 0; k < 3; ++k)
        {
            IMFActivate** activates = nullptr;
            UINT32 count = 0;
            HRESULT e = enumerate(MFT_CATEGORY_VIDEO_DECODER, kinds[k] | enumLocal, &in, nullptr, &activates, &count);
            std::string list = std::string("video: decoder H.264 ") + names[k] + ": " + Hex(e) + " " + std::to_string(count);
            for (UINT32 i = 0; i < count; ++i)
            {
                WCHAR* name = nullptr;
                UINT32 length = 0;
                GUID clsid = {};
                activates[i]->GetGUID(transformClsidAttribute, &clsid);
                if (SUCCEEDED(activates[i]->GetAllocatedString(friendlyNameAttribute, &name, &length)))
                {
                    list += " [" + std::string(name, name + length);
                    CoTaskMemFree(name);
                }
                else
                    list += " [?";
                char guid[48];
                sprintf_s(guid, " %08lX]", clsid.Data1);
                list += guid;
                // The app cannot CoCreateInstance it (0x80040154), but the system's own activation object can.
                if (!m_decoder && kinds[k] == enumSync)
                {
                    HRESULT a = activates[i]->ActivateObject(IID_PPV_ARGS(&m_decoder));
                    list += " attivato " + Hex(a);
                }
                activates[i]->Release();
            }
            CoTaskMemFree(activates);
            if (log)
                log(list);
        }
    }
    else if (log)
        log("video: MFTEnumEx non trovata");
    if (!m_decoder)
        hr = CoCreateInstance(clsidH264Decoder, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_decoder));
    if (!m_decoder)
    {
        Fail("CoCreateInstance(decoder H.264)", hr);
        return false;
    }
    ComPtr<IMFAttributes> attributes;
    UINT32 d3dAware = 0;
    if (SUCCEEDED(m_decoder->GetAttributes(&attributes)) && attributes)
    {
        attributes->SetUINT32(MF_LOW_LATENCY, TRUE);
        attributes->GetUINT32(MF_SA_D3D11_AWARE, &d3dAware);
    }
    if (d3dAware && m_videoDevice)
    {
        UINT token = 0;
        hr = MFCreateDXGIDeviceManager(&token, &m_manager);
        if (SUCCEEDED(hr))
            hr = m_manager->ResetDevice(m_device.Get(), token);
        if (SUCCEEDED(hr))
            hr = m_decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_manager.Get()));
        m_d3dDecoder = SUCCEEDED(hr);
        if (FAILED(hr))
        {
            Fail("decoder D3D11", hr);
            m_manager.Reset();
        }
    }

    ComPtr<IMFMediaType> input;
    MFCreateMediaType(&input);
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, width, height);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, 60, 1);
    m_inputWidth = width;
    m_inputHeight = height;
    hr = m_decoder->SetInputType(0, input.Get(), 0);
    if (FAILED(hr))
    {
        Fail("SetInputType H.264", hr);
        return false;
    }
    // The decoder offers its output types only once it has read the stream's SPS: before the first frame this fails
    // with MF_E_NO_MORE_TYPES, and Frame sets the type when ProcessOutput asks for it (TYPE_NOT_SET / STREAM_CHANGE).
    if (!SetOutputType() && log)
        log("video: tipo d'uscita al primo fotogramma");
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (log)
        log(std::string("video: decoder H.264 di sistema, ") + (m_d3dDecoder ? "su GPU (DXVA, D3D11)" : "software")
            + (d3dAware ? "" : " (non D3D11-aware)") + (m_providesSamples ? ", campioni suoi" : ", campioni nostri"));
    return true;
}

// NV12 out, the size the stream says (also after MF_E_TRANSFORM_STREAM_CHANGE). Every type offered goes to the log
// (once per different list): the phone's decoder may offer only its own formats.
bool Video::SetOutputType()
{
    std::string offered;
    ComPtr<IMFMediaType> chosen;
    for (DWORD i = 0;; ++i)
    {
        ComPtr<IMFMediaType> type;
        if (FAILED(m_decoder->GetOutputAvailableType(0, i, &type)))
            break;
        GUID subtype = {};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        char fourcc[16];
        unsigned long d = subtype.Data1;
        if (d > 0xFFFF)
            sprintf_s(fourcc, " %c%c%c%c", static_cast<char>(d), static_cast<char>(d >> 8), static_cast<char>(d >> 16), static_cast<char>(d >> 24));
        else
            sprintf_s(fourcc, " #%lu", d);
        offered += fourcc;
        if (subtype == MFVideoFormat_NV12 && !chosen)
            chosen = type;
        // '420O': Qualcomm's opaque 4:2:0 (DXGI_FORMAT_420_OPAQUE), the only type the phone's DXVA decoder offers. Only
        // the GPU can read it, and the video processor takes it as input: the path we want anyway.
        if (d == 0x4F303234 && m_d3dDecoder && !chosen)
            chosen = type;
    }
    if (offered != m_lastOffered && log)
        log("video: tipi d'uscita offerti:" + (offered.empty() ? std::string(" nessuno") : offered));
    m_lastOffered = offered;
    if (!chosen)
    {
        // DXVA on the phone may offer nothing before the stream starts: the same decoder without the D3D manager
        // (software, CPU) is the fallback, decided once.
        if (offered.empty() && m_d3dDecoder && m_framesIn > 8)
        {
            if (log)
                log("video: nessun tipo con DXVA dopo 8 fotogrammi: provo il decoder senza GPU");
            m_decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
            m_d3dDecoder = false;
            m_manager.Reset();
            ComPtr<IMFMediaType> input;
            MFCreateMediaType(&input);
            input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
            MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, m_inputWidth, m_inputHeight);
            input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            m_decoder->SetInputType(0, input.Get(), 0);
            m_framesIn = 0;
        }
        return false;
    }
    HRESULT hr = m_decoder->SetOutputType(0, chosen.Get(), 0);
    if (FAILED(hr))
    {
        Fail("SetOutputType NV12", hr);
        return false;
    }
    UINT32 w = 0, h = 0;
    MFGetAttributeSize(chosen.Get(), MF_MT_FRAME_SIZE, &w, &h);
    UINT32 stride = 0;
    if (FAILED(chosen->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) || !stride)
        stride = w;
    m_videoWidth = w;
    m_videoHeight = h;
    m_stride = stride;
    MFT_OUTPUT_STREAM_INFO info = {};
    m_decoder->GetOutputStreamInfo(0, &info);
    m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    m_outputSize = info.cbSize;
    m_vpEnum.Reset();
    m_vp.Reset();
    m_upload.Reset();
    if (log)
    {
        char text[128];
        sprintf_s(text, "video: uscita %ux%u stride %u, %s", w, h, stride, m_providesSamples ? "campioni suoi" : "campioni nostri");
        log(text);
    }
    return true;
}

bool Video::Frame(const unsigned char* data, size_t size, long long sampleTime, Timing& timing, bool show)
{
    if (!m_decoder)
        return false;
    long long start = NowUs();
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(size), &buffer);
    if (FAILED(hr))
        return false;
    BYTE* bytes = nullptr;
    buffer->Lock(&bytes, nullptr, nullptr);
    memcpy(bytes, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));
    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(sampleTime);
    sample->SetSampleDuration(166667);
    hr = m_decoder->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING)
    {
        Fail("ProcessInput", hr);
        return false;
    }
    bool presented = false;
    ++m_framesIn;
    // First frames: the output type becomes available once the decoder has read the SPS, either here or through
    // ProcessOutput's TYPE_NOT_SET / STREAM_CHANGE below.
    if (!m_stride && !SetOutputType())
    {
        MFT_OUTPUT_STREAM_INFO info = {};
        m_decoder->GetOutputStreamInfo(0, &info);
        m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    }
    for (int round = 0; round < 8; ++round)
    {
        MFT_OUTPUT_DATA_BUFFER output = {};
        ComPtr<IMFSample> own;
        if (!m_providesSamples)
        {
            ComPtr<IMFMediaBuffer> outBuffer;
            DWORD bytes = m_outputSize ? m_outputSize : m_stride ? m_stride * (m_videoHeight + 16) * 3 / 2
                                                                 : m_inputWidth * (m_inputHeight + 16) * 3 / 2;
            MFCreateMemoryBuffer(bytes, &outBuffer);
            MFCreateSample(&own);
            own->AddBuffer(outBuffer.Get());
            output.pSample = own.Get();
        }
        DWORD status = 0;
        hr = m_decoder->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents)
            output.pEvents->Release();
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
            break;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET)
        {
            if (output.pSample && m_providesSamples)
                output.pSample->Release();
            if (!SetOutputType())
                break; // not yet known: more input first
            continue;
        }
        if (FAILED(hr))
        {
            Fail("ProcessOutput", hr);
            return false;
        }
        timing.decoded = NowUs() - start;
        IMFSample* picture = output.pSample;
        bool ok = show ? Present(picture, timing, start) : picture != nullptr;
        presented = presented || ok;
        if (m_providesSamples && picture)
            picture->Release();
    }
    return presented;
}

bool Video::Present(IMFSample* sample, Timing& timing, long long start)
{
    if (!sample)
        return false;
    ComPtr<IMFMediaBuffer> buffer;
    sample->GetBufferByIndex(0, &buffer);
    ComPtr<IMFDXGIBuffer> dxgiBuffer;
    ComPtr<ID3D11Texture2D> texture;
    UINT slice = 0;
    if (buffer && SUCCEEDED(buffer.As(&dxgiBuffer)))
    {
        dxgiBuffer->GetResource(IID_PPV_ARGS(&texture));
        dxgiBuffer->GetSubresourceIndex(&slice);
    }
    UINT index = 0;
    ComPtr<IDXGISwapChain2> swap2;
    // FLIP_SEQUENTIAL: the buffer to draw is always 0 (the runtime rotates them).
    ComPtr<ID3D11Texture2D> back;
    m_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (texture && m_videoDevice && m_videoContext)
    {
        if (!m_vp)
        {
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content = {};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputWidth = m_videoWidth;
            content.InputHeight = m_videoHeight;
            content.OutputWidth = m_width;
            content.OutputHeight = m_height;
            content.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
            HRESULT hr = m_videoDevice->CreateVideoProcessorEnumerator(&content, &m_vpEnum);
            if (SUCCEEDED(hr))
                hr = m_videoDevice->CreateVideoProcessor(m_vpEnum.Get(), 0, &m_vp);
            if (FAILED(hr))
            {
                Fail("video processor", hr);
                m_videoDevice.Reset();
                return PresentSoftware(sample);
            }
        }
        ComPtr<ID3D11VideoProcessorOutputView> outView;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc = {};
        outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        HRESULT hr = m_videoDevice->CreateVideoProcessorOutputView(back.Get(), m_vpEnum.Get(), &outDesc, &outView);
        ComPtr<ID3D11VideoProcessorInputView> inView;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc = {};
        inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        inDesc.Texture2D.ArraySlice = slice;
        if (SUCCEEDED(hr))
            hr = m_videoDevice->CreateVideoProcessorInputView(texture.Get(), m_vpEnum.Get(), &inDesc, &inView);
        if (SUCCEEDED(hr))
        {
            D3D11_VIDEO_PROCESSOR_STREAM stream = {};
            stream.Enable = TRUE;
            stream.pInputSurface = inView.Get();
            hr = m_videoContext->VideoProcessorBlt(m_vp.Get(), outView.Get(), 0, 1, &stream);
        }
        if (FAILED(hr))
        {
            Fail("VideoProcessorBlt", hr);
            return false;
        }
    }
    else if (!PresentSoftware(sample))
        return false;
    timing.converted = NowUs() - start;
    HRESULT hr = m_swap->Present(1, 0);
    timing.presented = NowUs() - start;
    (void)index;
    if (FAILED(hr))
    {
        Fail("Present", hr);
        return false;
    }
    return true;
}

// No GPU path: NV12 from system memory to BGRA on the CPU, uploaded and copied to the back buffer. Slow (the measure
// says how slow); only for a phone that refuses DXVA or the video processor.
bool Video::PresentSoftware(IMFSample* sample)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer)))
        return false;
    BYTE* nv12 = nullptr;
    DWORD length = 0;
    if (FAILED(buffer->Lock(&nv12, nullptr, &length)))
        return false;
    if (!m_upload)
    {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = m_width;
        desc.Height = m_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_upload);
        if (FAILED(hr))
        {
            buffer->Unlock();
            Fail("texture di caricamento", hr);
            return false;
        }
        if (log)
            log("video: conversione NV12 -> BGRA sulla CPU");
    }
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_context->Map(m_upload.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
    {
        buffer->Unlock();
        return false;
    }
    int w = std::min(m_videoWidth, m_width);
    int h = std::min(m_videoHeight, m_height);
    // The chroma plane starts after the luma rows, padded to a multiple of 16 rows by the decoder.
    int lumaRows = static_cast<int>(length / (m_stride * 3 / 2));
    if (lumaRows < m_videoHeight)
        lumaRows = m_videoHeight;
    const BYTE* uvPlane = nv12 + m_stride * lumaRows;
    for (int y = 0; y < h; ++y)
    {
        const BYTE* luma = nv12 + y * m_stride;
        const BYTE* uv = uvPlane + (y / 2) * m_stride;
        unsigned char* out = static_cast<unsigned char*>(mapped.pData) + y * mapped.RowPitch;
        for (int x = 0; x < w; ++x)
        {
            int c = (luma[x] - 16) * 298;
            int d = uv[x & ~1] - 128;
            int e = uv[(x & ~1) + 1] - 128;
            out[4 * x + 0] = Clamp((c + 516 * d + 128) >> 8);
            out[4 * x + 1] = Clamp((c - 100 * d - 208 * e + 128) >> 8);
            out[4 * x + 2] = Clamp((c + 409 * e + 128) >> 8);
            out[4 * x + 3] = 255;
        }
    }
    m_context->Unmap(m_upload.Get(), 0);
    buffer->Unlock();
    ComPtr<ID3D11Texture2D> back;
    m_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    m_context->CopyResource(back.Get(), m_upload.Get());
    return true;
}

void Video::Shutdown()
{
    if (m_decoder)
    {
        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    m_decoder.Reset();
}
