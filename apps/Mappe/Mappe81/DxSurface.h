// DxSurface: D3D11 (feature level 9_x) + Direct2D 1.1 device context + composition swap chain for the
// SwapChainPanel, in physical pixels. Navit draws on it from its own thread (graphics_d2d in Navit81).
#pragma once

#include <string>

class DxSurface {
public:
    bool Create(Windows::UI::Xaml::Controls::SwapChainPanel^ panel, int width, int height, float scale,
                std::wstring& error);
    ID2D1DeviceContext* Context() const { return _ctx.Get(); }
    IDXGISwapChain1* SwapChain() const { return _swap.Get(); }
    IDWriteFactory* DWrite() const { return _dwrite.Get(); }
    IWICImagingFactory* Wic() const { return _wic.Get(); }
    std::wstring Describe() const { return _describe; }

private:
    Microsoft::WRL::ComPtr<ID3D11Device> _d3d;
    Microsoft::WRL::ComPtr<ID2D1Factory1> _factory;
    Microsoft::WRL::ComPtr<ID2D1Device> _device;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> _ctx;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> _swap;
    Microsoft::WRL::ComPtr<IDWriteFactory> _dwrite;
    Microsoft::WRL::ComPtr<IWICImagingFactory> _wic;
    std::wstring _describe;
};
