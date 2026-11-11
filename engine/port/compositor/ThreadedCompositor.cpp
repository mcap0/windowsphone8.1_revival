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

#include "config.h"
#include "ThreadedCompositor.h"

#if USE(COORDINATED_GRAPHICS)
#include "AcceleratedSurface.h"
#include "CompositingRunLoop.h"
#include "CoordinatedSceneState.h"
#include "ScrollingTreeDispatcher.h"
#include "Engine.h"
#include "LayerTreeHost.h"
#include "WebPage.h"
#include <WebCore/BitmapTexturePool.h>
#include <WebCore/CoordinatedBackingStore.h>
#include <WebCore/EventRegion.h>
#include <WebCore/InteractionRegion.h>
#include <WebCore/CoordinatedPlatformLayer.h>
#include <WebCore/Damage.h>
#include <WebCore/PlatformDisplay.h>
#include <WebCore/TextureMapperLayer.h>
#include <WebCore/TransformationMatrix.h>
#include <EGL/egl.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SetForScope.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>

#if !HAVE(DISPLAY_LINK)
#include "ThreadedDisplayRefreshMonitor.h"
#endif

#if USE(LIBEPOXY)
#include <epoxy/gl.h>
#else
#include <GLES2/gl2.h>
#endif

namespace WebCore {
// texmap/TextureMapper.cpp: per-frame GL counters (compositor thread).
extern unsigned lumiaDrawCalls;
extern unsigned lumiaUploads;
extern uint64_t lumiaUploadBytes;
extern double lumiaUploadMs;
extern bool lumiaDumpDraws;
}

namespace WebKit {
using namespace WebCore;

#if !HAVE(DISPLAY_LINK)
static constexpr unsigned c_defaultRefreshRate = 60000;
#endif

WTF_MAKE_TZONE_ALLOCATED_IMPL(ThreadedCompositor);

#if HAVE(DISPLAY_LINK)
Ref<ThreadedCompositor> ThreadedCompositor::create(LayerTreeHost& layerTreeHost)
{
    return adoptRef(*new ThreadedCompositor(layerTreeHost));
}
#else
Ref<ThreadedCompositor> ThreadedCompositor::create(LayerTreeHost& layerTreeHost, ThreadedDisplayRefreshMonitor::Client& displayRefreshMonitorClient, PlatformDisplayID displayID)
{
    return adoptRef(*new ThreadedCompositor(layerTreeHost, displayRefreshMonitorClient, displayID));
}
#endif

#if HAVE(DISPLAY_LINK)
ThreadedCompositor::ThreadedCompositor(LayerTreeHost& layerTreeHost)
#else
ThreadedCompositor::ThreadedCompositor(LayerTreeHost& layerTreeHost, ThreadedDisplayRefreshMonitor::Client& displayRefreshMonitorClient, PlatformDisplayID displayID)
#endif
    : m_layerTreeHost(&layerTreeHost)
    , m_surface(AcceleratedSurface::create(layerTreeHost.webPage(), [this] { frameComplete(); }))
    , m_sceneState(&m_layerTreeHost->sceneState())
    , m_flipY(m_surface->shouldPaintMirrored())
    , m_compositingRunLoop(makeUnique<CompositingRunLoop>([this] { renderLayerTree(); }))
#if HAVE(DISPLAY_LINK)
    , m_didRenderFrameTimer(RunLoop::mainSingleton(), "ThreadedCompositor::DidRenderFrameTimer"_s, this, &ThreadedCompositor::didRenderFrameTimerFired)
#else
    , m_displayRefreshMonitor(ThreadedDisplayRefreshMonitor::create(displayID, displayRefreshMonitorClient, WebCore::DisplayUpdate { 0, c_defaultRefreshRate / 1000 }))
#endif
{
    ASSERT(RunLoop::isMain());

    initializeFPSCounter();
#if ENABLE(DAMAGE_TRACKING)
    m_damage.visualizer = TextureMapperDamageVisualizer::create();
#endif

    const auto& webPage = m_layerTreeHost->webPage();
    updateSceneAttributes(webPage.size(), webPage.deviceScaleFactor());

    m_surface->didCreateCompositingRunLoop(m_compositingRunLoop->runLoop());
    registerTapHighlightCompositor(this);

#if !HAVE(DISPLAY_LINK)
    m_display.displayID = displayID;
    m_display.displayUpdate = { 0, c_defaultRefreshRate / 1000 };
#endif

    m_compositingRunLoop->performTaskSync([this, protectedThis = Ref { *this }] {
#if !HAVE(DISPLAY_LINK)
        m_display.updateTimer = makeUnique<RunLoop::Timer>(RunLoop::currentSingleton(), "ThreadedCompositor::UpdateTimer"_s, this, &ThreadedCompositor::displayUpdateFired);
        startFrameClock();
#endif

        // GLNativeWindowType depends on the EGL implementation: reinterpret_cast works
        // for pointers (only if they are 64-bit wide and not for other cases), and static_cast for
        // numeric types (and when needed they get extended to 64-bit) but not for pointers. Using
        // a plain C cast expression in this one instance works in all cases.
        static_assert(sizeof(GLNativeWindowType) <= sizeof(uint64_t), "GLNativeWindowType must not be longer than 64 bits.");
        auto nativeSurfaceHandle = (GLNativeWindowType)m_surface->window();
        m_context = GLContext::create(PlatformDisplay::sharedDisplay(), nativeSurfaceHandle);
        if (m_context && m_context->makeContextCurrent()) {
            AcceleratedSurface::limitFrameLatency();
            if (!nativeSurfaceHandle)
                m_flipY = !m_flipY;
            glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m_maxTextureSize);
            Lumia::log("ThreadedCompositor: contesto GL ok, finestra %d, texture max %d, GL %s / %s", !!nativeSurfaceHandle,
                m_maxTextureSize, glGetString(GL_VERSION), glGetString(GL_RENDERER));
            Lumia::probeSharedVideoTextures();
        } else
            Lumia::log("ThreadedCompositor: contesto GL NON creato (%s)", GLContext::lastErrorString());
    });
}

