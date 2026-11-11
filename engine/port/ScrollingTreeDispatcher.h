// What reaches the pages without the Web thread, as in WebKit2's EventDispatcher: the scrolling trees of all tabs, which
// the scrolling thread drives on every display refresh and on the pans coming from the app's input thread.
#pragma once

#include <WebCore/FloatPoint.h>
#include <WebCore/FloatSize.h>
#include <WebCore/PlatformScreen.h>
#include <WebCore/TouchAction.h>
#include <atomic>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/Vector.h>

namespace WebCore {
class ThreadedScrollingTree;
}

struct lw_view; // lumiaweb.h

namespace WebKit {

class WebPage;

class ScrollingTreeDispatcher {
    WTF_MAKE_NONCOPYABLE(ScrollingTreeDispatcher);
public:
    static ScrollingTreeDispatcher& singleton();

    // Web thread, when a tab is created and closed.
    void addScrollingTreeForView(lw_view&, WebPage&);
    void removeScrollingTreeForView(lw_view&);

    // Compositing threads. True while a tree still needs display refreshes (a finger, a fling, rubber banding): the
    // compositor's frame clock keeps ticking until every tree says no.
    bool notifyScrollingTreesDisplayDidRefresh(WebCore::PlatformDisplayID);

    // Input thread: one finger pan. The first call of a gesture is LW_PHASE_BEGIN (lw_phase in lumiaweb.h), then
    // LW_PHASE_UPDATE per move and LW_PHASE_END on lift (with the fling computed by the scrolling tree from the
    // deltas and their timing) or LW_PHASE_CANCEL. Never waits for the Web thread: the work is posted to the
    // scrolling thread with the tree kept alive by the call. touchGeneration: the touch gesture the pan belongs to
    // (beginTouchGesture), 0 if none.
    void handlePan(lw_view&, int phase, WebCore::FloatPoint position, WebCore::FloatSize delta, WTF::MonotonicTime, unsigned touchGeneration);

    // Touch gestures, without waiting for the page. Input thread, first finger down: the scrolling thread reads the
    // page's event regions under the finger (touch and pointer listeners, touch-action), as committed by the last
    // rendering update, and gates the gesture's pans with them. Only a non-passive touchstart or touchmove listener
    // under the finger makes the pan wait for the page's answer, on the scrolling thread and for 100 ms at most: the
    // finger's events are held meanwhile and replayed, so the scroll then catches up with the finger.
    void beginTouchGesture(lw_view&, unsigned generation, WebCore::FloatPoint position);
    // Web thread: the page's answer to the touchstart, or to the first touchmove, of `generation`. cancelled: it
    // called preventDefault and owns the gesture (no scroll, no pinch zoom).
    enum class TouchAnswer : uint8_t { Start, Move };
    void pageAnsweredTouch(unsigned generation, TouchAnswer, bool cancelled);

    // What gesture `generation` allows, as far as the scrolling thread knows yet. Any thread, wait-free.
    struct TouchPolicy {
        bool known { false }; // the event regions under the finger have been read
        bool listeners { true }; // touch or pointer listeners under the finger: the page wants the moves
        bool pinchZoom { true }; // touch-action allows it and the page did not take the gesture
        bool doubleTapZoom { true }; // touch-action is neither manipulation nor none
        bool pageOwns { false }; // the page cancelled touchstart or the first touchmove
    };
    TouchPolicy touchPolicy(unsigned generation) const;

private:
    friend NeverDestroyed<ScrollingTreeDispatcher>;
    ScrollingTreeDispatcher() = default;

    // Tracer: finger-to-dispatch delay of every event of the current gesture, logged as one line when it ends.
    void recordPanDelay(int phase, double delayMs, MonotonicTime fingerTime);
    Vector<float> m_panDelays;
    // Time between consecutive finger events of a gesture (their own timestamps): the rate the digitizer delivers.
    Vector<float> m_panIntervals;
    MonotonicTime m_panPreviousTime;
    MonotonicTime m_panStartTime;

    struct Pan {
        int phase;
        WebCore::FloatPoint position;
        WebCore::FloatSize delta;
        WTF::MonotonicTime time;
    };
    // Scrolling thread.
    void gatePan(WebCore::ThreadedScrollingTree&, const Pan&, unsigned touchGeneration);
    void applyPan(WebCore::ThreadedScrollingTree&, const Pan&);
    void releaseHeldPans();
    void publishTouchPolicy();

    // Scrolling thread only: the current touch gesture and what its pans may do.
    struct PanGate {
        unsigned generation { 0 };
        // Non-passive listeners under the finger: the pan waits for the page's answer to these.
        bool waitForStart { false };
        bool waitForMove { false };
        bool listeners { false };
        bool pageOwns { false };
        OptionSet<WebCore::TouchAction> touchActions { WebCore::TouchAction::Auto };
        // touch-action allows one axis only: the pan's direction is known at its first move.
        bool waitForDirection { false };
        enum class Axis : uint8_t { Both, Horizontal, Vertical } axis { Axis::Both };
        WebCore::FloatPoint start;
        bool dropped { false };
        RefPtr<WebCore::ThreadedScrollingTree> tree;
        Vector<Pan> held;

        bool waiting() const { return waitForStart || waitForMove || waitForDirection; }
    };
    PanGate m_panGate;
    // PanGate as TouchPolicy: generation << 8 | flags. Written by the scrolling thread.
    std::atomic<uint32_t> m_touchPolicy { 0 };

    Lock m_scrollingTreesLock;
    HashMap<lw_view*, Ref<WebCore::ThreadedScrollingTree>> m_scrollingTrees WTF_GUARDED_BY_LOCK(m_scrollingTreesLock);
};

} // namespace WebKit
