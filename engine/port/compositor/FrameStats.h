// Frame pacing as the compositor sees it, summarised every 2 s instead of one log line per frame: 60 lines a second
// from the compositor thread were a cost of their own (formatting, the app's file write, the 2 s upload), and an idle
// gap between two frames counted as a "frame" of 15 s. Compositor thread only.
//
// One summary line per window:
//   compositor: 2.0 s, 118 fotogrammi, intervallo mediana 16.8 p90 17.3 max 33.8 ms, vsync 1/2/3+ 97/3/0%,
//               oltre 50 ms 0, pittura mediana 8.3 p90 9.3 max 12.0, swap mediana 7.3 p90 8.3 max 9.8, dopo pausa 1,
//               tick 121
// plus one "fotogramma lento" line per janky frame (at most six per window). tools/trace.py reads both.
#pragma once

#include <algorithm>
#include <array>
#include <wtf/MonotonicTime.h>

namespace WebKit {

class FrameStats {
public:
    void frameRendered(MonotonicTime paintStart, MonotonicTime paintEnd, MonotonicTime swapEnd);
    // One tick of the frame clock (ThreadedCompositor::displayUpdateFired): reported next to the frames, so a still
    // page shows as few ticks and a moving one as about one tick per frame.
    void clockTicked() { m_ticks++; }
    // Where the paint time of the frame about to be reported went: applying the scene update (layer changes and the
    // texture uploads of newly painted tiles) and drawing the layer tree. Called before frameRendered.
    // Also what the frame cost the GPU driver: draw calls, and tile texture uploads (count, bytes, time in the upload
    // call itself), all reset for the next frame.
    void paintBreakdown(double updateMs, double drawMs, unsigned drawCalls, unsigned uploads, uint64_t uploadBytes, double uploadMs)
    {
        m_update.add(updateMs);
        m_draw.add(drawMs);
        m_drawCalls += drawCalls;
        m_maxDrawCalls = std::max(m_maxDrawCalls, drawCalls);
        m_uploads += uploads;
        m_uploadBytes += uploadBytes;
        m_uploadMs.add(uploadMs);
        m_breakdowns++;
    }

private:
    // 0.5 ms bins up to 100 ms: fine enough to tell a 16.7 ms frame from a 20.9 ms one, which is the whole question
    // when frames miss vsync. Longer values only count towards the maximum and the "over 50 ms" figure.
    struct Histogram {
        static constexpr unsigned binsPerMs = 2;
        static constexpr unsigned binCount = 100 * binsPerMs;
        std::array<unsigned, binCount + 1> bins { };
        unsigned count { 0 };
        double max { 0 };

        void add(double ms);
        double quantile(double) const;
    };

    void report(MonotonicTime now);

    MonotonicTime m_windowStart;
    MonotonicTime m_previousFrame;
    unsigned m_frameNumber { 0 };
    Histogram m_interval;
    Histogram m_paint;
    Histogram m_swap;
    Histogram m_update;
    Histogram m_draw;
    Histogram m_uploadMs;
    uint64_t m_drawCalls { 0 };
    unsigned m_maxDrawCalls { 0 };
    uint64_t m_uploads { 0 };
    uint64_t m_uploadBytes { 0 };
    unsigned m_breakdowns { 0 };
    std::array<unsigned, 3> m_vsyncs { };
    unsigned m_over50 { 0 };
    unsigned m_afterPause { 0 };
    unsigned m_slowLines { 0 };
    unsigned m_ticks { 0 };
};

} // namespace WebKit