ThreadedCompositor::~ThreadedCompositor() = default;

uint64_t ThreadedCompositor::surfaceID() const
{
    ASSERT(RunLoop::isMain());
    return m_surface->surfaceID();
}

void ThreadedCompositor::invalidate()
{
    ASSERT(RunLoop::isMain());
    registerTapHighlightCompositor(nullptr, this);
    m_compositingRunLoop->stopUpdates();
#if HAVE(DISPLAY_LINK)
    m_didRenderFrameTimer.stop();
#else
    m_displayRefreshMonitor->invalidate();
#endif
    m_compositingRunLoop->performTaskSync([this, protectedThis = Ref { *this }] {
        if (!m_context || !m_context->makeContextCurrent())
            return;

        // Update the scene at this point ensures the layers state are correctly propagated.
        updateSceneState();

        m_sceneState->invalidateCommittedLayers();
        m_textureMapper = nullptr;
        m_surface->willDestroyGLContext();
        m_context = nullptr;

#if !HAVE(DISPLAY_LINK)
        m_display.running = false;
        m_display.updateTimer = nullptr;
#endif
    });
    m_sceneState = nullptr;
    m_layerTreeHost = nullptr;
    m_surface->willDestroyCompositingRunLoop();
    m_compositingRunLoop = nullptr;
    m_surface = nullptr;
}

void ThreadedCompositor::suspend()
{
    ASSERT(RunLoop::isMain());
    m_surface->visibilityDidChange(false);

    if (++m_suspendedCount > 1)
        return;

    m_compositingRunLoop->suspend();
#if !HAVE(DISPLAY_LINK)
    m_compositingRunLoop->performTask([this, protectedThis = Ref { *this }] {
        m_display.running = false;
        if (m_display.updateTimer)
            m_display.updateTimer->stop();
    });
#endif
}

void ThreadedCompositor::resume()
{
    ASSERT(RunLoop::isMain());
    m_surface->visibilityDidChange(true);

    ASSERT(m_suspendedCount > 0);
    if (--m_suspendedCount > 0)
        return;

    m_compositingRunLoop->resume();
#if !HAVE(DISPLAY_LINK)
    m_compositingRunLoop->performTask([this, protectedThis = Ref { *this }] {
        startFrameClock();
    });
#endif
}

String ThreadedCompositor::displayRefreshState() const
{
#if !HAVE(DISPLAY_LINK)
    return m_displayRefreshMonitor->stateForLog();
#else
    return { };
#endif
}

bool ThreadedCompositor::isActive() const
{
    return m_compositingRunLoop->isActive();
}

void ThreadedCompositor::backgroundColorDidChange()
{
    ASSERT(RunLoop::isMain());
    m_surface->backgroundColorDidChange();
}

void ThreadedCompositor::setSize(const IntSize& size, float deviceScaleFactor)
{
    ASSERT(RunLoop::isMain());
    Locker locker { m_attributes.lock };
    updateSceneAttributes(size, deviceScaleFactor);
}

#if ENABLE(DAMAGE_TRACKING)
void ThreadedCompositor::setDamagePropagationSettings(std::optional<OptionSet<DamagePropagationFlags>> flags, unsigned /* rectangleThreshold */)
{
    m_damage.flags = flags;
    if (m_damage.visualizer && m_damage.flags) {
        // We don't use damage when rendering layers if the visualizer is enabled, because we need to make sure the whole
        // frame is invalidated in the next paint so that previous damage rects are cleared.
        m_damage.flags->remove(DamagePropagationFlags::UseForCompositing);
    }
}

void ThreadedCompositor::enableFrameDamageNotificationForTesting()
{
    m_damage.shouldNotifyFrameDamageForTesting = true;
}
#endif

void ThreadedCompositor::updateSceneState()
{
    if (!m_textureMapper)
        m_textureMapper = TextureMapper::create();

    auto reasons = OptionSet<CompositionReason>::all();
    m_sceneState->flushCompositingState(reasons, false);
}

namespace {

// The tap highlight's state, written by the input and Web threads, read by the compositor thread. One view per app,
// so one compositor; it registers in its constructor and leaves in invalidate().
struct TapHighlight {
    enum class Phase : uint8_t { None, Pressed, Tapped };
    Lock lock;
    ThreadedCompositor* compositor WTF_GUARDED_BY_LOCK(lock) { nullptr };
    Phase phase WTF_GUARDED_BY_LOCK(lock) { Phase::None };
    FloatPoint point WTF_GUARDED_BY_LOCK(lock);
    MonotonicTime pressTime WTF_GUARDED_BY_LOCK(lock);
    MonotonicTime shownTime WTF_GUARDED_BY_LOCK(lock);
    bool clickDone WTF_GUARDED_BY_LOCK(lock) { false };
    unsigned generation WTF_GUARDED_BY_LOCK(lock) { 0 };
    bool hadTarget WTF_GUARDED_BY_LOCK(lock) { false };
};

TapHighlight& tapHighlight()
{
    static NeverDestroyed<TapHighlight> state;
    return state;
}

constexpr Seconds tapHighlightPressDelay = 60_ms;
constexpr Seconds tapHighlightMinimumTime = 100_ms;
constexpr Seconds tapHighlightMaximumTime = 1500_ms;

// Asks the compositor for a frame now or after a delay (to show or hide the box); no-op once it is gone.
void scheduleTapHighlightFrame(Seconds delay = 0_s)
{
    auto& state = tapHighlight();
    Locker locker { state.lock };
    if (!state.compositor)
        return;
    if (!delay) {
        state.compositor->scheduleTapHighlightUpdate();
        return;
    }
    state.compositor->tapHighlightRunLoop().dispatchAfter(delay, [] {
        scheduleTapHighlightFrame();
    });
}

} // namespace

