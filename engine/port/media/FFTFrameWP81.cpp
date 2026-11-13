// WebCore's FFTFrame (Web Audio: convolution, the analyser, periodic waves, HRTF) over a transform of our own. WebCore
// has one for Accelerate and one for GStreamer; the stub it builds anywhere else does nothing.
// Sizes are powers of two. The layout is the GStreamer port's: fftSize / 2 + 1 bins, real and imaginary apart, the
// forward transform unscaled and the inverse scaled by 1 / fftSize.

#include "config.h"

#if ENABLE(WEB_AUDIO)

#include "FFTFrame.h"

#include "VectorMath.h"
#include <array>
#include <cmath>
#include <mutex>
#include <numbers>
#include <wtf/Lock.h>
#include <wtf/MathExtras.h>
#include <wtf/StdLibExtras.h>

namespace WebCore {

namespace {

constexpr unsigned minFFTPow2Size = 2;
constexpr unsigned maxFFTPow2Size = 24;

// A real transform of n points through a complex one of n / 2 (the even samples as real parts, the odd ones as
// imaginary), then one pass that untangles the two halves: half the work of a complex transform of n.
struct Tables {
    unsigned half { 0 };            // n / 2: the size of the complex transform
    Vector<uint32_t> reversed;      // bit reversal for `half` points
    Vector<float> cosine, sine;     // e^(-2 pi i k / half), k < half / 2: the complex transform's twiddles
    Vector<float> postCosine, postSine; // e^(-2 pi i k / n), k <= half / 2... k < half: the untangling pass
};

const Tables& tablesFor(unsigned log2Size)
{
    static Lock lock;
    static std::array<std::unique_ptr<Tables>, maxFFTPow2Size + 1> tables;
    Locker locker { lock };
    auto& slot = tables[log2Size];
    if (slot)
        return *slot;

    auto created = std::make_unique<Tables>();
    unsigned n = 1u << log2Size;
    unsigned half = n / 2;
    created->half = half;
    unsigned bits = log2Size - 1;
    created->reversed.grow(half);
    for (unsigned i = 0; i < half; ++i) {
        unsigned r = 0;
        for (unsigned b = 0; b < bits; ++b) {
            if (i & (1u << b))
                r |= 1u << (bits - 1 - b);
        }
        created->reversed[i] = r;
    }
    created->cosine.grow(std::max(half / 2, 1u));
    created->sine.grow(std::max(half / 2, 1u));
    for (unsigned k = 0; k < half / 2; ++k) {
        double angle = -2 * std::numbers::pi * k / half;
        created->cosine[k] = static_cast<float>(std::cos(angle));
        created->sine[k] = static_cast<float>(std::sin(angle));
    }
    created->postCosine.grow(half);
    created->postSine.grow(half);
    for (unsigned k = 0; k < half; ++k) {
        double angle = -2 * std::numbers::pi * k / n;
        created->postCosine[k] = static_cast<float>(std::cos(angle));
        created->postSine[k] = static_cast<float>(std::sin(angle));
    }
    slot = WTF::move(created);
    return *slot;
}

// In place, `half` complex points already in bit-reversed order. Forward: e^(-i...). Inverse: the conjugate
// twiddles, unscaled.
void complexTransform(float* re, float* im, const Tables& tables, bool inverse)
{
    unsigned n = tables.half;
    for (unsigned length = 2; length <= n; length <<= 1) {
        unsigned halfLength = length / 2;
        unsigned step = n / length;
        for (unsigned start = 0; start < n; start += length) {
            for (unsigned j = 0; j < halfLength; ++j) {
                float wr = tables.cosine[j * step];
                float wi = inverse ? -tables.sine[j * step] : tables.sine[j * step];
                unsigned a = start + j;
                unsigned b = a + halfLength;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
}

size_t unpackedFFTDataSize(unsigned fftSize)
{
    return fftSize / 2 + 1;
}

} // namespace

FFTFrame::FFTFrame(unsigned fftSize)
    : m_FFTSize(fftSize)
    , m_log2FFTSize(static_cast<unsigned>(log2(fftSize)))
    , m_realData(unpackedFFTDataSize(fftSize))
    , m_imagData(unpackedFFTDataSize(fftSize))
    , m_scratchRealWP81(fftSize / 2)
    , m_scratchImagWP81(fftSize / 2)
{
    // The tables are made here, not at the first transform on the audio thread.
    if (m_log2FFTSize >= minFFTPow2Size && m_log2FFTSize <= maxFFTPow2Size)
        tablesFor(m_log2FFTSize);
}

FFTFrame::FFTFrame()
    : m_FFTSize(0)
    , m_log2FFTSize(0)
{
}

FFTFrame::FFTFrame(const FFTFrame& frame)
    : m_FFTSize(frame.m_FFTSize)
    , m_log2FFTSize(frame.m_log2FFTSize)
    , m_realData(unpackedFFTDataSize(frame.m_FFTSize))
    , m_imagData(unpackedFFTDataSize(frame.m_FFTSize))
    , m_scratchRealWP81(frame.m_FFTSize / 2)
    , m_scratchImagWP81(frame.m_FFTSize / 2)
{
    memcpySpan(m_realData.span(), frame.realData().span());
    memcpySpan(m_imagData.span(), frame.imagData().span());
}

FFTFrame::~FFTFrame() = default;

void FFTFrame::initialize()
{
}

void FFTFrame::doFFT(std::span<const float> data)
{
    if (m_log2FFTSize < minFFTPow2Size || m_log2FFTSize > maxFFTPow2Size || data.size() < m_FFTSize)
        return;
    auto& tables = tablesFor(m_log2FFTSize);
    unsigned half = tables.half;
    auto zr = m_scratchRealWP81.span();
    auto zi = m_scratchImagWP81.span();
    // z[k] = x[2k] + i x[2k + 1], placed where the transform wants it.
    for (unsigned k = 0; k < half; ++k) {
        unsigned r = tables.reversed[k];
        zr[r] = data[2 * k];
        zi[r] = data[2 * k + 1];
    }
    complexTransform(zr.data(), zi.data(), tables, false);

    // X[k] = (Z[k] + conj(Z[half - k])) / 2 + e^(-2 pi i k / n) (Z[k] - conj(Z[half - k])) / (2i), for k <= half.
    auto real = m_realData.span();
    auto imag = m_imagData.span();
    for (unsigned k = 0; k <= half; ++k) {
        unsigned a = k % half;
        unsigned b = (half - k) % half;
        float evenRe = (zr[a] + zr[b]) / 2;
        float evenIm = (zi[a] - zi[b]) / 2;
        // (Z[a] - conj(Z[b])) / (2i) = ((zi[a] + zi[b]) - i (zr[a] - zr[b])) / 2
        float oddRe = (zi[a] + zi[b]) / 2;
        float oddIm = -(zr[a] - zr[b]) / 2;
        float wr = k < half ? tables.postCosine[k] : -1;
        float wi = k < half ? tables.postSine[k] : 0;
        real[k] = evenRe + oddRe * wr - oddIm * wi;
        imag[k] = evenIm + oddRe * wi + oddIm * wr;
    }
}

void FFTFrame::doInverseFFT(std::span<float> data)
{
    if (m_log2FFTSize < minFFTPow2Size || m_log2FFTSize > maxFFTPow2Size || data.size() < m_FFTSize)
        return;
    auto& tables = tablesFor(m_log2FFTSize);
    unsigned half = tables.half;
    auto real = m_realData.span();
    auto imag = m_imagData.span();
    auto zr = m_scratchRealWP81.span();
    auto zi = m_scratchImagWP81.span();
    // The pass of doFFT backwards: Z[k] = E[k] + i O[k], with E[k] = (X[k] + conj(X[half - k])) / 2 and
    // O[k] = e^(2 pi i k / n) (X[k] - conj(X[half - k])) / 2 (the spectra of the even and of the odd samples).
    for (unsigned k = 0; k < half; ++k) {
        unsigned b = half - k;
        float evenRe = (real[k] + real[b]) / 2;
        float evenIm = (imag[k] - imag[b]) / 2;
        float differenceRe = (real[k] - real[b]) / 2;
        float differenceIm = (imag[k] + imag[b]) / 2;
        float wr = tables.postCosine[k];
        float wi = -tables.postSine[k];
        float oddRe = differenceRe * wr - differenceIm * wi;
        float oddIm = differenceRe * wi + differenceIm * wr;
        unsigned r = tables.reversed[k];
        zr[r] = evenRe - oddIm;
        zi[r] = evenIm + oddRe;
    }
    complexTransform(zr.data(), zi.data(), tables, true);
    // The complex inverse lacks its 1 / half; with it a forward then an inverse transform give the data back, which
    // is the scale WebCore expects (the GStreamer port's 1 / fftSize on an unscaled inverse of fftSize points).
    float scale = 1.0f / half;
    for (unsigned k = 0; k < half; ++k) {
        data[2 * k] = zr[k] * scale;
        data[2 * k + 1] = zi[k] * scale;
    }
}

int FFTFrame::minFFTSize()
{
    return 1 << minFFTPow2Size;
}

int FFTFrame::maxFFTSize()
{
    return 1 << maxFFTPow2Size;
}

} // namespace WebCore

#endif // ENABLE(WEB_AUDIO)
