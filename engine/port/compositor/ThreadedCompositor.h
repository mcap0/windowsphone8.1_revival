/*
 * Copyright (C) 2014 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if USE(COORDINATED_GRAPHICS)
#include "CompositingRunLoop.h"
#include "FrameStats.h"
#include <WebCore/Damage.h>
#include <WebCore/DisplayUpdate.h>
#include <WebCore/GLContext.h>
#include <WebCore/IntSize.h>
#include <WebCore/TextureMapperDamageVisualizer.h>
#include <atomic>
#include <optional>
#include <wtf/Atomics.h>
#include <wtf/CheckedPtr.h>
#include <WebCore/FloatRect.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/ThreadSafeRefCounted.h>

#if !HAVE(DISPLAY_LINK)
#include "ThreadedDisplayRefreshMonitor.h"
#endif

namespace WebCore {
class CoordinatedPlatformLayer;
class TextureMapper;
class TransformationMatrix;
}

namespace WebKit {
class AcceleratedSurface;
class CoordinatedSceneState;
class LayerTreeHost;

class ThreadedCompositor : public ThreadSafeRefCounted<ThreadedCompositor>, public CanMakeThreadSafeCheckedPtr<ThreadedCompositor> {
    WTF_MAKE_TZONE_ALLOCATED(ThreadedCompositor);
    WTF_MAKE_NONCOPYABLE(ThreadedCompositor);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(ThreadedCompositor);
public:
#if HAVE(DISPLAY_LINK)
    static Ref<ThreadedCompositor> create(LayerTreeHost&);
#else
    static Ref<ThreadedCompositor> create(LayerTreeHost&, ThreadedDisplayRefreshMonitor::Client&, WebCore::PlatformDisplayID);
#endif
    virtual ~ThreadedCompositor();

    uint64_t surfaceID() const;

    // Tap highlight (phase 3): a square translucent box on the link, button or field under the finger, drawn by the
    // compositor from the interaction regions of its own layers, so it shows while the Web thread is busy. Any thread.
    // Press: shown 60 ms later if the finger has neither lifted nor started a pan (a pan start never flashes).
    static void tapHighlightPress(const WebCore::FloatPoint& viewPoint);
    // Tap (finger lifted without panning): shown at once, until the click is done and for at least 100 ms.
    static void tapHighlightTap(const WebCore::FloatPoint& viewPoint);
    static void tapHighlightClickDone();
    // A pan started or the gesture was cancelled.
    static void tapHighlightClear();
    // Pinch zoom (phase 3). During the gesture the compositor scales the last committed scene itself, around the
    // fingers, every frame, without the Web thread; at the end the page takes the new page scale and the compositor
    // drops its own scale on the very frame whose scene has it (sceneCommitLock), so nothing jumps.
    struct PinchResult {
        bool valid { false };
        float scale { 1 };
        WebCore::FloatPoint startCenter;
        WebCore::FloatPoint center;
        unsigned generation { 0 };
    };
    // Input thread. scale = finger distance / distance at BEGIN; center in view pixels.
    static void pinchBegin(const WebCore::FloatPoint& center);
    static void pinchUpdate(float scale, const WebCore::FloatPoint& center);
    // The gesture as the screen shows it, for the Web thread to apply; the compositor keeps showing it meanwhile.
    static PinchResult pinchEnd();
    static void pinchCancel();
    // The visible page's scale and its viewport's limits (WebPage::updateCompositorZoom, Web thread): the gesture is
    // clamped to them, and a double tap goes back to the initial scale (the page's fit).
    struct ZoomLimits {
        float pageScale { 1 };
        float minimumScale { 1 };
        float maximumScale { 5 };
        float initialScale { 1 };
    };
    static void setZoomLimits(const ZoomLimits&);
    // Web thread, inside the rendering update that commits the new scale, holding sceneCommitLock().
    static void pinchCommitted(unsigned generation);
    static Lock& sceneCommitLock();
    // Double tap: the same transform animated by the compositor from 1 to `scale` around `point` (which stays
    // put), then onEnd (compositor thread) hands the result to the Web thread exactly as a pinch's end. False while
    // another pinch or zoom is under way.
    static bool zoomAnimate(const WebCore::FloatPoint& point, float scale, Function<void(const PinchResult&)>&& onEnd);
    // As last set by setZoomLimits.
    static ZoomLimits zoomLimits();
    // A pinch or double-tap zoom is on screen or being applied: the page scale is the finger's until it lands.
    static bool zoomGestureActive();
    // Whether the last tap highlight's hit test found a link, button or field.
    static bool tapHighlightHadTarget();

    static void registerTapHighlightCompositor(ThreadedCompositor*, ThreadedCompositor* leaving = nullptr);
    void scheduleTapHighlightUpdate();
    RunLoop& tapHighlightRunLoop();
    int maxTextureSize() const { return m_maxTextureSize; }

    void backgroundColorDidChange();

    void setSize(const WebCore::IntSize&, float);
    uint32_t requestComposition();
    void scheduleUpdate();
    // The scrolling thread cannot see the display refresh: a frame it asks for is started by the compositor's frame
    // clock, so scroll frames land on refresh boundaries instead of in the middle of one (vsync pacing). Any thread;
    // wakes the clock when it is stopped.
    void requestCompositionAtNextDisplayRefresh();
    RunLoop* runLoop();

    void invalidate();

#if !HAVE(DISPLAY_LINK)
    WebCore::DisplayRefreshMonitor& displayRefreshMonitor() const;
#endif

    void suspend();
    void resume();

    bool isActive() const;
    String displayRefreshState() const; // for the log: LayerTreeHost::renderingWatchdogFired

    std::optional<float> fps() const { return m_fpsCounter.fps.load(); };

#if ENABLE(DAMAGE_TRACKING)
    enum class DamagePropagationFlags : uint8_t {
        Unified = 1 << 0,
        UseForCompositing = 1 << 1
    };
    void setDamagePropagationSettings(std::optional<OptionSet<DamagePropagationFlags>>, unsigned rectangleThreshold);
    void enableFrameDamageNotificationForTesting();
#endif

private:
#if HAVE(DISPLAY_LINK)
    explicit ThreadedCompositor(LayerTreeHost&);
#else
    ThreadedCompositor(LayerTreeHost&, ThreadedDisplayRefreshMonitor::Client&, WebCore::PlatformDisplayID);
#endif

    void updateSceneState();
    void renderLayerTree();
    // Compositor thread, each frame: whether the highlight is drawn this frame (hit testing it on the first one).
    bool updateTapHighlight(const WebCore::TransformationMatrix&);
    // Compositor thread, after the scene update: the pinch's view transform (identity when none) and whether this
    // frame or the previous one used it.
    WebCore::TransformationMatrix pinchTransform(bool& changed);
    bool m_pinchWasActive { false };
    void drawTapHighlight();
    RefPtr<WebCore::CoordinatedPlatformLayer> m_tapHighlightLayer;
    WebCore::FloatRect m_tapHighlightRect;
    unsigned m_tapHighlightGeneration { 0 };
    bool m_tapHighlightHitTested { false };
    bool m_tapHighlightVisible { false };
    bool m_tapHighlightWasVisible { false };
    bool m_dumpedDraws { false }; // timeline: draws of one frame logged (paintToCurrentGLContext)
    int m_lastSwapError { 0x3000 }; // EGL_SUCCESS; the last swap error logged
#if !HAVE(DISPLAY_LINK)
    void startRequestedFrame();
#endif
    void paintToCurrentGLContext(const WebCore::TransformationMatrix&, const WebCore::IntSize&);
    void frameComplete();

#if HAVE(DISPLAY_LINK)
    void didRenderFrameTimerFired();
#else
    void displayUpdateFired();
    void sceneUpdateFinished();
    void startFrameClock();
    void wakeFrameClock();
#endif

    void updateSceneAttributes(const WebCore::IntSize&, float deviceScaleFactor);

    void initializeFPSCounter();
    void updateFPSCounter();

    CheckedPtr<LayerTreeHost> m_layerTreeHost;
    RefPtr<AcceleratedSurface> m_surface;
    RefPtr<CoordinatedSceneState> m_sceneState;
    std::unique_ptr<WebCore::GLContext> m_context;

    bool m_flipY { false };
    int m_maxTextureSize { 0 };
    std::atomic<unsigned> m_suspendedCount { 0 };

    std::unique_ptr<CompositingRunLoop> m_compositingRunLoop;

    struct {
        Lock lock;
        WebCore::IntSize viewportSize;
        float deviceScaleFactor { 1 };

#if !HAVE(DISPLAY_LINK)
        bool clientRendersNextFrame { false };
#endif
    } m_attributes;

    std::unique_ptr<WebCore::TextureMapper> m_textureMapper;

    struct {
        bool exposesFPS { false };
        Seconds calculationInterval { 1_s };
        MonotonicTime lastCalculationTimestamp;
        unsigned frameCountSinceLastCalculation { 0 };
        std::atomic<std::optional<float>> fps;
    } m_fpsCounter;

#if ENABLE(DAMAGE_TRACKING)
    struct {
        std::optional<OptionSet<DamagePropagationFlags>> flags;
        std::unique_ptr<WebCore::TextureMapperDamageVisualizer> visualizer;
        std::atomic<bool> shouldNotifyFrameDamageForTesting { false };
    } m_damage;
#endif

    std::atomic<uint32_t> m_compositionRequestID { 0 };
    std::atomic<bool> m_compositionRequestedForNextDisplayRefresh { false };
    FrameStats m_frameStats;
#if HAVE(DISPLAY_LINK)
    std::atomic<uint32_t> m_compositionResponseID { 0 };
    RunLoop::Timer m_didRenderFrameTimer;
#else
    struct {
        WebCore::PlatformDisplayID displayID;
        WebCore::DisplayUpdate displayUpdate;
        std::unique_ptr<RunLoop::Timer> updateTimer;
        // The frame clock (see displayUpdateFired): compositing thread, except `running` and `wakeQueued`, which the
        // threads that wake it read.
        std::atomic<bool> running { false };
        std::atomic<bool> wakeQueued { false };
        std::atomic<bool> frameDispatchQueued { false }; // requestCompositionAtNextDisplayRefresh -> startRequestedFrame
        unsigned idleTicks { 0 };
        bool tickAfterFrame { false };
    } m_display;

    const Ref<ThreadedDisplayRefreshMonitor> m_displayRefreshMonitor;
    uint32_t m_renderedCompositionRequestID { 0 }; // compositing thread: last client commit shown on screen

#endif
};

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)