namespace {

struct Pinch {
    enum class Phase : uint8_t { None, Active, Committing };
    Lock lock;
    Phase phase WTF_GUARDED_BY_LOCK(lock) { Phase::None };
    float scale WTF_GUARDED_BY_LOCK(lock) { 1 };
    FloatPoint startCenter WTF_GUARDED_BY_LOCK(lock);
    FloatPoint center WTF_GUARDED_BY_LOCK(lock);
    unsigned generation WTF_GUARDED_BY_LOCK(lock) { 0 };
    float pageScale WTF_GUARDED_BY_LOCK(lock) { 1 };
    float minimumScale WTF_GUARDED_BY_LOCK(lock) { 1 };
    float maximumScale WTF_GUARDED_BY_LOCK(lock) { 5 };
    float initialScale WTF_GUARDED_BY_LOCK(lock) { 1 };
    // Double-tap animation: from scale 1 to animationScale, then onEnd.
    bool animating WTF_GUARDED_BY_LOCK(lock) { false };
    MonotonicTime animationStart WTF_GUARDED_BY_LOCK(lock);
    float animationScale WTF_GUARDED_BY_LOCK(lock) { 1 };
    Function<void(const ThreadedCompositor::PinchResult&)> onEnd WTF_GUARDED_BY_LOCK(lock);
};

constexpr Seconds zoomAnimationDuration = 250_ms;

Pinch& pinchState()
{
    static NeverDestroyed<Pinch> state;
    return state;
}

} // namespace

Lock& ThreadedCompositor::sceneCommitLock()
{
    static Lock lock;
    return lock;
}

void ThreadedCompositor::pinchBegin(const FloatPoint& center)
{
    auto& state = pinchState();
    Locker locker { state.lock };
    // The previous pinch is still being applied by the page: this one would start from a scene about to change.
    if (state.phase == Pinch::Phase::Committing)
        return;
    state.phase = Pinch::Phase::Active;
    state.scale = 1;
    state.startCenter = center;
    state.center = center;
    ++state.generation;
}

void ThreadedCompositor::pinchUpdate(float scale, const FloatPoint& center)
{
    {
        auto& state = pinchState();
        Locker locker { state.lock };
        if (state.phase != Pinch::Phase::Active)
            return;
        state.scale = std::clamp(scale, state.minimumScale / state.pageScale, state.maximumScale / state.pageScale);
        state.center = center;
    }
    scheduleTapHighlightFrame();
}

ThreadedCompositor::PinchResult ThreadedCompositor::pinchEnd()
{
    auto& state = pinchState();
    Locker locker { state.lock };
    if (state.phase != Pinch::Phase::Active)
        return { };
    state.phase = Pinch::Phase::Committing;
    return { true, state.scale, state.startCenter, state.center, state.generation };
}

void ThreadedCompositor::pinchCancel()
{
    {
        auto& state = pinchState();
        Locker locker { state.lock };
        if (state.phase != Pinch::Phase::Active)
            return;
        state.phase = Pinch::Phase::None;
    }
    scheduleTapHighlightFrame();
}

void ThreadedCompositor::setZoomLimits(const ZoomLimits& limits)
{
    auto& state = pinchState();
    Locker locker { state.lock };
    state.pageScale = limits.pageScale > 0 ? limits.pageScale : 1;
    state.minimumScale = limits.minimumScale > 0 ? limits.minimumScale : state.pageScale;
    state.maximumScale = std::max(limits.maximumScale, state.minimumScale);
    state.initialScale = std::clamp(limits.initialScale, state.minimumScale, state.maximumScale);
}

void ThreadedCompositor::pinchCommitted(unsigned generation)
{
    {
        auto& state = pinchState();
        Locker locker { state.lock };
        if (state.phase != Pinch::Phase::Committing || state.generation != generation)
            return;
        state.phase = Pinch::Phase::None;
    }
    scheduleTapHighlightFrame();
}

bool ThreadedCompositor::zoomAnimate(const FloatPoint& point, float scale, Function<void(const PinchResult&)>&& onEnd)
{
    {
        auto& state = pinchState();
        Locker locker { state.lock };
        if (state.phase != Pinch::Phase::None)
            return false;
        state.phase = Pinch::Phase::Active;
        state.scale = 1;
        state.startCenter = point;
        state.center = point;
        ++state.generation;
        state.animating = true;
        state.animationStart = MonotonicTime::now();
        state.animationScale = std::clamp(scale, state.minimumScale / state.pageScale, state.maximumScale / state.pageScale);
        state.onEnd = WTF::move(onEnd);
    }
    scheduleTapHighlightFrame();
    return true;
}

ThreadedCompositor::ZoomLimits ThreadedCompositor::zoomLimits()
{
    auto& state = pinchState();
    Locker locker { state.lock };
    return { state.pageScale, state.minimumScale, state.maximumScale, state.initialScale };
}

bool ThreadedCompositor::zoomGestureActive()
{
    auto& state = pinchState();
    Locker locker { state.lock };
    return state.phase != Pinch::Phase::None;
}

