#include "config.h"
#include "FrameStats.h"

#include "Engine.h"
#include <algorithm>
#include <cmath>

namespace WebKit {

namespace {

constexpr Seconds summaryInterval = 2_s;
// A frame later than this after the previous one starts a new burst (page idle, nothing to draw): it says nothing
// about pacing, so it is counted apart instead of as a 15 s frame.
constexpr double pauseMs = 250;
constexpr double slowMs = 50;
constexpr double vsyncMs = 1000.0 / 60;
constexpr unsigned maxSlowLines = 6;

} // namespace

void FrameStats::Histogram::add(double ms)
{
    count++;
    max = std::max(max, ms);
    auto bin = static_cast<unsigned>(std::max(0.0, ms) * binsPerMs);
    bins[std::min(bin, binCount)]++;
}

double FrameStats::Histogram::quantile(double q) const
{
    if (!count)
        return 0;
    auto rank = static_cast<unsigned>(std::ceil(q * count));
    unsigned seen = 0;
    for (unsigned bin = 0; bin < binCount; ++bin) {
        seen += bins[bin];
        if (seen >= rank)
            return (bin + 0.5) / binsPerMs;
    }
    return max;
}

void FrameStats::frameRendered(MonotonicTime paintStart, MonotonicTime paintEnd, MonotonicTime swapEnd)
{
    if (!Lumia::traceEnabled())
        return;

    ++m_frameNumber;
    if (!m_windowStart)
        m_windowStart = swapEnd;

    double paint = (paintEnd - paintStart).milliseconds();
    double swap = (swapEnd - paintEnd).milliseconds();
    m_paint.add(paint);
    m_swap.add(swap);

    if (m_previousFrame) {
        double interval = (swapEnd - m_previousFrame).milliseconds();
        if (interval >= pauseMs)
            m_afterPause++;
        else {
            m_interval.add(interval);
            // A frame counts as one refresh only up to 15% over 16.7 ms: rounding called a steady 20.8 ms (48 fps)
            // "vsync 1" and hid that scrolling ran below 60 Hz.
            m_vsyncs[interval <= 1.15 * vsyncMs ? 0 : interval <= 2.15 * vsyncMs ? 1 : 2]++;
            if (interval >= slowMs)
                m_over50++;
            // The individual lines are for attribution: a slow frame is slow in paint, in swap, or between frames
            // (the compositor was not scheduled), and only these three numbers together tell which.
            if ((interval >= slowMs || paint >= 2 * vsyncMs) && m_slowLines < maxSlowLines) {
                m_slowLines++;
                Lumia::log("compositor: fotogramma lento %u, %.1f ms dal precedente, pittura %.1f, swap %.1f",
                    m_frameNumber, interval, paint, swap);
            }
        }
    }
    m_previousFrame = swapEnd;

    if (swapEnd - m_windowStart >= summaryInterval)
        report(swapEnd);
}

void FrameStats::report(MonotonicTime now)
{
    unsigned paced = m_vsyncs[0] + m_vsyncs[1] + m_vsyncs[2];
    auto percent = [&](unsigned n) { return paced ? static_cast<unsigned>(std::lround(100.0 * n / paced)) : 0u; };
    Lumia::log("compositor: %.1f s, %u fotogrammi, intervallo mediana %.1f p90 %.1f max %.1f ms, vsync 1/2/3+ %u/%u/%u%%, "
        "oltre 50 ms %u, pittura mediana %.1f p90 %.1f max %.1f, swap mediana %.1f p90 %.1f max %.1f, dopo pausa %u, tick %u, "
        "aggiornamento mediana %.1f p90 %.1f, disegno mediana %.1f p90 %.1f, chiamate di disegno media %.0f max %u, "
        "upload %.1f per fotogramma %.0f KB, tempo upload mediana %.1f p90 %.1f max %.1f",
        (now - m_windowStart).seconds(), m_paint.count, m_interval.quantile(0.5), m_interval.quantile(0.9), m_interval.max,
        percent(m_vsyncs[0]), percent(m_vsyncs[1]), percent(m_vsyncs[2]), m_over50,
        m_paint.quantile(0.5), m_paint.quantile(0.9), m_paint.max, m_swap.quantile(0.5), m_swap.quantile(0.9), m_swap.max,
        m_afterPause, m_ticks, m_update.quantile(0.5), m_update.quantile(0.9), m_draw.quantile(0.5), m_draw.quantile(0.9),
        m_breakdowns ? double(m_drawCalls) / m_breakdowns : 0.0, m_maxDrawCalls,
        m_breakdowns ? double(m_uploads) / m_breakdowns : 0.0, m_breakdowns ? m_uploadBytes / 1024.0 / m_breakdowns : 0.0,
        m_uploadMs.quantile(0.5), m_uploadMs.quantile(0.9), m_uploadMs.max);

    m_windowStart = now;
    m_interval = { };
    m_paint = { };
    m_swap = { };
    m_update = { };
    m_draw = { };
    m_uploadMs = { };
    m_drawCalls = 0;
    m_maxDrawCalls = 0;
    m_uploads = 0;
    m_uploadBytes = 0;
    m_breakdowns = 0;
    m_vsyncs = { };
    m_over50 = 0;
    m_afterPause = 0;
    m_slowLines = 0;
    m_ticks = 0;
}

} // namespace WebKit
