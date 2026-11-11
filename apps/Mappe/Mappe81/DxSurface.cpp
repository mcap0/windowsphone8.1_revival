#include "pch.h"
#include "DxSurface.h"

using Microsoft::WRL::ComPtr;

namespace {
std::wstring Hr(const wchar_t* what, HRESULT hr) {
    wchar_t b[96];
    swprintf_s(b, L"%s 0x%08X", what, static_cast<unsigned>(hr));
    return b;
}
}

bool DxSurface::Create(Windows::UI::Xaml::Controls::SwapChainPanel^ panel, int width, int height, float scale,
                       std::wstring& error) {
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1};
    D3D_FEATURE_LEVEL got;
    ComPtr<ID3D11DeviceContext> immediate;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &_d3d, &got, &immediate);
    if (FAILED(hr)) { error = Hr(L"D3D11CreateDevice", hr); return false; }

    D2D1_FACTORY_OPTIONS options = {};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_MULTI_THREADED, __uuidof(ID2D1Factory1), &options,
                           reinterpret_cast<void**>(_factory.GetAddressOf()));
    if (FAILED(hr)) { error = Hr(L"D2D1CreateFactory", hr); return false; }
    ComPtr<IDXGIDevice> dxgi;
    _d3d.As(&dxgi);
    hr = _factory->CreateDevice(dxgi.Get(), &_device);
    if (FAILED(hr)) { error = Hr(L"CreateDevice(D2D)", hr); return false; }
    hr = _device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &_ctx);
    if (FAILED(hr)) { error = Hr(L"CreateDeviceContext", hr); return false; }
    _ctx->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    ComPtr<IDXGIAdapter> adapter;
    dxgi->GetAdapter(&adapter);
    ComPtr<IDXGIFactory2> dxgiFactory;
    adapter->GetParent(IID_PPV_ARGS(&dxgiFactory));
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
    hr = dxgiFactory->CreateSwapChainForComposition(_d3d.Get(), &desc, nullptr, &_swap);
    if (FAILED(hr)) { error = Hr(L"CreateSwapChainForComposition", hr); return false; }

    // The panel is measured in view pixels: map the physical-pixel swap chain onto it 1:1.
    ComPtr<IDXGISwapChain2> swap2;
    if (SUCCEEDED(_swap.As(&swap2)) && scale > 0) {
        DXGI_MATRIX_3X2_F m = {1.0f / scale, 0, 0, 1.0f / scale, 0, 0};
        swap2->SetMatrixTransform(&m);
    }
    ComPtr<ISwapChainPanelNative> native;
    hr = reinterpret_cast<IUnknown*>(panel)->QueryInterface(IID_PPV_ARGS(&native));
    if (SUCCEEDED(hr)) hr = native->SetSwapChain(_swap.Get());
    if (FAILED(hr)) { error = Hr(L"SetSwapChain", hr); return false; }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(_dwrite.GetAddressOf()));
    if (FAILED(hr)) { error = Hr(L"DWriteCreateFactory", hr); return false; }
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&_wic));
    if (FAILED(hr)) { error = Hr(L"WIC", hr); return false; }

    wchar_t b[128];
    swprintf_s(b, L"D3D FL %x, swap chain %dx%d, scala %.2f", static_cast<unsigned>(got), width, height, scale);
    _describe = b;
    return true;
}