TransformationMatrix ThreadedCompositor::pinchTransform(bool& changed)
{
    TransformationMatrix transform;
    bool active = false;
    Function<void(const PinchResult&)> onEnd;
    PinchResult result;
    {
        auto& state = pinchState();
        Locker locker { state.lock };
        if (state.animating) {
            float t = std::min<float>((MonotonicTime::now() - state.animationStart) / zoomAnimationDuration, 1);
            // Ease out (cubic): fast start, gentle landing.
            float eased = 1 - (1 - t) * (1 - t) * (1 - t);
            state.scale = 1 + (state.animationScale - 1) * eased;
            if (t >= 1) {
                state.animating = false;
                state.phase = Pinch::Phase::Committing;
                onEnd = std::exchange(state.onEnd, nullptr);
                result = { true, state.scale, state.startCenter, state.center, state.generation };
            } else
                m_compositingRunLoop->scheduleUpdate();
        }
        if (state.phase != Pinch::Phase::None) {
            active = true;
            // View point u before the gesture shows at c + s (u - c0): the content under the starting center follows
            // the fingers, scaled by how far they spread.
            transform.translate(state.center.x(), state.center.y());
            transform.scale(state.scale);
            transform.translate(-state.startCenter.x(), -state.startCenter.y());
        }
    }
    if (onEnd)
        onEnd(result);
    changed = active || m_pinchWasActive;
    m_pinchWasActive = active;
    WebCore::CoordinatedBackingStore::setPinchActive(active);
    return transform;
}

void ThreadedCompositor::registerTapHighlightCompositor(ThreadedCompositor* compositor, ThreadedCompositor* leaving)
{
    auto& state = tapHighlight();
    Locker locker { state.lock };
    if (leaving && state.compositor != leaving)
        return;
    state.compositor = compositor;
    state.phase = TapHighlight::Phase::None;
}

void ThreadedCompositor::scheduleTapHighlightUpdate()
{
    m_compositingRunLoop->scheduleUpdate();
}

RunLoop& ThreadedCompositor::tapHighlightRunLoop()
{
    return m_compositingRunLoop->runLoop();
}

void ThreadedCompositor::tapHighlightPress(const FloatPoint& viewPoint)
{
    {
        auto& state = tapHighlight();
        Locker locker { state.lock };
        state.phase = TapHighlight::Phase::Pressed;
        state.point = viewPoint;
        state.pressTime = MonotonicTime::now();
        state.shownTime = { };
        state.clickDone = false;
        ++state.generation;
    }
    scheduleTapHighlightFrame(tapHighlightPressDelay);
}

void ThreadedCompositor::tapHighlightTap(const FloatPoint& viewPoint)
{
    {
        auto& state = tapHighlight();
        Locker locker { state.lock };
        // Same place as the press (the finger did not pan): keep the box already found, if any.
        if (state.phase != TapHighlight::Phase::Pressed || state.point != viewPoint)
            ++state.generation;
        state.phase = TapHighlight::Phase::Tapped;
        state.point = viewPoint;
        state.clickDone = false;
    }
    scheduleTapHighlightFrame();
    // The safety limit, should the click never report back.
    scheduleTapHighlightFrame(tapHighlightMaximumTime);
}

bool ThreadedCompositor::tapHighlightHadTarget()
{
    auto& state = tapHighlight();
    Locker locker { state.lock };
    return state.hadTarget;
}

void ThreadedCompositor::tapHighlightClickDone()
{
    Seconds wait;
    {
        auto& state = tapHighlight();
        Locker locker { state.lock };
        if (state.phase != TapHighlight::Phase::Tapped)
            return;
        state.clickDone = true;
        // Not shown yet: the next frame shows it and the one after the minimum time hides it.
        auto shownFor = state.shownTime ? MonotonicTime::now() - state.shownTime : 0_s;
        wait = std::max(tapHighlightMinimumTime - shownFor, 0_s);
    }
    scheduleTapHighlightFrame(wait);
    scheduleTapHighlightFrame(wait + 20_ms);
}

void ThreadedCompositor::tapHighlightClear()
{
    {
        auto& state = tapHighlight();
        Locker locker { state.lock };
        if (state.phase == TapHighlight::Phase::None)
            return;
        state.phase = TapHighlight::Phase::None;
        ++state.generation;
    }
    scheduleTapHighlightFrame();
}

