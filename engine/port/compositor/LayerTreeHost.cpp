/*
 * Copyright (C) 2011-2025 Apple Inc. All rights reserved.
 * Copyright (C) 2011 Nokia Corporation and/or its subsidiary(-ies).
 * Copyright (C) 2012 Company 100, Inc.
 * Copyright (C) 2014-2019 Igalia S.L.
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
#include "LayerTreeHost.h"

#if USE(COORDINATED_GRAPHICS)
#include "CoordinatedSceneState.h"
#include "Engine.h"
#include "WebPage.h"
#include <JavaScriptCore/ConsoleTypes.h>
#include <WebCore/AsyncScrollingCoordinator.h>
#include <WebCore/Chrome.h>
#include <WebCore/CoordinatedTileBuffer.h>
#include <WebCore/Document.h>
#include <WebCore/ProgressTracker.h>
#include <WebCore/Damage.h>
#include <WebCore/GraphicsLayerCoordinated.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/NativeImage.h>
#include <WebCore/PageOverlayController.h>
#include <WebCore/RenderLayerBacking.h>
#include <wtf/Activity.h>
#include <WebCore/RenderView.h>
#include <WebCore/ScrollingThread.h>
#include <WebCore/Settings.h>
#include <WebCore/ThreadedScrollingTree.h>
#include <wtf/Scope.h>
#include <wtf/SetForScope.h>
#include <wtf/SystemTracing.h>
#include <wtf/TZoneMallocInlines.h>

#if USE(CAIRO)
#include <WebCore/CairoPaintingEngine.h>
#elif USE(SKIA)
#include <WebCore/SkiaPaintingEngine.h>
#endif

namespace WebKit {

// About 16 screen-wide tiles (768x512x4): two full screens being repainted. A first paint of a long page creates more
// at once; only the next updates wait for them.
static constexpr double maximumTileBacklogBytes = 24 * 1024 * 1024;
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(LayerTreeHost);

#if HAVE(DISPLAY_LINK)
LayerTreeHost::LayerTreeHost(WebPage& webPage)
#else
LayerTreeHost::LayerTreeHost(WebPage& webPage, WebCore::PlatformDisplayID displayID)
#endif
    : m_webPage(webPage)
    , m_sceneState(CoordinatedSceneState::create())
    , m_layerFlushTimer(RunLoop::mainSingleton(), "LayerTreeHost::LayerFlushTimer"_s, this, &LayerTreeHost::layerFlushTimerFired)
    , m_tileBacklogTimer(RunLoop::mainSingleton(), "LayerTreeHost::TileBacklogTimer"_s, this, &LayerTreeHost::tileBacklogTimerFired)
    , m_renderingWatchdog(RunLoop::mainSingleton(), "LayerTreeHost::RenderingWatchdog"_s, this, &LayerTreeHost::renderingWatchdogFired)
#if !HAVE(DISPLAY_LINK)
    , m_displayID(displayID)
#endif
#if USE(CAIRO)
    , m_paintingEngine(Cairo::PaintingEngine::create())
#elif USE(SKIA)
    // No GrContext proxy: no DDL recording, tiles are painted with PlatformDisplay's Skia GL context.
    , m_skiaPaintingEngine(SkiaPaintingEngine::create(nullptr))
#endif
{
    {
        auto& rootLayer = m_sceneState->rootLayer();
#if ENABLE(DAMAGE_TRACKING)
        rootLayer.setDamagePropagationEnabled(webPage.corePage()->settings().propagateDamagingInformation());
        if (webPage.corePage()->settings().propagateDamagingInformation()) {
            m_damageInGlobalCoordinateSpace = std::make_shared<Damage>(m_webPage.size());
            rootLayer.setDamageInGlobalCoordinateSpace(m_damageInGlobalCoordinateSpace);
        }
#endif
        Locker locker { rootLayer.lock() };
        rootLayer.setAnchorPoint(FloatPoint3D(0, 0, 0));
        rootLayer.setSize(m_webPage.size());
    }

    scheduleRenderingUpdate();

#if HAVE(DISPLAY_LINK)
    m_compositor = ThreadedCompositor::create(*this);
#else
    m_compositor = ThreadedCompositor::create(*this, *this, displayID);
#endif
    m_renderingWatchdog.startRepeating(1_s);
#if ENABLE(DAMAGE_TRACKING)
    std::optional<OptionSet<ThreadedCompositor::DamagePropagationFlags>> damagePropagationFlags;
    const auto& settings = webPage.corePage()->settings();
    if (settings.propagateDamagingInformation()) {
        damagePropagationFlags = OptionSet<ThreadedCompositor::DamagePropagationFlags> { };
        if (settings.unifyDamagedRegions())
            damagePropagationFlags->add(ThreadedCompositor::DamagePropagationFlags::Unified);
        if (settings.useDamagingInformationForCompositing())
            damagePropagationFlags->add(ThreadedCompositor::DamagePropagationFlags::UseForCompositing);
    }
    m_compositor->setDamagePropagationSettings(damagePropagationFlags, settings.damageRectangleThreshold());
#endif
}

LayerTreeHost::~LayerTreeHost()
{
    if (m_forceRepaintAsync.callback)
        m_forceRepaintAsync.callback();

    cancelRenderingUpdate();

#if USE(SKIA)
    m_skiaPaintingEngine = nullptr;
#endif

    // ThreadedCompositor must be invalidated before invalidating CoordinatedSceneState
    // to invalidate pending layers in the compositor thread.
    m_compositor->invalidate();
    m_sceneState->invalidate();
}

void LayerTreeHost::setLayerTreeStateIsFrozen(bool isFrozen)
{
    if (m_layerTreeStateIsFrozen == isFrozen)
        return;

    m_layerTreeStateIsFrozen = isFrozen;

    if (m_layerTreeStateIsFrozen)
        cancelRenderingUpdate();
    else
        scheduleRenderingUpdate();
}

void LayerTreeHost::scheduleRenderingUpdate()
{
    WTFEmitSignpost(this, ScheduleLayerFlush, "isWaitingForRenderer %i", m_isWaitingForRenderer);

    if (m_layerTreeStateIsFrozen)
        return;

    if (m_webPage.size().isEmpty())
        return;

    if (m_isWaitingForRenderer) {
        m_scheduledWhileWaitingForRenderer = true;
        return;
    }

    auto delay = loadPacingDelay();
    if (!m_layerFlushTimer.isActive()) {
        m_flushIsPaced = delay > 0_s;
        m_layerFlushTimer.startOneShot(delay);
    } else if (m_flushIsPaced && !delay) {
        // The reason for the pause is gone (a finger on the screen, the load ended): now.
        m_flushIsPaced = false;
        m_layerFlushTimer.startOneShot(0_s);
    }
}

// While a page loads, every rendering update resolves the style of what arrived since the last one, lays it out,
// paints and records it: on this phone 100-180 ms each on a page like poste.it, back to back, which left the load
// itself (parser, scripts, style sheets) a fifth of the Web thread. A desktop engine can afford to show every step.
// Here, after an update that cost X the load gets 2X of the thread before the next (at most 400 ms): the page still
// appears in steps, a few per second instead of one per frame, and finishes sooner. Not when an update is cheap (under
// 30 ms: nothing to gain), not once the load is over, and never within a second of a touch or a key: what the user
// does is answered at once.
Seconds LayerTreeHost::loadPacingDelay() const
{
    if (m_lastFlushDuration < 30_ms || !Lumia::loadPacingEnabled())
        return 0_s;
    RefPtr page = m_webPage.corePage();
    if (!page || !page->progress().isMainLoadProgressing())
        return 0_s;
    auto now = MonotonicTime::now();
    if (now - Lumia::lastUserInput() < 1_s)
        return 0_s;
    auto next = m_lastFlushTime + m_lastFlushDuration + std::min(m_lastFlushDuration * 2, Seconds { 400_ms });
    return std::max(next - now, Seconds { 0_s });
}

// The update asked for while the compositor was busy with the last one: didComposite and renderNextFrame would run it
// at once, which is how the updates of a load follow each other back to back. True when it waits out the load's pause
// on the flush timer instead.
bool LayerTreeHost::deferForLoadPacing()
{
    auto delay = loadPacingDelay();
    if (!delay)
        return false;
    m_flushIsPaced = true;
    m_layerFlushTimer.startOneShot(delay);
    return true;
}

void LayerTreeHost::cancelRenderingUpdate()
{
    m_layerFlushTimer.stop();
}

void LayerTreeHost::flushLayers()
{
    RELEASE_ASSERT(!m_isFlushingLayers);
    if (m_layerTreeStateIsFrozen)
        return;

    // F2.c: the whole rendering update (style, layout, compositing) as seen from the Web thread. Its inner scopes say
    // where the time went; this label says how much of the task it was.
    WTF::ActivityRecorder::Scope activityScope(WTF::Activity::Render);

    SetForScope<bool> reentrancyProtector(m_isFlushingLayers, true);
    m_lastFlushTime = MonotonicTime::now();
    m_flushIsPaced = false;

    // Committing a pinch: the compositor must not draw a scene that has the new page scale while still applying
    // its own pinch scale, nor the reverse. Layer changes reach the compositor's layers as they are flushed, so the
    // whole update runs under the lock the compositor takes around its scene update; the fingers are up, so the
    // frames it delays show nothing moving.
    std::optional<Locker<Lock>> pinchLocker;
    auto pinchGeneration = std::exchange(m_pinchCommitGeneration, std::nullopt);
    if (pinchGeneration)
        pinchLocker.emplace(ThreadedCompositor::sceneCommitLock());
    auto finishPinch = makeScopeExit([&] {
        if (pinchGeneration)
            ThreadedCompositor::pinchCommitted(*pinchGeneration);
    });

    Ref page { m_webPage };
    page->updateRendering();
    page->flushPendingEditorStateUpdate();

    // The clear colour follows the document's background (see AcceleratedSurface::backgroundColorDidChange): this is
    // what a not-yet-rasterized tile shows while scrolling.
    backgroundColorDidChange();

    if (m_overlayCompositingLayer)
        m_overlayCompositingLayer->flushCompositingState(visibleContentsRect());

    page->finalizeRenderingUpdate({ FinalizeRenderingUpdateFlags::ApplyScrollingTreeLayerPositions });

    if (m_pendingResize) {
        m_compositor->setSize(page->size(), page->deviceScaleFactor());
        auto& rootLayer = m_sceneState->rootLayer();
        Locker locker { rootLayer.lock() };
        rootLayer.setSize(page->size());
    }

    bool didChangeSceneState = m_sceneState->flush();
    if (m_compositionRequired || m_pendingResize || m_forceFrameSync || didChangeSceneState)
        commitSceneState();

    m_compositionRequired = false;
    m_pendingResize = false;
    m_forceFrameSync = false;

    page->didUpdateRendering();

    // Eject any backing stores whose only reference is held in the HashMap cache.
    m_imageBackingStores.removeIf([](auto& it) {
        return it.value->hasOneRef();
    });

    if (m_waitUntilPaintingComplete) {
        m_sceneState->waitUntilPaintingComplete();
        m_waitUntilPaintingComplete = false;
    }
    m_lastFlushDuration = MonotonicTime::now() - m_lastFlushTime;
}

void LayerTreeHost::layerFlushTimerFired()
{
    WTFBeginSignpost(this, LayerFlushTimerFired, "isWaitingForRenderer %i", m_isWaitingForRenderer);

    if (m_isSuspended) {
        WTFEndSignpost(this, LayerFlushTimerFired);
        return;
    }

    if (m_isWaitingForRenderer) {
        WTFEndSignpost(this, LayerFlushTimerFired);
        return;
    }

    if (CoordinatedTileBuffer::currentMemoryUsage() > maximumTileBacklogBytes) {
        if (!m_waitingForTileBacklog) {
            m_waitingForTileBacklog = true;
            Lumia::log("tile: %.0f MB in attesa di raster o upload, aggiornamento del rendering rimandato",
                CoordinatedTileBuffer::currentMemoryUsage() / (1024 * 1024));
        }
        if (!m_tileBacklogTimer.isActive())
            m_tileBacklogTimer.startOneShot(8_ms);
        WTFEndSignpost(this, LayerFlushTimerFired);
        return;
    }

#if !HAVE(DISPLAY_LINK)
    // If a force-repaint callback was registered, we should force a 'frame sync' that
    // will guarantee us a call to renderNextFrame() once the update is complete.
    if (m_forceRepaintAsync.callback)
        m_forceFrameSync = true;
#endif

    flushLayers();

    WTFEndSignpost(this, LayerFlushTimerFired);
}

void LayerTreeHost::tileBacklogTimerFired()
{
    if (CoordinatedTileBuffer::currentMemoryUsage() > maximumTileBacklogBytes) {
        m_tileBacklogTimer.startOneShot(8_ms);
        return;
    }
    m_waitingForTileBacklog = false;
    scheduleRenderingUpdate();
}

// The page asked for a rendering update and none ran: seen on 2026-11-06, pictures stopped for up to 130 s (only pans
// moved) until the app was closed and reopened; the cause is not known yet. WebCore's side of the request is exact:
// Page::scheduleRenderingUpdate sets a flag that only Page::updateRendering clears (nextRenderingUpdateTimestamp is
// its public face), so "asked, and no flush between two ticks of this timer" is a broken chain, whatever link broke.
// The line says where each link stands (page -> RenderingUpdateScheduler -> display refresh monitor -> this host's
// flush timer -> compositor -> refresh callback); then one flush runs, which completes every cycle. A document that
// only needs layout or style while it loads is not a stall (WebCore holds those until the style sheets arrive), and a
// Web thread busy for a second is not one either: the request must be seen pending by two ticks in a row.
void LayerTreeHost::renderingWatchdogFired()
{
    if (m_isSuspended || m_layerTreeStateIsFrozen || m_isFlushingLayers || m_webPage.size().isEmpty()) {
        m_watchdogPendingSince = { };
        return;
    }
    auto now = MonotonicTime::now();
    RefPtr page = m_webPage.corePage();
    bool asked = page && page->nextRenderingUpdateTimestamp();
    bool commitLost = m_isWaitingForRenderer && now - m_lastCommitTime > 1_s && !m_compositor->isActive();
    if ((!asked && !commitLost) || m_waitingForTileBacklog) {
        m_watchdogPendingSince = { };
        return;
    }
    // The same flush as at the last tick is still the latest one: nothing ran in between.
    if (m_watchdogPendingSince != m_lastFlushTime) {
        m_watchdogPendingSince = m_lastFlushTime;
        return;
    }
    bool needsLayout = false;
    bool needsStyle = false;
    bool visualUpdates = true;
    if (auto* view = m_webPage.localMainFrameView()) {
        needsLayout = view->needsLayout();
        if (RefPtr document = view->frame().document()) {
            visualUpdates = document->visualUpdatesAllowed();
            needsStyle = document->hasPendingStyleRecalc();
        }
    }
    Lumia::log("ridisegno: fermo da %.0f ms: pagina in attesa %d, visibile %d, attesa compositor %d (commit da %.0f ms), richiesta in attesa %d, timer %d, compositor attivo %d, %s, layout %d, stile %d, caricamento %d, aggiornamenti visivi %d",
        (now - m_lastFlushTime).milliseconds(), asked, page && page->isVisible(), m_isWaitingForRenderer, (now - m_lastCommitTime).milliseconds(),
        m_scheduledWhileWaitingForRenderer, m_layerFlushTimer.isActive(), m_compositor->isActive(), m_compositor->displayRefreshState().utf8().data(),
        needsLayout, needsStyle, page && page->progress().isMainLoadProgressing(), visualUpdates);
    m_watchdogPendingSince = { };
    m_isWaitingForRenderer = false;
    m_scheduledWhileWaitingForRenderer = false;
    m_forceFrameSync = true;
    cancelRenderingUpdate();
    flushLayers();
}

void LayerTreeHost::updateRootLayer()
{
    Vector<Ref<CoordinatedPlatformLayer>> children;
    if (m_rootCompositingLayer) {
        children.append(downcast<GraphicsLayerCoordinated>(m_rootCompositingLayer)->coordinatedPlatformLayer());
        if (m_overlayCompositingLayer)
            children.append(downcast<GraphicsLayerCoordinated>(m_overlayCompositingLayer)->coordinatedPlatformLayer());
    }

    m_sceneState->setRootLayerChildren(WTF::move(children));
}

void LayerTreeHost::setRootCompositingLayer(GraphicsLayer* graphicsLayer)
{
    if (m_rootCompositingLayer == graphicsLayer)
        return;

    m_rootCompositingLayer = graphicsLayer;
    updateRootLayer();
}

void LayerTreeHost::setViewOverlayRootLayer(GraphicsLayer* graphicsLayer)
{
    if (m_overlayCompositingLayer == graphicsLayer)
        return;

    m_overlayCompositingLayer = graphicsLayer;
    updateRootLayer();
}

void LayerTreeHost::commitPinch(unsigned generation)
{
    m_pinchCommitGeneration = generation;
    m_forceFrameSync = true;
    scheduleRenderingUpdate();
}

void LayerTreeHost::updateRenderingWithForcedRepaint()
{
#if !HAVE(DISPLAY_LINK)
    // This is necessary for running layout tests. Since in this case we are not waiting for a UIProcess to reply nicely.
    // Instead we are just triggering forceRepaint. But we still want to have the scripted animation callbacks being executed.
    if (auto* frameView = m_webPage.localMainFrameView())
        frameView->updateLayoutAndStyleIfNeededRecursive();

    // We need to schedule another flush, otherwise the forced paint might cancel a later expected flush.
    m_forceFrameSync = true;
    scheduleRenderingUpdate();

    if (!m_isWaitingForRenderer)
        flushLayers();
#else
    if (m_isWaitingForRenderer) {
        if (m_forceRepaintAsync.callback)
            m_pendingForceRepaint = true;
        return;
    }

    m_pendingForceRepaint = false;
    m_webPage.corePage()->forceRepaintAllFrames();
    m_forceFrameSync = true;

    // Make sure `m_sceneState->waitUntilPaintingComplete()` is invoked at the
    // end of the currently running layer flush, or after the next one if there
    // is none ongoing at present.
    m_waitUntilPaintingComplete = true;

    // If updateRenderingWithForcedRepaint() is invoked via JS through e.g. a rAF() callback, a call
    // to `page->updateRendering()` _during_ a layer flush is responsible for that.
    // If m_isFlushingLayers is true, that layer flush is still ongoing, so we do
    // not need to cancel pending ones and immediately flush again (re-entrancy!).
    if (m_isFlushingLayers)
        return;
    cancelRenderingUpdate();
    flushLayers();
#endif
}

void LayerTreeHost::updateRenderingWithForcedRepaintAsync(CompletionHandler<void()>&& callback)
{
#if !HAVE(DISPLAY_LINK)
    scheduleRenderingUpdate();

    // We want a clean repaint, meaning that if we're currently waiting for the renderer
    // to finish an update, we'll have to schedule another flush when it's done.
    ASSERT(!m_forceRepaintAsync.callback);
    m_forceRepaintAsync.callback = WTF::move(callback);
    m_forceRepaintAsync.needsFreshFlush = m_scheduledWhileWaitingForRenderer;
#else
    ASSERT(!m_forceRepaintAsync.callback);
    m_forceRepaintAsync.callback = WTF::move(callback);
    updateRenderingWithForcedRepaint();
    if (m_pendingForceRepaint)
        m_forceRepaintAsync.compositionRequestID = std::nullopt;
    else
        m_forceRepaintAsync.compositionRequestID = m_compositionRequestID;
#endif
}

void LayerTreeHost::sizeDidChange()
{
    m_pendingResize = true;
    if (m_isWaitingForRenderer)
        scheduleRenderingUpdate();
    else {
        cancelRenderingUpdate();
        flushLayers();
    }
}

void LayerTreeHost::pauseRendering()
{
    m_isSuspended = true;
    m_renderingWatchdog.stop();
    m_compositor->suspend();
}

void LayerTreeHost::resumeRendering()
{
    m_isSuspended = false;
    m_compositor->resume();
    m_renderingWatchdog.startRepeating(1_s);
    scheduleRenderingUpdate();
}

void LayerTreeHost::dispatchAfterCurrentFrame(Function<void()>&& function)
{
    if (auto* runLoop = m_compositor->runLoop())
        runLoop->dispatch(WTF::move(function));
}

GraphicsLayerFactory* LayerTreeHost::graphicsLayerFactory()
{
    return this;
}

FloatRect LayerTreeHost::visibleContentsRect() const
{
    if (auto* localMainFrameView = m_webPage.localMainFrameView())
        return FloatRect({ }, localMainFrameView->sizeForVisibleContent(ScrollableArea::VisibleContentRectIncludesScrollbars::Yes));
    return m_webPage.bounds();
}

void LayerTreeHost::backgroundColorDidChange()
{
    m_compositor->backgroundColorDidChange();
}

void LayerTreeHost::attachLayer(CoordinatedPlatformLayer& layer)
{
#if ENABLE(DAMAGE_TRACKING)
    layer.setDamagePropagationEnabled(webPage().corePage()->settings().propagateDamagingInformation());
    if (m_damageInGlobalCoordinateSpace)
        layer.setDamageInGlobalCoordinateSpace(m_damageInGlobalCoordinateSpace);
#endif
    m_sceneState->addLayer(layer);
}

void LayerTreeHost::detachLayer(CoordinatedPlatformLayer& layer)
{
    m_sceneState->removeLayer(layer);
}

void LayerTreeHost::notifyCompositionRequired()
{
#if ENABLE(SCROLLING_THREAD)
    if (ScrollingThread::isCurrentThread()) {
        m_compositionRequiredInScrollingThread = true;
        return;
    }
#endif
    m_compositionRequired = true;
}

bool LayerTreeHost::isCompositionRequiredOrOngoing() const
{
    return m_compositionRequired || m_forceFrameSync || m_compositor->isActive();
}

void LayerTreeHost::requestComposition(CompositionReason)
{
#if ENABLE(SCROLLING_THREAD)
    if (ScrollingThread::isCurrentThread()) {
        m_sceneState->flushPendingState();
        if (!m_compositionRequiredInScrollingThread)
            return;
        m_compositionRequiredInScrollingThread = false;
        Lumia::timelineMark('r');
        // Scroll frames wait for the display tick: rendering the moment a pan arrives puts frames between refreshes,
        // where the screen shows some twice and skips others (the stutter under the finger).
        m_compositor->requestCompositionAtNextDisplayRefresh();
        return;
    }
#endif

    Lumia::timelineMark('m');
    m_compositor->scheduleUpdate();
}

RunLoop* LayerTreeHost::compositingRunLoop() const
{
    return m_compositor->runLoop();
}

void LayerTreeHost::willPaintTile()
{
    m_sceneState->willPaintTile();
}

void LayerTreeHost::didPaintTile()
{
    m_sceneState->didPaintTile();
    // At most one frame per display refresh however many tiles finish in it; the frame applies every tile that is
    // ready and leaves the rest queued (CoordinatedBackingStoreTile::processPendingUpdates).
    if (RefPtr compositor = m_compositor)
        compositor->requestCompositionAtNextDisplayRefresh();
}

int LayerTreeHost::maxTextureSize() const
{
    return m_compositor->maxTextureSize();
}

#if USE(CAIRO)
Cairo::PaintingEngine& LayerTreeHost::paintingEngine()
{
    return *m_paintingEngine;
}
#endif

Ref<CoordinatedImageBackingStore> LayerTreeHost::imageBackingStore(Ref<NativeImage>&& nativeImage)
{
    auto nativeImageID = nativeImage->uniqueID();
    auto addResult = m_imageBackingStores.ensure(nativeImageID, [&] {
        return CoordinatedImageBackingStore::create(WTF::move(nativeImage));
    });
    return addResult.iterator->value;
}

Ref<GraphicsLayer> LayerTreeHost::createGraphicsLayer(GraphicsLayer::Type layerType, GraphicsLayerClient& client)
{
    return adoptRef(*new GraphicsLayerCoordinated(layerType, client, CoordinatedPlatformLayer::create(*this)));
}

#if !HAVE(DISPLAY_LINK)
RefPtr<DisplayRefreshMonitor> LayerTreeHost::createDisplayRefreshMonitor(PlatformDisplayID displayID)
{
    ASSERT_UNUSED(displayID, m_displayID == displayID);
    return Ref { m_compositor->displayRefreshMonitor() };
}

void LayerTreeHost::requestDisplayRefreshMonitorUpdate()
{
    // Flush layers to cause a repaint. If m_isWaitingForRenderer was true at this point, the layer
    // flush won't do anything, but that means there's a painting ongoing that will send the
    // display refresh notification when it's done.
    m_forceFrameSync = true;
    scheduleRenderingUpdate();
}

void LayerTreeHost::handleDisplayRefreshMonitorUpdate(bool hasBeenRescheduled)
{
    // Call renderNextFrame. If hasBeenRescheduled is true, the layer flush will force a repaint
    // that will cause the display refresh notification to come.
    renderNextFrame(hasBeenRescheduled);
}
#endif

#if HAVE(DISPLAY_LINK)
void LayerTreeHost::didComposite(uint32_t compositionResponseID)
{
    WTFBeginSignpost(this, DidComposite, "compositionRequestID %i, compositionResponseID %i", m_compositionRequestID, compositionResponseID);

    if (m_forceRepaintAsync.callback && m_forceRepaintAsync.compositionRequestID && compositionResponseID >= *m_forceRepaintAsync.compositionRequestID) {
        m_forceRepaintAsync.callback();
        m_forceRepaintAsync.compositionRequestID = std::nullopt;
    }

    if (!m_isWaitingForRenderer || m_compositionRequestID == compositionResponseID) {
        m_isWaitingForRenderer = false;
        bool scheduledWhileWaitingForRenderer = std::exchange(m_scheduledWhileWaitingForRenderer, false);
        if (m_pendingForceRepaint) {
            if (m_layerTreeStateIsFrozen) {
                if (m_forceRepaintAsync.callback) {
                    m_forceRepaintAsync.callback();
                    m_forceRepaintAsync.compositionRequestID = std::nullopt;
                }
            } else {
                updateRenderingWithForcedRepaint();
                if (m_forceRepaintAsync.callback)
                    m_forceRepaintAsync.compositionRequestID = m_compositionRequestID;
            }
        } else if (!m_isSuspended && !m_layerTreeStateIsFrozen && (scheduledWhileWaitingForRenderer || m_layerFlushTimer.isActive())) {
            if (!deferForLoadPacing()) {
                cancelRenderingUpdate();
                flushLayers();
            }
        }
    }
    WTFEndSignpost(this, DidComposite);
}
#endif

void LayerTreeHost::commitSceneState()
{
    m_lastCommitTime = MonotonicTime::now();
    m_isWaitingForRenderer = true;
    m_compositionRequestID = m_compositor->requestComposition();
    WTFEmitSignpost(this, CommitSceneState, "compositionRequestID %i", m_compositionRequestID);
}

#if !HAVE(DISPLAY_LINK)
void LayerTreeHost::renderNextFrame(bool forceRepaint)
{
    WTFBeginSignpost(this, RenderNextFrame);

    m_isWaitingForRenderer = false;
    bool scheduledWhileWaitingForRenderer = std::exchange(m_scheduledWhileWaitingForRenderer, false);

    if (m_forceRepaintAsync.callback) {
        // If the asynchronous force-repaint needs a separate fresh flush, it was due to
        // the force-repaint request being registered while CoordinatedLayerTreeHost was
        // waiting for the renderer to finish an update.
        ASSERT(!m_forceRepaintAsync.needsFreshFlush || scheduledWhileWaitingForRenderer);

        // Execute the callback if another layer flush and the subsequent state update
        // aren't needed. If they are, the callback will be executed when this function
        // is called after the next update.
        if (!m_forceRepaintAsync.needsFreshFlush)
            m_forceRepaintAsync.callback();
        m_forceRepaintAsync.needsFreshFlush = false;
    }

    if ((scheduledWhileWaitingForRenderer || m_layerFlushTimer.isActive()) && !forceRepaint && deferForLoadPacing()) {
        WTFEndSignpost(this, RenderNextFrame);
        return;
    }

    if (scheduledWhileWaitingForRenderer || m_layerFlushTimer.isActive() || forceRepaint) {
        m_layerFlushTimer.stop();
        if (forceRepaint)
            m_forceFrameSync = true;
        layerFlushTimerFired();
    }

    WTFEndSignpost(this, RenderNextFrame);
}
#endif

#if ENABLE(DAMAGE_TRACKING)
void LayerTreeHost::notifyFrameDamageForTesting(Region&& damageRegion)
{
    Locker locker { m_frameDamageHistoryForTestingLock };
    m_frameDamageHistoryForTesting.append(WTF::move(damageRegion));
}

void LayerTreeHost::resetDamageHistoryForTesting()
{
    {
        Locker locker { m_frameDamageHistoryForTestingLock };
        m_frameDamageHistoryForTesting.clear();
    }
    m_compositor->enableFrameDamageNotificationForTesting();
}

void LayerTreeHost::foreachRegionInDamageHistoryForTesting(Function<void(const Region&)>&& callback)
{
    Locker locker { m_frameDamageHistoryForTestingLock };
    for (const auto& region : m_frameDamageHistoryForTesting)
        callback(region);
}
#endif

} // namespace WebKit

#endif // USE(COORDINATED_GRAPHICS)
