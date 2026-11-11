#include "config.h"
#include "ScrollingTreeDispatcher.h"

#include "Engine.h"
#include "WebPage.h"
#include <WebCore/AsyncScrollingCoordinator.h>
#include <WebCore/IntPoint.h>
#include <WebCore/PlatformWheelEvent.h>
#include <WebCore/ScrollingCoordinatorTypes.h>
#include <WebCore/ScrollingThread.h>
#include <WebCore/ScrollingTreeCoordinated.h>
#include <WebCore/ThreadedScrollingTree.h>
#include <wtf/RunLoop.h>
#include <algorithm>
#include <cmath>

namespace WebKit {
using namespace WebCore;

namespace {

// PlatformWheelEvent keeps the gesture phase and the velocity protected: every platform's native event factory sets
// them, and a finger pan is exactly a precise wheel gesture (WebKit's kinetic scrolling, rubber banding and scroll
// snapping are all written for that shape of event). The port supplies them here.
class PanWheelEvent final : public PlatformWheelEvent {
public:
    using PlatformWheelEvent::PlatformWheelEvent;

    void setGesturePhase(PlatformWheelEventPhase phase) { m_phase = phase; }
    void setGestureMomentumPhase(PlatformWheelEventPhase phase) { m_momentumPhase = phase; }
    void setGestureVelocity(const FloatSize& velocity) { m_scrollingVelocity = velocity; }
    void setGestureTimestamp(MonotonicTime time) { m_timestamp = time; }
};

PlatformWheelEventPhase gesturePhase(int phase)
{
    switch (phase) {
    case LW_PHASE_BEGIN:
        return PlatformWheelEventPhase::Began;
    case LW_PHASE_END:
        return PlatformWheelEventPhase::Ended;
    case LW_PHASE_CANCEL:
        return PlatformWheelEventPhase::Cancelled;
    case LW_PHASE_UPDATE:
    default:
        return PlatformWheelEventPhase::Changed;
    }
}

} // namespace

ScrollingTreeDispatcher& ScrollingTreeDispatcher::singleton()
{
    static NeverDestroyed<ScrollingTreeDispatcher> dispatcher;
    return dispatcher;
}

void ScrollingTreeDispatcher::addScrollingTreeForView(lw_view& view, WebPage& webPage)
{
    RefPtr scrollingCoordinator = dynamicDowncast<AsyncScrollingCoordinator>(webPage.corePage()->scrollingCoordinator());
    if (!scrollingCoordinator)
        return;
    ScrollingTree* tree = scrollingCoordinator->scrollingTree();
    if (!tree)
        return;
    Ref scrollingTree = downcast<ThreadedScrollingTree>(*tree);

    Locker locker { m_scrollingTreesLock };
    m_scrollingTrees.set(&view, WTF::move(scrollingTree));
}

void ScrollingTreeDispatcher::removeScrollingTreeForView(lw_view& view)
{
    Locker locker { m_scrollingTreesLock };
    m_scrollingTrees.remove(&view);
}

bool ScrollingTreeDispatcher::notifyScrollingTreesDisplayDidRefresh(PlatformDisplayID displayID)
{
    bool active = false;
    Locker locker { m_scrollingTreesLock };
    for (Ref scrollingTree : m_scrollingTrees.values()) {
        active |= scrollingTree->hasRecentActivity();
        scrollingTree->displayDidRefresh(displayID);
    }
    return active;
}

void ScrollingTreeDispatcher::recordPanDelay(int phase, double delayMs, MonotonicTime fingerTime)
{
    // Input thread only. One line per gesture: a line per finger move (60-100 a second) cost the input thread a
    // formatted string and a file write each, for numbers that only matter as a distribution.
    if (phase == LW_PHASE_BEGIN) {
        m_panDelays.shrink(0);
        m_panIntervals.shrink(0);
        m_panStartTime = fingerTime;
    } else if (m_panPreviousTime && m_panIntervals.size() < 4096)
        m_panIntervals.append(static_cast<float>((fingerTime - m_panPreviousTime).milliseconds()));
    m_panPreviousTime = fingerTime;
    if (m_panDelays.size() < 4096)
        m_panDelays.append(static_cast<float>(delayMs));
    if (phase != LW_PHASE_END && phase != LW_PHASE_CANCEL)
        return;
    if (m_panDelays.isEmpty())
        return;
    std::sort(m_panDelays.begin(), m_panDelays.end());
    auto at = [&](double q) { return m_panDelays[std::min<size_t>(m_panDelays.size() - 1, static_cast<size_t>(q * m_panDelays.size()))]; };
    float intervalMedian = 0, intervalP90 = 0;
    if (!m_panIntervals.isEmpty()) {
        std::sort(m_panIntervals.begin(), m_panIntervals.end());
        intervalMedian = m_panIntervals[m_panIntervals.size() / 2];
        intervalP90 = m_panIntervals[std::min<size_t>(m_panIntervals.size() - 1, m_panIntervals.size() * 9 / 10)];
    }
    Lumia::log("lw pan: gesto di %u eventi in %.0f ms%s, ritardo dal dito mediana %.1f p90 %.1f max %.1f ms, tra eventi mediana %.1f p90 %.1f ms",
        static_cast<unsigned>(m_panDelays.size()), (fingerTime - m_panStartTime).milliseconds(),
        phase == LW_PHASE_CANCEL ? " (annullato)" : "", at(0.5), at(0.9), m_panDelays.last(), intervalMedian, intervalP90);
    m_panDelays.shrink(0);
    m_panIntervals.shrink(0);
    m_panPreviousTime = { };
}

void ScrollingTreeDispatcher::handlePan(lw_view& view, int phase, FloatPoint position, FloatSize delta, MonotonicTime time, unsigned touchGeneration)
{
    RefPtr<ThreadedScrollingTree> scrollingTree;
    {
        Locker locker { m_scrollingTreesLock };
        scrollingTree = m_scrollingTrees.get(&view);
    }
    if (!scrollingTree)
        return;

    Lumia::timelineMark('p');
    if (Lumia::traceEnabled())
        recordPanDelay(phase, (MonotonicTime::now() - time).milliseconds(), time);

    // Everything below runs on the scrolling thread, which already renders the fling in step with the display.
    ScrollingThread::dispatch([this, tree = scrollingTree.releaseNonNull(), pan = Pan { phase, position, delta, time }, touchGeneration] {
        Lumia::timelineMark('q');
        gatePan(tree.get(), pan, touchGeneration);
    });
}

void ScrollingTreeDispatcher::applyPan(ThreadedScrollingTree& tree, const Pan& pan)
{
    // WebCore's wheel deltas are reversed from the scrolling direction: positive deltaY scrolls the content down
    // (the scroll offset decreases), which is exactly where a finger going down goes. The finger's own deltas are
    // therefore passed through unchanged.
    IntPoint position(static_cast<int>(std::lround(pan.position.x())), static_cast<int>(std::lround(pan.position.y())));
    PanWheelEvent event(position, position, pan.delta.width(), pan.delta.height(), 0, 0,
        PlatformWheelEventGranularity::ScrollByPixelWheelEvent, false, false, false, false);
    event.setHasPreciseScrollingDeltas(true);
    event.setGesturePhase(gesturePhase(pan.phase));
    event.setGestureMomentumPhase(PlatformWheelEventPhase::None);
    // The fling velocity is computed by ScrollAnimationKinetic from the deltas and the finger's timestamps.
    event.setGestureTimestamp(pan.time);
    // Only the asynchronous scroll: the page's touch listeners get the finger separately (lw_input_touch), so a busy
    // JS thread can never delay it.
    tree.willProcessWheelEvent();
    tree.handleWheelEvent(event, { WheelEventProcessingSteps::AsyncScrolling });
    // The Web thread may be busy (a long script) and not run its rendering update: apply the new scroll position to
    // the coordinated layers right here, which also asks the compositor for a frame. Without this the page only
    // moves when the Web thread is free again.
    tree.applyLayerPositions();
}

void ScrollingTreeDispatcher::gatePan(ThreadedScrollingTree& tree, const Pan& pan, unsigned touchGeneration)
{
    auto& gate = m_panGate;
    if (!touchGeneration || touchGeneration != gate.generation) {
        // No touch gesture for this pan (or its regions were not read): nothing to wait for.
        applyPan(tree, pan);
        return;
    }

    bool ends = pan.phase == LW_PHASE_END || pan.phase == LW_PHASE_CANCEL;
    if (pan.phase == LW_PHASE_BEGIN) {
        gate.tree = &tree;
        gate.start = pan.position;
        gate.held.clear();
        auto actions = gate.touchActions;
        bool anyPan = actions.containsAny({ TouchAction::Auto, TouchAction::Manipulation });
        bool panX = anyPan || actions.contains(TouchAction::PanX);
        bool panY = anyPan || actions.contains(TouchAction::PanY);
        gate.axis = PanGate::Axis::Both;
        gate.waitForDirection = panX != panY;
        gate.dropped = gate.pageOwns || (!panX && !panY);
        if (gate.dropped && Lumia::traceEnabled())
            Lumia::log("touch: niente scorrimento (%s)", gate.pageOwns ? "la pagina ha preso il gesto" : "touch-action");
    }
    if (gate.dropped)
        return;

    if (gate.waitForDirection && pan.phase == LW_PHASE_UPDATE) {
        // touch-action: pan-x or pan-y alone. The first move decides: along the allowed axis the pan scrolls on that
        // axis only, across it the gesture is the page's.
        auto total = pan.position - gate.start;
        bool horizontal = std::abs(total.width()) > std::abs(total.height());
        bool allowsX = gate.touchActions.contains(TouchAction::PanX);
        gate.waitForDirection = false;
        if (horizontal != allowsX) {
            gate.dropped = true;
            gate.held.clear();
            return;
        }
        gate.axis = allowsX ? PanGate::Axis::Horizontal : PanGate::Axis::Vertical;
    }

    if (gate.waiting()) {
        if (gate.held.isEmpty() && (gate.waitForStart || gate.waitForMove)) {
            // A page that does not answer in time gets the scroll anyway: a busy script must not freeze the finger.
            RunLoop::currentSingleton().dispatchAfter(100_ms, [this, generation = gate.generation] {
                auto& gate = m_panGate;
                if (gate.generation != generation || gate.dropped || !(gate.waitForStart || gate.waitForMove))
                    return;
                Lumia::log("touch: la pagina non ha risposto in 100 ms, scorre");
                gate.waitForStart = gate.waitForMove = false;
                releaseHeldPans();
            });
        }
        // Moves beyond a few seconds of them merge into the last one: same distance, fewer events.
        if (gate.held.size() >= 256 && pan.phase == LW_PHASE_UPDATE && gate.held.last().phase == LW_PHASE_UPDATE) {
            auto& last = gate.held.last();
            last.delta += pan.delta;
            last.position = pan.position;
            last.time = pan.time;
        } else
            gate.held.append(pan);
        return;
    }

    if (!gate.held.isEmpty()) {
        gate.held.append(pan);
        releaseHeldPans();
        return;
    }

    Pan filtered = pan;
    if (gate.axis == PanGate::Axis::Horizontal)
        filtered.delta.setHeight(0);
    else if (gate.axis == PanGate::Axis::Vertical)
        filtered.delta.setWidth(0);
    applyPan(tree, filtered);
    if (ends)
        gate.tree = nullptr;
}

void ScrollingTreeDispatcher::releaseHeldPans()
{
    auto& gate = m_panGate;
    if (gate.waiting() || gate.dropped || !gate.tree)
        return;
    auto held = std::exchange(gate.held, { });
    Ref tree = *gate.tree;
    for (auto& pan : held) {
        Pan filtered = pan;
        if (gate.axis == PanGate::Axis::Horizontal)
            filtered.delta.setHeight(0);
        else if (gate.axis == PanGate::Axis::Vertical)
            filtered.delta.setWidth(0);
        applyPan(tree.get(), filtered);
        if (pan.phase == LW_PHASE_END || pan.phase == LW_PHASE_CANCEL)
            gate.tree = nullptr;
    }
}

void ScrollingTreeDispatcher::beginTouchGesture(lw_view& view, unsigned generation, FloatPoint position)
{
    RefPtr<ThreadedScrollingTree> scrollingTree;
    {
        Locker locker { m_scrollingTreesLock };
        scrollingTree = m_scrollingTrees.get(&view);
    }
    if (!scrollingTree)
        return;
    ScrollingThread::dispatch([this, tree = scrollingTree.releaseNonNull(), generation, position] {
        auto& gate = m_panGate;
        gate = { };
        gate.generation = generation;
        if (auto* coordinatedTree = dynamicDowncast<ScrollingTreeCoordinated>(tree.get())) {
            // Region lookup only (the layers' event regions, under their locks): no page code, no Web thread.
            auto info = coordinatedTree->touchRegionInfoForPoint(position);
            gate.waitForStart = info.touchStart == TrackingType::Synchronous;
            gate.waitForMove = info.touchMove == TrackingType::Synchronous;
            gate.listeners = info.touchStart != TrackingType::NotTracking || info.touchMove != TrackingType::NotTracking;
            gate.touchActions = info.touchActions;
            if (Lumia::traceEnabled() && (gate.listeners || !gate.touchActions.contains(TouchAction::Auto))) {
                Lumia::log("touch: sotto il dito ascoltatori %s, attende touchstart %d, touchmove %d, touch-action %02X",
                    gate.listeners ? "si" : "no", gate.waitForStart, gate.waitForMove, gate.touchActions.toRaw());
            }
        }
        publishTouchPolicy();
    });
}

void ScrollingTreeDispatcher::pageAnsweredTouch(unsigned generation, TouchAnswer answer, bool cancelled)
{
    ScrollingThread::dispatch([this, generation, answer, cancelled] {
        auto& gate = m_panGate;
        if (gate.generation != generation)
            return;
        bool scrolling = gate.tree && !gate.waiting() && gate.held.isEmpty() && !gate.dropped;
        if (answer == TouchAnswer::Start)
            gate.waitForStart = false;
        else
            gate.waitForMove = false;
        if (cancelled && !scrolling) {
            // Too late once the scroll has started (the page then gets the rest of the gesture as it goes).
            gate.pageOwns = true;
            gate.dropped = true;
            gate.held.clear();
        }
        publishTouchPolicy();
        releaseHeldPans();
    });
}

namespace {
enum : uint32_t {
    PolicyKnown = 1 << 0,
    PolicyListeners = 1 << 1,
    PolicyPinchZoom = 1 << 2,
    PolicyDoubleTapZoom = 1 << 3,
    PolicyPageOwns = 1 << 4,
};
}

void ScrollingTreeDispatcher::publishTouchPolicy()
{
    auto& gate = m_panGate;
    auto actions = gate.touchActions;
    uint32_t flags = PolicyKnown;
    if (gate.listeners)
        flags |= PolicyListeners;
    if (!gate.pageOwns && actions.containsAny({ TouchAction::Auto, TouchAction::Manipulation, TouchAction::PinchZoom }))
        flags |= PolicyPinchZoom;
    if (touchActionAllowsDoubleTapToZoom(actions))
        flags |= PolicyDoubleTapZoom;
    if (gate.pageOwns)
        flags |= PolicyPageOwns;
    m_touchPolicy.store(gate.generation << 8 | flags, std::memory_order_release);
}

auto ScrollingTreeDispatcher::touchPolicy(unsigned generation) const -> TouchPolicy
{
    uint32_t value = m_touchPolicy.load(std::memory_order_acquire);
    if ((value >> 8) != (generation & 0xFFFFFF) || !(value & PolicyKnown))
        return { };
    return { true, !!(value & PolicyListeners), !!(value & PolicyPinchZoom), !!(value & PolicyDoubleTapZoom), !!(value & PolicyPageOwns) };
}

} // namespace WebKit