bool ThreadedCompositor::updateTapHighlight(const TransformationMatrix& matrix)
{
    TapHighlight::Phase phase;
    FloatPoint point;
    unsigned generation;
    bool visible = false;
    {
        auto& state = tapHighlight();
        Locker locker { state.lock };
        auto now = MonotonicTime::now();
        phase = state.phase;
        point = state.point;
        generation = state.generation;
        if (phase == TapHighlight::Phase::Pressed)
            visible = now - state.pressTime >= tapHighlightPressDelay;
        else if (phase == TapHighlight::Phase::Tapped) {
            if (!state.shownTime)
                state.shownTime = now;
            auto shownFor = now - state.shownTime;
            visible = shownFor < tapHighlightMaximumTime && !(state.clickDone && shownFor >= tapHighlightMinimumTime);
            if (!visible)
                state.phase = TapHighlight::Phase::None;
        }
    }

    if (generation != m_tapHighlightGeneration) {
        m_tapHighlightGeneration = generation;
        m_tapHighlightLayer = nullptr;
        m_tapHighlightHitTested = false;
    }

    if (visible && !m_tapHighlightHitTested) {
        m_tapHighlightHitTested = true;
        // The layers in paint order, so the last region containing the point is the one on top. A layer's regions
        // are in its own coordinates; its surface transform includes the scroll offset of this very frame.
        HashMap<const TextureMapperLayer*, CoordinatedPlatformLayer*> platformLayers;
        platformLayers.add(&m_sceneState->rootLayer().ensureTarget(), &m_sceneState->rootLayer());
        for (auto& layer : m_sceneState->committedLayers())
            platformLayers.add(&layer->ensureTarget(), layer.ptr());
        FloatPoint surfacePoint = matrix.mapPoint(point);
        unsigned regions = 0;
        Function<void(TextureMapperLayer&)> visit = [&](TextureMapperLayer& layer) {
            if (auto* platformLayer = platformLayers.get(&layer)) {
                if (auto inverse = layer.toSurfaceTransform().inverse()) {
                    FloatPoint local = inverse->mapPoint(surfacePoint);
                    std::optional<FloatRect> best;
                    {
                        Locker locker { platformLayer->lock() };
                        for (const auto& region : platformLayer->eventRegion().interactionRegions()) {
                            ++regions;
                            if (region.type != InteractionRegion::Type::Interaction || !region.rectInLayerCoordinates.contains(local))
                                continue;
                            // Nested regions (a button inside a link): the smallest is what the finger is on.
                            if (!best || region.rectInLayerCoordinates.area() < best->area())
                                best = region.rectInLayerCoordinates;
                        }
                    }
                    if (best) {
                        m_tapHighlightLayer = platformLayer;
                        m_tapHighlightRect = *best;
                    }
                }
            }
            for (auto* child : layer.children())
                visit(*child);
        };
        visit(m_sceneState->rootLayer().ensureTarget());
        Lumia::log("evidenziazione: %s a %.0f,%.0f (%u regioni nella scena)", m_tapHighlightLayer ? "trovata" : "niente",
            point.x(), point.y(), regions);
        auto& state = tapHighlight();
        Locker locker { state.lock };
        state.hadTarget = !!m_tapHighlightLayer;
    }

    m_tapHighlightVisible = visible && m_tapHighlightLayer;
    bool changed = m_tapHighlightVisible || m_tapHighlightWasVisible;
    m_tapHighlightWasVisible = m_tapHighlightVisible;
    return changed;
}

void ThreadedCompositor::drawTapHighlight()
{
    if (!m_tapHighlightVisible)
        return;
    // Square, as the rest of the phone's UI; mid grey reads on light and dark pages alike.
    static constexpr auto color = SRGBA<uint8_t> { 128, 128, 128, 110 };
    m_textureMapper->drawSolidColor(m_tapHighlightRect, m_tapHighlightLayer->ensureTarget().toSurfaceTransform(), color, true);
}

void ThreadedCompositor::paintToCurrentGLContext(const TransformationMatrix& matrix, const IntSize& size)
{
    // Recording a timeline: wait for the GPU first, so g->h is how long it was still busy and h->u what the tile
    // uploads cost by themselves.
    if (Lumia::timelineRecording()) {
        Lumia::timelineMark('g');
        glFinish();
        Lumia::timelineMark('h');
    }
    WebCore::CoordinatedBackingStore::beginFrameUploads();
    auto updateStart = MonotonicTime::now();
    // The scene and the pinch's transform are read together: the rendering update that commits a pinch's page scale
    // holds this lock until it has dropped the transform (LayerTreeHost::flushLayers).
    bool pinchFrame = false;
    TransformationMatrix viewMatrix = matrix;
    {
        Locker locker { sceneCommitLock() };
        updateSceneState();
        viewMatrix.multiply(pinchTransform(pinchFrame));
    }
    // The frame's clip layer is the view's size in CSS pixels: the device viewport over the device scale (matrix).
    TextureMapperLayer::setSuspendedClipSize(m_pinchWasActive ? FloatSize(size).scaled(1 / matrix.m11()) : FloatSize());
    auto updateEnd = MonotonicTime::now();
    Lumia::timelineMark('u');

    FloatRect clipRect(FloatPoint { }, size);
    TextureMapperLayer& currentRootLayer = m_sceneState->rootLayer().ensureTarget();
    if (currentRootLayer.transform() != viewMatrix)
        currentRootLayer.setTransform(viewMatrix);

    bool sceneHasRunningAnimations = currentRootLayer.applyAnimationsRecursively(MonotonicTime::now());

    // The box appears and disappears without damage of its own: frames that draw it, or the first one without it,
    // repaint everything.
    bool tapHighlightFrame = updateTapHighlight(viewMatrix) || pinchFrame;

    m_textureMapper->beginPainting(m_flipY ? TextureMapper::FlipY::Yes : TextureMapper::FlipY::No);
    m_textureMapper->setViewportRect(IntRect(IntPoint { }, size));
    m_textureMapper->beginClip(TransformationMatrix(), FloatRoundedRect(clipRect));

#if ENABLE(DAMAGE_TRACKING)
    std::optional<FloatRoundedRect> rectContainingRegionThatActuallyChanged;
    currentRootLayer.prepareForPainting(*m_textureMapper);
    if (m_damage.flags) {
        Damage frameDamage(size, m_damage.flags->contains(DamagePropagationFlags::Unified) ? Damage::Mode::BoundingBox : Damage::Mode::Rectangles);

        WTFBeginSignpost(this, CollectDamage);
        currentRootLayer.collectDamage(*m_textureMapper, frameDamage);
        WTFEndSignpost(this, CollectDamage);

        if (m_damage.shouldNotifyFrameDamageForTesting && m_layerTreeHost)
            m_layerTreeHost->notifyFrameDamageForTesting(frameDamage.regionForTesting());

        if (!frameDamage.isEmpty())
            m_surface->setFrameDamage(WTF::move(frameDamage));

        if (m_damage.flags->contains(DamagePropagationFlags::UseForCompositing)) {
            const auto& damageSinceLastSurfaceUse = m_surface->renderTargetDamage();
            if (damageSinceLastSurfaceUse && !FloatRect(damageSinceLastSurfaceUse->bounds()).contains(clipRect))
                rectContainingRegionThatActuallyChanged = FloatRoundedRect(damageSinceLastSurfaceUse->bounds());

            m_textureMapper->setDamage(damageSinceLastSurfaceUse);
        }
    }

    if (tapHighlightFrame && m_damage.flags) {
        rectContainingRegionThatActuallyChanged = std::nullopt;
        m_textureMapper->setDamage(std::nullopt);
        m_surface->setFrameDamage(Damage(size, Damage::Mode::Full));
    }
    if (rectContainingRegionThatActuallyChanged)
        m_textureMapper->beginClip(TransformationMatrix(), *rectContainingRegionThatActuallyChanged);
#endif

    m_surface->clear({ });

    WTFBeginSignpost(this, PaintTextureMapperLayerTree);
    // Timeline: its first frame logs its draws.
    bool recording = Lumia::timelineRecording();
    WebCore::lumiaDumpDraws = recording && !m_dumpedDraws;
    m_dumpedDraws = recording;
    currentRootLayer.paint(*m_textureMapper);
    WebCore::lumiaDumpDraws = false;
    drawTapHighlight();
    WTFEndSignpost(this, PaintTextureMapperLayerTree);

#if ENABLE(DAMAGE_TRACKING)
    if (rectContainingRegionThatActuallyChanged)
        m_textureMapper->endClip();
#endif

#if ENABLE(DAMAGE_TRACKING)
    if (m_damage.visualizer) {
        m_damage.visualizer->paintDamage(*m_textureMapper, m_surface->frameDamage());
        // When damage visualizer is active, we cannot send the original damage to the platform as in this case
        // the damage rects visualized previous frame may not get erased if platform actually uses damage.
        m_surface->setFrameDamage(Damage(size, Damage::Mode::Full));
    }
#endif

    m_textureMapper->endClip();
    m_textureMapper->endPainting();
    m_frameStats.paintBreakdown((updateEnd - updateStart).milliseconds(), (MonotonicTime::now() - updateEnd).milliseconds(),
        WebCore::lumiaDrawCalls, WebCore::lumiaUploads, WebCore::lumiaUploadBytes, WebCore::lumiaUploadMs);
    WebCore::lumiaDrawCalls = 0;
    WebCore::lumiaUploads = 0;
    WebCore::lumiaUploadBytes = 0;
    WebCore::lumiaUploadMs = 0;

    if (sceneHasRunningAnimations || WebCore::CoordinatedBackingStore::uploadsDeferred())
        scheduleUpdate();
}

