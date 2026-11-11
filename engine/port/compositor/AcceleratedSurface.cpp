#include "config.h"
#include "AcceleratedSurface.h"

#include "Engine.h"
#include "WebPage.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/PlatformDisplay.h>
#include <d3d11.h>
#include <algorithm>
#include <wtf/Threading.h>
#include <wtf/HexNumber.h>
#include <wtf/text/MakeString.h>
#include <dxgi1_3.h>
#include <mutex>
#include <roapi.h>
#include <windows.foundation.collections.h>
#include <windows.foundation.h>
#include <winstring.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(AcceleratedSurface);

static uint64_t generateID()
{
    static std::atomic<uint64_t> identifier;
    return ++identifier;
}

Ref<AcceleratedSurface> AcceleratedSurface::create(WebPage& webPage, Function<void()>&& frameCompleteHandler)
{
    return adoptRef(*new AcceleratedSurface(webPage, WTF::move(frameCompleteHandler)));
}

// WinRT ANGLE sizes its swap chain from the panel's size in view pixels unless told the scale: a property set
// {EGLNativeWindowTypeProperty: panel, EGLRenderResolutionScaleProperty: scale} (angle_windowsstore.h) gives a
// swap chain in physical pixels, shown 1:1 by the panel. Made on the Web thread (WinRT initialized there).
static IUnknown* createNativeWindow(void* panel, float scale)
{
    using namespace ABI::Windows::Foundation;
    auto string = [](const wchar_t* text, HSTRING_HEADER& header) {
        HSTRING result = nullptr;
        WindowsCreateStringReference(text, static_cast<UINT32>(wcslen(text)), &header, &result);
        return result;
    };
    HSTRING_HEADER classHeader, valueHeader, panelKeyHeader, scaleKeyHeader;
    IInspectable* propertySet = nullptr;
    if (FAILED(RoActivateInstance(string(RuntimeClass_Windows_Foundation_Collections_PropertySet, classHeader), &propertySet)))
        return nullptr;
    __FIMap_2_HSTRING_IInspectable* map = nullptr;
    IPropertyValueStatics* values = nullptr;
    IInspectable* scaleValue = nullptr;
    boolean replaced = false;
    bool ok = SUCCEEDED(propertySet->QueryInterface(__uuidof(__FIMap_2_HSTRING_IInspectable), reinterpret_cast<void**>(&map)))
        && SUCCEEDED(RoGetActivationFactory(string(RuntimeClass_Windows_Foundation_PropertyValue, valueHeader), __uuidof(IPropertyValueStatics), reinterpret_cast<void**>(&values)))
        && SUCCEEDED(values->CreateSingle(scale, &scaleValue))
        && SUCCEEDED(map->Insert(string(L"EGLNativeWindowTypeProperty", panelKeyHeader), static_cast<IInspectable*>(panel), &replaced))
        && SUCCEEDED(map->Insert(string(L"EGLRenderResolutionScaleProperty", scaleKeyHeader), scaleValue, &replaced));
    if (scaleValue)
        scaleValue->Release();
    if (values)
        values->Release();
    if (map)
        map->Release();
    if (!ok) {
        propertySet->Release();
        return nullptr;
    }
    return propertySet;
}

AcceleratedSurface::AcceleratedSurface(WebPage& webPage, Function<void()>&& frameCompleteHandler)
    : m_swapChainPanel(webPage.swapChainPanel())
    , m_frameCompleteHandler(WTF::move(frameCompleteHandler))
    , m_id(generateID())
    , m_webPage(webPage)
{
    m_nativeWindow = createNativeWindow(m_swapChainPanel, webPage.deviceScaleFactor());
    Lumia::log("AcceleratedSurface: finestra ANGLE %s, scala %.2f", m_nativeWindow ? "con scala" : "SENZA scala (errore WinRT)", webPage.deviceScaleFactor());
    backgroundColorDidChange();
}

AcceleratedSurface::~AcceleratedSurface()
{
    if (m_nativeWindow)
        m_nativeWindow->Release();
}

static IDXGIDevice1* sharedDXGIDevice()
{
    EGLAttrib device = 0;
    EGLAttrib d3dDevice = 0;
    if (!eglQueryDisplayAttribEXT(PlatformDisplay::sharedDisplay().eglDisplay(), EGL_DEVICE_EXT, &device) || !device
        || !eglQueryDeviceAttribEXT(reinterpret_cast<EGLDeviceEXT>(device), EGL_D3D11_DEVICE_ANGLE, &d3dDevice) || !d3dDevice) {
        WTFLogAlways("AcceleratedSurface: no D3D11 device from ANGLE");
        return nullptr;
    }
    IDXGIDevice1* dxgiDevice = nullptr;
    if (FAILED(reinterpret_cast<ID3D11Device*>(d3dDevice)->QueryInterface(__uuidof(IDXGIDevice1), reinterpret_cast<void**>(&dxgiDevice)))) {
        WTFLogAlways("AcceleratedSurface: no IDXGIDevice1");
        return nullptr;
    }
    return dxgiDevice;
}

// The D3D11 device is shared by every tab (one EGL display), so this runs once per process.
void AcceleratedSurface::limitFrameLatency()
{
    static std::once_flag once;
    std::call_once(once, [] { setFrameLatency(1); });
}

