// Where the compositor thread draws a tab: the XAML SwapChainPanel, as an EGL window surface through ANGLE (WinRT
// ANGLE takes the panel's IInspectable* as EGLNativeWindowType and owns the DXGI swap chain). Replaces WebKit2's
// AcceleratedSurface (Igalia, BSD), which hands buffers to a UI process; the interface is the one ThreadedCompositor
// uses.
#pragma once

#include <WebCore/ColorComponents.h>
#include <WebCore/ColorModels.h>
#include <WebCore/CoordinatedCompositionReason.h>
#include <WebCore/Damage.h>
#include <WebCore/IntSize.h>
#include <atomic>
#include <unknwn.h>
#include <optional>
#include <wtf/Function.h>
#include <wtf/RunLoop.h>
#include <wtf/text/WTFString.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/WeakPtr.h>

namespace WebKit {

class WebPage;

class AcceleratedSurface final : public ThreadSafeRefCounted<AcceleratedSurface, WTF::DestructionThread::MainRunLoop> {
    WTF_MAKE_TZONE_ALLOCATED(AcceleratedSurface);
public:
    // frameCompleteHandler runs on the compositing thread once a frame has been presented.
    static Ref<AcceleratedSurface> create(WebPage&, Function<void()>&& frameCompleteHandler);
    ~AcceleratedSurface();

    using ColorComponents = WebCore::ColorComponents<float, 4>;

    // What ANGLE gets as EGLNativeWindowType: a property set with the panel and the screen scale (see .cpp).
    uint64_t window() const { return reinterpret_cast<uintptr_t>(m_nativeWindow ? m_nativeWindow : m_swapChainPanel); }
    uint64_t surfaceID() const { return m_id; }
    // ANGLE's D3D11 window surface already presents GL's bottom-left origin upright: no extra flip (true painted
    // every page upside down).
    // Not mirrored, also with ANGLE's fast present path (PlatformDisplayWin): mirrored, the page came out upside down
    // on the phone (2026-10-27).
    bool shouldPaintMirrored() const { return false; }

    // Compositing thread, with the GL context current: lets DXGI queue one frame only, so eglSwapBuffers returns when
    // the previous frame is on screen. Frames (and the scrolling trees' display refresh, which follows them) are then
    // paced by the display with one frame of latency instead of DXGI's default three.
    static void limitFrameLatency();
    // lw_gpu_test: DXGI frame latency of the shared D3D11 device, and the display's real vblank period (logged).
    static void setFrameLatency(unsigned);
    // Why D3D dropped the shared device ("dispositivo valido" when it did not).
    static String deviceRemovedReason();
    static void measureVBlank();
    // App suspension, with every compositor idle (any thread): the driver gives back the buffers it keeps to speed up
    // rendering (IDXGIDevice3::Trim, which Windows asks of every Direct3D app when it is suspended). Textures and the
    // swap chain stay, so the page is on screen at once on resume.
    static void trimGPUMemory();

    void didCreateCompositingRunLoop(RunLoop&);
    void willDestroyCompositingRunLoop();
    void willDestroyGLContext() { }

    void willRenderFrame(const WebCore::IntSize&);
    void didRenderFrame();
    void clear(const OptionSet<WebCore::CompositionReason>&);

    void visibilityDidChange(bool visible) { m_isVisible = visible; }
    void backgroundColorDidChange();

    // Damage: recorded for the visualizer; the swap chain's buffers are not tracked yet, so every frame repaints all
    // of the render target.
    void setFrameDamage(WebCore::Damage&& damage) { m_frameDamage = WTF::move(damage); }
    const std::optional<WebCore::Damage>& frameDamage() const LIFETIME_BOUND { return m_frameDamage; }
    const std::optional<WebCore::Damage>& renderTargetDamage() const LIFETIME_BOUND { return m_renderTargetDamage; }

private:
    AcceleratedSurface(WebPage&, Function<void()>&& frameCompleteHandler);

    static constexpr ColorComponents white { 1.f, 1.f, 1.f, WebCore::AlphaTraits<float>::opaque };

    void* m_swapChainPanel;
    IUnknown* m_nativeWindow { nullptr };
    Function<void()> m_frameCompleteHandler;
    uint64_t m_id;
    WebCore::IntSize m_size;
    bool m_isVisible { true };
    WeakPtr<WebPage> m_webPage;
    std::atomic<ColorComponents> m_backgroundColor { white };
    RefPtr<RunLoop> m_compositingRunLoop;
    std::optional<WebCore::Damage> m_frameDamage;
    std::optional<WebCore::Damage> m_renderTargetDamage;
};

} // namespace WebKit