void ThreadedCompositor::renderLayerTree()
{
    Lumia::timelineMark('b');
    ASSERT(m_sceneState);
    ASSERT(m_compositingRunLoop->isCurrent());

    if (m_suspendedCount > 0)
        return;

    if (!m_context || !m_context->makeContextCurrent())
        return;

    // Retrieve the scene attributes in a thread-safe manner.
    IntSize viewportSize;
    float deviceScaleFactor;
    {
        Locker locker { m_attributes.lock };
        viewportSize = m_attributes.viewportSize;
        deviceScaleFactor = m_attributes.deviceScaleFactor;

#if !HAVE(DISPLAY_LINK)
        // Client has to be notified upon finishing this scene update when the frame carries a commit it is waiting
        // for (LayerTreeHost::commitSceneState -> requestComposition). The PlayStation code asked
        // m_sceneState->layersDidChange(), which CoordinatedSceneState::flush() has already reset by now: the client
        // then waited forever after the first frame.
        uint32_t requestID = m_compositionRequestID.load();
        m_attributes.clientRendersNextFrame = requestID != m_renderedCompositionRequestID;
        m_renderedCompositionRequestID = requestID;
#endif
    }

    if (viewportSize.isEmpty())
        return;

    TransformationMatrix viewportTransform;
    viewportTransform.scale(deviceScaleFactor);

    m_surface->willRenderFrame(viewportSize);
    // No task for the Web thread per frame (WebKit's willRenderFrame/didRenderFrame): nothing on this port listens
    // (ScrollingTreeCoordinated::didCompleteRenderingUpdate), and a scroll or fling frame must not wake a page.

    WTFBeginSignpost(this, PaintToGLContext);
    auto paintStart = MonotonicTime::now();
    paintToCurrentGLContext(viewportTransform, viewportSize);
    auto paintEnd = MonotonicTime::now();
    WTFEndSignpost(this, PaintToGLContext);

    updateFPSCounter();

    uint32_t compositionRequestID = m_compositionRequestID.load();
#if HAVE(DISPLAY_LINK)
    m_compositionResponseID = compositionRequestID;
    if (!m_didRenderFrameTimer.isActive())
        m_didRenderFrameTimer.startOneShot(0_s);
#else
    UNUSED_VARIABLE(compositionRequestID);
#endif

    WTFEmitSignpost(this, DidRenderFrame, "compositionResponseID %i", compositionRequestID);

    Lumia::timelineMark('d');
    m_context->swapBuffers();
    Lumia::timelineMark('w');
    // A frame that never reaches the screen (the page stuck on one picture while scrolling and tiles still worked,
    // claude.ai 2026-10-27): the swap's EGL error and, on a lost device, D3D's reason.
    if (EGLint swapError = eglGetError(); swapError != EGL_SUCCESS && swapError != m_lastSwapError) {
        m_lastSwapError = swapError;
        Lumia::log("ThreadedCompositor: swap fallito, errore EGL 0x%x, glError 0x%x, %s", swapError, glGetError(),
            AcceleratedSurface::deviceRemovedReason().utf8().data());
    }
    auto swapEnd = MonotonicTime::now();
    // The texture pool frees what has gone unused for 3 s from a RunLoop timer on the Web thread, which has no GL
    // context on this port and so released nothing: on claude.ai 272 textures (456 MB) piled up until the OS killed
    // the app. It runs here instead, after each frame, with this thread's context current.
    BitmapTexturePool::singleton().wp81ReleaseUnusedTextures();
    // Frame pacing under the finger: interval between two presented frames, time spent painting and time spent in
    // swapBuffers (the phone's EGL waits for the display), summarised every 2 s (FrameStats.h).
    m_frameStats.frameRendered(paintStart, paintEnd, swapEnd);
    // The first frames, as a check that the scene reaches the screen (FrameStats summarizes the rest).
    static unsigned frameCount;
    if (frameCount < 3) {
        ++frameCount;
        // Layers in the scene: the root's children and their children.
        unsigned layers = 0;
        Vector<TextureMapperLayer*> stack { &m_sceneState->rootLayer().ensureTarget() };
        while (!stack.isEmpty()) {
            auto* layer = stack.takeLast();
            layers++;
            for (auto* child : layer->children())
                stack.append(child);
        }
        Lumia::log("ThreadedCompositor: fotogramma %u, %dx%d, layer nella scena %u, glError 0x%x", frameCount, viewportSize.width(),
            viewportSize.height(), layers, glGetError());
    }

    m_surface->didRenderFrame();
}