String AcceleratedSurface::deviceRemovedReason()
{
    EGLAttrib device = 0;
    EGLAttrib d3dDevice = 0;
    if (!eglQueryDisplayAttribEXT(PlatformDisplay::sharedDisplay().eglDisplay(), EGL_DEVICE_EXT, &device) || !device
        || !eglQueryDeviceAttribEXT(reinterpret_cast<EGLDeviceEXT>(device), EGL_D3D11_DEVICE_ANGLE, &d3dDevice) || !d3dDevice)
        return "dispositivo D3D non disponibile"_s;
    HRESULT reason = reinterpret_cast<ID3D11Device*>(d3dDevice)->GetDeviceRemovedReason();
    if (reason == S_OK)
        return "dispositivo valido"_s;
    return makeString("dispositivo rimosso, motivo 0x"_s, hex(static_cast<uint32_t>(reason)));
}

void AcceleratedSurface::setFrameLatency(unsigned frames)
{
    if (auto* dxgiDevice = sharedDXGIDevice()) {
        dxgiDevice->SetMaximumFrameLatency(frames);
        dxgiDevice->Release();
    }
}

// ANGLE's own trim on suspension (EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE) runs on the UI thread at the Suspending
// event, while a compositor may still be drawing; here it comes after the compositors have stopped. No ClearState:
// ANGLE caches the pipeline bindings and would not know they were gone.
void AcceleratedSurface::trimGPUMemory()
{
    auto* dxgiDevice = sharedDXGIDevice();
    if (!dxgiDevice)
        return;
    IDXGIDevice3* device = nullptr;
    if (SUCCEEDED(dxgiDevice->QueryInterface(__uuidof(IDXGIDevice3), reinterpret_cast<void**>(&device)))) {
        device->Trim();
        device->Release();
    } else
        Lumia::log("gpu: IDXGIDevice3 assente, memoria del driver non rilasciata");
    dxgiDevice->Release();
}

// Times 60 vblanks of the first output on a thread of its own: whether the panel really refreshes at 60 Hz.
void AcceleratedSurface::measureVBlank()
{
    Thread::create("VBlank"_s, [] {
        auto* dxgiDevice = sharedDXGIDevice();
        if (!dxgiDevice)
            return;
        IDXGIAdapter* adapter = nullptr;
        IDXGIOutput* output = nullptr;
        if (FAILED(dxgiDevice->GetAdapter(&adapter)) || FAILED(adapter->EnumOutputs(0, &output))) {
            Lumia::log("gpu prova: nessuna uscita DXGI, vblank non misurabile");
            if (adapter)
                adapter->Release();
            dxgiDevice->Release();
            return;
        }
        Vector<double> periods;
        auto previous = MonotonicTime::now();
        HRESULT result = S_OK;
        for (unsigned i = 0; i < 61 && SUCCEEDED(result); ++i) {
            result = output->WaitForVBlank();
            auto now = MonotonicTime::now();
            if (i)
                periods.append((now - previous).milliseconds());
            previous = now;
        }
        if (periods.isEmpty())
            Lumia::log("gpu prova: WaitForVBlank fallita (0x%08X)", static_cast<unsigned>(result));
        else {
            std::sort(periods.begin(), periods.end());
            Lumia::log("gpu prova: vblank mediana %.2f ms (min %.2f max %.2f) su %u", periods[periods.size() / 2], periods.first(), periods.last(), periods.size());
        }
        output->Release();
        adapter->Release();
        dxgiDevice->Release();
    })->detach();
}

void AcceleratedSurface::didCreateCompositingRunLoop(RunLoop& runLoop)
{
    m_compositingRunLoop = &runLoop;
}

void AcceleratedSurface::willDestroyCompositingRunLoop()
{
    m_compositingRunLoop = nullptr;
}

// Main thread (the Web thread owns WTF's main thread).
void AcceleratedSurface::backgroundColorDidChange()
{
    ASSERT(RunLoop::isMain());
    // What the page actually paints wins over the UI's default (white). This is the colour glClear() leaves where no
    // tile has been painted yet: with the page's own colour a tile still being rasterized shows the page background
    // instead of a white flash.
    std::optional<WebCore::Color> color;
    if (m_webPage) {
        if (auto* view = m_webPage->localMainFrameView()) {
            auto documentColor = view->documentBackgroundColor();
            if (documentColor.isValid())
                color = documentColor;
        }
        if (!color)
            color = m_webPage->backgroundColor();
    }
    m_backgroundColor = color ? color->toResolvedColorComponentsInColorSpace(WebCore::ColorSpace::SRGB) : white;
}

// The swap chain follows the panel's size by itself (ANGLE listens to SizeChanged); the viewport follows the scene.
void AcceleratedSurface::willRenderFrame(const IntSize& size)
{
    if (size != m_size) {
        // Bring-up diagnostics (phase 2): the swap chain's real size next to the scene's.
        EGLint surfaceWidth = 0, surfaceHeight = 0;
        EGLDisplay display = eglGetCurrentDisplay();
        EGLSurface surface = eglGetCurrentSurface(EGL_DRAW);
        eglQuerySurface(display, surface, EGL_WIDTH, &surfaceWidth);
        eglQuerySurface(display, surface, EGL_HEIGHT, &surfaceHeight);
        Lumia::log("AcceleratedSurface: scena %dx%d, superficie EGL %dx%d", size.width(), size.height(), surfaceWidth, surfaceHeight);
    }
    m_size = size;
    glViewport(0, 0, size.width(), size.height());
}

// eglSwapBuffers has presented the frame (it waits for the display when the swap chain is full): the next one can
// start. Dispatched, so the compositor sees the completion after renderLayerTree returns, as with a real backend.
void AcceleratedSurface::didRenderFrame()
{
    if (!m_compositingRunLoop)
        return;
    m_compositingRunLoop->dispatch([this, protectedThis = Ref { *this }] {
        if (m_compositingRunLoop)
            m_frameCompleteHandler();
    });
}

void AcceleratedSurface::clear(const OptionSet<CompositionReason>&)
{
    auto [r, g, b, a] = m_backgroundColor.load();
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

} // namespace WebKit
