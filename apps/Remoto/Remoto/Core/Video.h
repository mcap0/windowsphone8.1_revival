#pragma once

// Remoto's video path (PLAN §64.2): an H.264 access unit from the network -> Media Foundation decoder (DXVA on the
// phone's GPU when it takes a D3D11 device) -> NV12 texture -> D3D11 video processor (NV12 to BGRA on the GPU) -> the
// swap chain of the SwapChainPanel. Everything runs on the caller's thread (the network thread): no queue between
// receiving a frame and presenting it. Software fallbacks are kept, and logged, for each step the phone refuses.

#include <functional>
#include <string>
#include <wrl/client.h>
#include <d3d11_1.h>
#include <dxgi1_3.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

class Video
{
public:
    // Log lines (any thread).
    std::function<void(const std::string&)> log;

    // UI thread: device, swap chain on the panel (physical pixels; scale = panel's CompositionScaleX).
    bool CreateDevice(Windows::UI::Xaml::Controls::SwapChainPanel^ panel, int width, int height, float scale);
    // Network thread: the decoder for frames of this size.
    bool CreateDecoder(int width, int height);
    // Network thread: one access unit (Annex-B). Returns true when a picture came out of the decoder (and was
    // presented, if show); the times are QPC microseconds after the call's start (0 when that step did not happen).
    // show = false: decoded only (a newer frame is already here: presenting this one would only wait a vsync).
    struct Timing { long long decoded = 0, converted = 0, presented = 0; };
    bool Frame(const unsigned char* data, size_t size, long long sampleTime, Timing& timing, bool show = true);
    void Shutdown();

    std::string Describe() const { return m_describe; }

private:
    bool SetOutputType();
    bool Present(IMFSample* sample, Timing& timing, long long start);
    bool PresentSoftware(IMFSample* sample);
    void Fail(const char* what, HRESULT hr);

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swap;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_manager;
    Microsoft::WRL::ComPtr<IMFTransform> m_decoder;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDevice;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoContext;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_vp;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_upload; // software path: BGRA, CPU-written, copied to the back buffer
    int m_width = 0, m_height = 0;         // swap chain
    int m_videoWidth = 0, m_videoHeight = 0, m_stride = 0;
    bool m_d3dDecoder = false;            // the decoder writes D3D11 textures
    bool m_providesSamples = false;
    DWORD m_outputSize = 0;
    std::string m_describe;
    std::string m_lastFailure;
    std::string m_lastOffered;
    int m_inputWidth = 0, m_inputHeight = 0;
    int m_framesIn = 0;
};