uint32_t ThreadedCompositor::requestComposition()
{
    ASSERT(RunLoop::isMain());
    uint32_t compositionRequestID = ++m_compositionRequestID;
    scheduleUpdate();
    return compositionRequestID;
}

void ThreadedCompositor::scheduleUpdate()
{
    m_compositingRunLoop->scheduleUpdate();
}

void ThreadedCompositor::requestCompositionAtNextDisplayRefresh()
{
#if HAVE(DISPLAY_LINK)
    // With a display link the compositor already waits for the display's frame callback.
    scheduleUpdate();
#else
    m_compositionRequestedForNextDisplayRefresh = true;
    if (!m_display.running.load()) {
        wakeFrameClock();
        return;
    }
    // The clock is running: start the frame now instead of at the next tick. The scrolling thread answers a tick
    // asynchronously, so its request always arrived just after the tick and waited for the next one, a RunLoop timer
    // that Windows' 15.6 ms timer resolution turned into 16-31 ms: 23-28 ms frames with 3 ms of work and a 16.67 ms
    // vblank (lw_gpu_test). The swap that follows waits for the display, which is the pacing.
    if (m_display.frameDispatchQueued.exchange(true))
        return;
    if (RunLoop* runLoop = this->runLoop()) {
        runLoop->dispatch([this, protectedThis = Ref { *this }] {
            m_display.frameDispatchQueued = false;
            startRequestedFrame();
        });
    } else
        m_display.frameDispatchQueued = false;
#endif
}

#if !HAVE(DISPLAY_LINK)
void ThreadedCompositor::startRequestedFrame()
{
    if (!m_display.running || !m_compositionRequestedForNextDisplayRefresh.exchange(false))
        return;
    m_display.idleTicks = 0;
    Lumia::timelineMark('f');
    m_compositingRunLoop->scheduleUpdate();
    m_display.tickAfterFrame = true;
    // Safety net, as in displayUpdateFired: frameComplete ticks at once.
    m_display.updateTimer->startOneShot(Seconds { 1.0 / m_display.displayUpdate.updatesPerSecond } * 2);
}
#endif

RunLoop* ThreadedCompositor::runLoop()
{
    if (!m_compositingRunLoop)
        return nullptr;

    return &m_compositingRunLoop->runLoop();
}

void ThreadedCompositor::frameComplete()
{
    WTFEmitSignpost(this, FrameComplete);
    Lumia::timelineMark('c');

    ASSERT(m_compositingRunLoop->isCurrent());
#if !HAVE(DISPLAY_LINK)
    // A frame the clock asked for: the swap has just returned, which on this swap chain is the display's refresh
    // (it waits for the previous frame to be shown), so the next tick starts here instead of on a free-running timer
    // that beat against the display (median 21 ms, every other frame a missed refresh).
    if (m_display.tickAfterFrame) {
        m_display.tickAfterFrame = false;
        if (m_display.updateTimer)
            m_display.updateTimer->startOneShot(0_s);
    }
    // requestAnimationFrame reads the update number: it advances with the frames while the clock is stopped.
    if (!m_display.running)
        m_display.displayUpdate = m_display.displayUpdate.nextUpdate();
    sceneUpdateFinished();
#else
    Locker stateLocker { m_compositingRunLoop->stateLock() };
    m_compositingRunLoop->updateCompleted(stateLocker);
#endif
}

#if HAVE(DISPLAY_LINK)
void ThreadedCompositor::didRenderFrameTimerFired()
{
    if (m_layerTreeHost)
        m_layerTreeHost->didComposite(m_compositionResponseID);
}
#else
WebCore::DisplayRefreshMonitor& ThreadedCompositor::displayRefreshMonitor() const
{
    return m_displayRefreshMonitor.get();
}

// The frame clock. It ticks only while something moves without the Web thread: the scrolling trees (a finger, a fling,
// rubber banding: ScrollingTree::hasRecentActivity) and the frames the scrolling thread or the tile workers ask for
// (requestCompositionAtNextDisplayRefresh). A few idle ticks stop it, and a request wakes it again: a still page no
// longer wakes the compositor and the scrolling thread 60 times a second. Web thread frames (commits,
// requestAnimationFrame) never needed it: they go through scheduleUpdate and are paced by the swap.
//
// While it runs, a tick that starts a frame is followed by the next tick as soon as that frame's swap returns
// (frameComplete), i.e. on the refresh; ticks without a frame (a finger held still) use the timer.
void ThreadedCompositor::startFrameClock()
{
    if (!m_display.updateTimer || m_suspendedCount > 0)
        return;
    m_display.running = true;
    m_display.idleTicks = 0;
    m_display.tickAfterFrame = false;
    // At once: the request that woke the clock is usually a finger, and waiting a refresh would only add latency.
    m_display.updateTimer->startOneShot(0_s);
}

void ThreadedCompositor::wakeFrameClock()
{
    // Any thread. Pairs with the stop in displayUpdateFired(): the clock clears `running` before it looks at the
    // request flag a last time, and a requester sets that flag before it looks at `running`, so a request is never
    // left without a tick. While the clock runs this is one atomic load per request.
    if (m_display.running.load() || m_display.wakeQueued.exchange(true))
        return;
    RunLoop* runLoop = this->runLoop();
    if (!runLoop) {
        m_display.wakeQueued = false;
        return;
    }
    runLoop->dispatch([this, protectedThis = Ref { *this }] {
        m_display.wakeQueued = false;
        if (!m_display.running)
            startFrameClock();
    });
}

void ThreadedCompositor::displayUpdateFired()
{
    // Ticks with nothing to do before the clock stops: long enough for the scrolling thread's answer to a tick (it
    // runs the fling a moment later, on its own thread) to arrive as a new request.
    constexpr unsigned idleTicksBeforeStop = 4;

    if (!m_display.running)
        return;
    Lumia::timelineMark('t');
    m_display.tickAfterFrame = false;
    m_display.displayUpdate = m_display.displayUpdate.nextUpdate();
    m_frameStats.clockTicked();

    bool scrolling = ScrollingTreeDispatcher::singleton().notifyScrollingTreesDisplayDidRefresh(m_display.displayID);

    // A frame asked for by the scrolling thread starts here, on the refresh boundary.
    bool requested = m_compositionRequestedForNextDisplayRefresh.exchange(false);
    if (requested)
        m_compositingRunLoop->scheduleUpdate();

    if (requested || scrolling)
        m_display.idleTicks = 0;
    else if (++m_display.idleTicks >= idleTicksBeforeStop) {
        m_display.running = false;
        if (!m_compositionRequestedForNextDisplayRefresh.load())
            return;
        // A request came in while stopping: keep going.
        m_display.running = true;
        m_display.idleTicks = 0;
    }

    Seconds period { 1.0 / m_display.displayUpdate.updatesPerSecond };
    if (requested) {
        m_display.tickAfterFrame = true;
        // Only a safety net: frameComplete restarts the timer at once. A frame that never completes (suspended,
        // no GL context) must not stop the clock with `running` still set, or no request could wake it again.
        m_display.updateTimer->startOneShot(period * 2);
    } else
        m_display.updateTimer->startOneShot(period);
}

void ThreadedCompositor::sceneUpdateFinished()
{
    // The composition has finished. Now we have to determine how to manage
    // the scene update completion.

    // The DisplayRefreshMonitor will be used to dispatch a callback on the client thread if:
    //  - clientRendersNextFrame is true (i.e. client has to be notified about the finished update), or
    //  - a DisplayRefreshMonitor callback was requested from the Web engine
    bool shouldDispatchDisplayRefreshCallback = m_displayRefreshMonitor->requiresDisplayRefreshCallback(m_display.displayUpdate);

    if (!shouldDispatchDisplayRefreshCallback) {
        Locker locker { m_attributes.lock };
        shouldDispatchDisplayRefreshCallback |= m_attributes.clientRendersNextFrame;
    }

    Locker stateLocker { m_compositingRunLoop->stateLock() };

    // Schedule the DisplayRefreshMonitor callback, if necessary.
    if (shouldDispatchDisplayRefreshCallback)
        m_displayRefreshMonitor->dispatchDisplayRefreshCallback();

    // Mark the scene update as completed.
    m_compositingRunLoop->updateCompleted(stateLocker);
}
#endif // !HAVE(DISPLAY_LINK)

void ThreadedCompositor::updateSceneAttributes(const IntSize& size, float deviceScaleFactor)
{
    m_attributes.viewportSize = size;
    m_attributes.deviceScaleFactor = deviceScaleFactor;
    m_attributes.viewportSize.scale(m_attributes.deviceScaleFactor);
}

void ThreadedCompositor::initializeFPSCounter()
{
    // When the envvar is set, the FPS is logged to the console, so it may be necessary to enable the
    // 'LogsPageMessagesToSystemConsole' runtime preference to see it.
    const auto showFPSEnvironment = String::fromLatin1(getenv("WEBKIT_SHOW_FPS"));
    bool ok = false;
    Seconds interval(showFPSEnvironment.toDouble(&ok));
    if (ok && interval) {
        m_fpsCounter.exposesFPS = true;
        m_fpsCounter.calculationInterval = interval;
    }
}

void ThreadedCompositor::updateFPSCounter()
{
    if (!m_fpsCounter.exposesFPS)
        return;

    m_fpsCounter.frameCountSinceLastCalculation++;
    const Seconds delta = MonotonicTime::now() - m_fpsCounter.lastCalculationTimestamp;
    if (delta >= m_fpsCounter.calculationInterval) {
        WTFSetCounter(FPS, static_cast<int>(std::round(m_fpsCounter.frameCountSinceLastCalculation / delta.seconds())));
        if (m_fpsCounter.exposesFPS)
            m_fpsCounter.fps = m_fpsCounter.frameCountSinceLastCalculation / delta.seconds();
        m_fpsCounter.frameCountSinceLastCalculation = 0;
        m_fpsCounter.lastCalculationTimestamp += delta;
    } else if (m_fpsCounter.exposesFPS)
        m_fpsCounter.fps = std::nullopt;
}

}
#endif // USE(COORDINATED_GRAPHICS)
