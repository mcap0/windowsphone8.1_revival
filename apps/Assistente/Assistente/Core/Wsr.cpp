// Wsr: see Wsr.h. Every step mirrors a function of tools/wsr (named in the comments) so the parity test
// (tools/wsr/wsrtest.cpp) can compare them.
#include "Wsr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace assist {

double Wsr::Beta = 0.15;

namespace {

const int SR = 16000, WIN = 400, HOP = 160, NFFT = 512, NB = NFFT / 2 + 1, NMEL = 40, NCEP = 13;
const double PI = 3.14159265358979323846;

struct Tables {
    float ham[WIN];
    std::vector<float> mel;   // NMEL x NB
    float dct[NCEP][NMEL];
    float cosT[NFFT / 2], sinT[NFFT / 2];
    int rev[NFFT];
    Tables() : mel(NMEL * NB, 0.0f) {
        for (int n = 0; n < WIN; ++n) ham[n] = (float)(0.54 - 0.46 * std::cos(2 * PI * n / (WIN - 1)));
        // ear.mel_matrix: triangles equally spaced on the mel scale, 0..8000 Hz, peak 1
        double lo = 0.0, hi = 2595.0 * std::log10(1.0 + (SR / 2) / 700.0);
        double edges[NMEL + 2];
        for (int i = 0; i < NMEL + 2; ++i) {
            double m = lo + (hi - lo) * i / (NMEL + 1);
            edges[i] = 700.0 * (std::pow(10.0, m / 2595.0) - 1.0);
        }
        for (int i = 0; i < NMEL; ++i)
            for (int k = 0; k < NB; ++k) {
                double f = k * (double)SR / NFFT, a = edges[i], b = edges[i + 1], c = edges[i + 2];
                double v = std::min((f - a) / (b - a), (c - f) / (c - b));
                mel[i * NB + k] = (float)std::max(0.0, v);
            }
        for (int q = 0; q < NCEP; ++q)
            for (int i = 0; i < NMEL; ++i) dct[q][i] = (float)std::cos(PI * q * (i + 0.5) / NMEL);
        for (int k = 0; k < NFFT / 2; ++k) {
            cosT[k] = (float)std::cos(2 * PI * k / NFFT);
            sinT[k] = (float)-std::sin(2 * PI * k / NFFT);
        }
        int bits = 9;
        for (int i = 0; i < NFFT; ++i) {
            int r = 0;
            for (int b = 0; b < bits; ++b) r |= ((i >> b) & 1) << (bits - 1 - b);
            rev[i] = r;
        }
    }
};

const Tables& T() {
    static Tables t;
    return t;
}

// in-place radix-2 FFT of NFFT complex values
void Fft(float* re, float* im) {
    const Tables& t = T();
    for (int i = 0; i < NFFT; ++i)
        if (t.rev[i] > i) { std::swap(re[i], re[t.rev[i]]); std::swap(im[i], im[t.rev[i]]); }
    for (int len = 2; len <= NFFT; len <<= 1) {
        int half = len / 2, step = NFFT / len;
        for (int s = 0; s < NFFT; s += len)
            for (int j = 0; j < half; ++j) {
                float wr = t.cosT[j * step], wi = t.sinT[j * step];
                float xr = re[s + j + half] * wr - im[s + j + half] * wi;
                float xi = re[s + j + half] * wi + im[s + j + half] * wr;
                re[s + j + half] = re[s + j] - xr; im[s + j + half] = im[s + j] - xi;
                re[s + j] += xr; im[s + j] += xi;
            }
    }
}

int FrameCount(size_t n) {
    long long t = 1 + ((long long)n - WIN + HOP - 1) / HOP;
    if ((long long)n - WIN + HOP - 1 < 0) t = 1 + (((long long)n - WIN + HOP - 1) - (HOP - 1)) / HOP;  // floor
    return (int)std::max(1LL, t);
}

// numpy.percentile, linear
double Percentile(std::vector<float> v, double q) {
    std::sort(v.begin(), v.end());
    double pos = q / 100.0 * (v.size() - 1);
    size_t lo = (size_t)std::floor(pos);
    if (lo + 1 >= v.size()) return v.back();
    return v[lo] + (pos - lo) * (v[lo + 1] - v[lo]);
}

// dtwalign.feats + speech_span: c1..c12, mean and spread of the speech frames removed; [a, b) = speech span.
// Model features (mfccmodel.features): c1..c12 minus the speech mean, averaged over 5 frames.
void Features(const std::vector<float>& cep, const std::vector<float>& energy, std::vector<float>& dtw,
              std::vector<float>& model, int& a, int& b) {
    int n = (int)energy.size();
    double fl = std::max(Percentile(energy, 10.0), -75.0);
    std::vector<char> sp(n);
    int cnt = 0;
    a = -1; b = 0;
    for (int t = 0; t < n; ++t) {
        sp[t] = energy[t] > fl + 15;
        if (sp[t]) { ++cnt; if (a < 0) a = t; b = t + 1; }
    }
    if (a < 0) { a = 0; b = n; }
    double mean[Wsr::D] = {0}, sd[Wsr::D] = {0};
    for (int d = 0; d < Wsr::D; ++d) {
        double s = 0, s2 = 0; int m = 0;
        for (int t = 0; t < n; ++t)
            if (sp[t] || cnt == 0) { double x = cep[t * NCEP + 1 + d]; s += x; s2 += x * x; ++m; }
        mean[d] = s / m;
        sd[d] = std::sqrt(std::max(0.0, s2 / m - mean[d] * mean[d]));
    }
    dtw.assign((size_t)n * Wsr::D, 0.0f);
    model.assign((size_t)n * Wsr::D, 0.0f);
    std::vector<float> cm((size_t)n * Wsr::D);
    for (int t = 0; t < n; ++t)
        for (int d = 0; d < Wsr::D; ++d) {
            double x = cep[t * NCEP + 1 + d] - mean[d];
            cm[t * Wsr::D + d] = (float)x;
            dtw[t * Wsr::D + d] = (float)(x / (sd[d] + 1e-6));
        }
    for (int t = 0; t < n; ++t)
        for (int d = 0; d < Wsr::D; ++d) {
            double s = 0;
            for (int j = -2; j <= 2; ++j) s += cm[std::min(n - 1, std::max(0, t + j)) * Wsr::D + d];
            model[t * Wsr::D + d] = (float)(s / 5);
        }
}

// decode.cost / dtw.c: summed Euclidean distance along the best path / (n + m), Sakoe-Chiba band 0.4.
// With path != nullptr also the path (pairs i, j) by backtracking.
double Dtw(const float* A, int n, const float* B, int m, std::vector<int>* path) {
    int w = (int)(0.4 * std::max(n, m));
    w = std::max(w, std::abs(n - m) + 5);
    const double INF = 1e300;
    std::vector<double> C;
    std::vector<double> prev(m + 1, INF), cur(m + 1, INF);
    if (path) C.assign((size_t)(n + 1) * (m + 1), INF);
    prev[0] = 0;
    if (path) C[0] = 0;
    for (int i = 1; i <= n; ++i) {
        std::fill(cur.begin(), cur.end(), INF);
        int jc = (int)((long long)i * m / n), lo = std::max(1, jc - w), hi = std::min(m, jc + w);
        const float* a = A + (size_t)(i - 1) * Wsr::D;
        for (int j = lo; j <= hi; ++j) {
            const float* bb = B + (size_t)(j - 1) * Wsr::D;
            double s = 0;
            for (int k = 0; k < Wsr::D; ++k) { double t = a[k] - bb[k]; s += t * t; }
            double best = std::min(prev[j - 1], std::min(prev[j], cur[j - 1]));
            cur[j] = std::sqrt(s) + best;
            if (path) C[(size_t)i * (m + 1) + j] = cur[j];
        }
        std::swap(prev, cur);
    }
    double r = prev[m] / (n + m);
    if (path) {
        path->clear();
        int i = n, j = m;
        while (i > 0 && j > 0) {
            path->push_back(i - 1); path->push_back(j - 1);
            double d = C[(size_t)(i - 1) * (m + 1) + j - 1], u = C[(size_t)(i - 1) * (m + 1) + j],
                   l = C[(size_t)i * (m + 1) + j - 1];
            if (d <= u && d <= l) { --i; --j; } else if (u <= l) --i; else --j;
        }
        // reversed pairs
        std::vector<int> r2(path->size());
        for (size_t k = 0; k < path->size(); k += 2) {
            r2[path->size() - 2 - k] = (*path)[k];
            r2[path->size() - 1 - k] = (*path)[k + 1];
        }
        path->swap(r2);
    }
    return r;
}

double LogSumExp(const double* v, int n) {
    double m = -1e300;
    for (int i = 0; i < n; ++i) m = std::max(m, v[i]);
    double s = 0;
    for (int i = 0; i < n; ++i) s += std::exp(v[i] - m);
    return m + std::log(s);
}

template <class T> T Read(const std::string& b, size_t& p, bool& ok) {
    T v{};
    if (p + sizeof(T) > b.size()) { ok = false; return v; }
    std::memcpy(&v, b.data() + p, sizeof(T));
    p += sizeof(T);
    return v;
}

}  // namespace

void WsrCepstra(const int16_t* pcm, size_t count, std::vector<float>& cep, std::vector<float>& energy) {
    const Tables& t = T();
    int n = FrameCount(count);
    cep.assign((size_t)n * NCEP, 0.0f);
    energy.assign(n, 0.0f);
    std::vector<float> x(count), y(count);
    for (size_t i = 0; i < count; ++i) x[i] = pcm[i] / 32768.0f;
    for (size_t i = 0; i < count; ++i) y[i] = i ? x[i] - 0.97f * x[i - 1] : x[i];
    float re[NFFT], im[NFFT];
    double P[NB], lm[NMEL];
    for (int f = 0; f < n; ++f) {
        size_t s = (size_t)f * HOP;
        double e = 0;
        for (int k = 0; k < NFFT; ++k) {
            float v = 0;
            if (k < WIN && s + k < count) { v = y[s + k] * t.ham[k]; e += (double)x[s + k] * x[s + k]; }
            re[k] = v; im[k] = 0;
        }
        energy[f] = (float)(10 * std::log10(e / WIN + 1e-12));
        Fft(re, im);
        for (int k = 0; k < NB; ++k) P[k] = (double)re[k] * re[k] + (double)im[k] * im[k];
        for (int i = 0; i < NMEL; ++i) {
            double s2 = 0;
            const float* row = &t.mel[i * NB];
            for (int k = 0; k < NB; ++k) s2 += row[k] * P[k];
            lm[i] = std::log(s2 + 1e-10);
        }
        for (int q = 0; q < NCEP; ++q) {
            double s3 = 0;
            for (int i = 0; i < NMEL; ++i) s3 += t.dct[q][i] * lm[i];
            cep[(size_t)f * NCEP + q] = (float)s3;
        }
    }
}

bool Wsr::Load(const std::string& b, std::string& error) {
    size_t p = 0;
    bool ok = true;
    if (b.size() < 12 || b.compare(0, 4, "WSR1") != 0) { error = "not a WSR1 file"; return false; }
    p = 4;
    uint32_t ns = Read<uint32_t>(b, p, ok), d = Read<uint32_t>(b, p, ok);
    if (d != (uint32_t)D) { error = "D mismatch"; return false; }
    _nv = (int)Read<uint32_t>(b, p, ok);
    _mu.resize(_nv * D); _width.resize(D); _prior.resize(_nv); _group.resize(_nv);
    for (auto& v : _mu) v = Read<float>(b, p, ok);
    for (auto& v : _width) v = Read<float>(b, p, ok);
    for (auto& v : _prior) v = Read<float>(b, p, ok);
    for (auto& v : _group) v = Read<uint8_t>(b, p, ok);
    _sent.clear();
    _sent.reserve(ns);
    for (uint32_t s = 0; s < ns && ok; ++s) {
        Sentence st;
        uint16_t len = Read<uint16_t>(b, p, ok);
        if (p + len > b.size()) { ok = false; break; }
        st.text = b.substr(p, len); p += len;
        st.frames = Read<uint16_t>(b, p, ok);
        st.feat.resize((size_t)st.frames * D);
        for (auto& v : st.feat) v = Read<int16_t>(b, p, ok) / 1024.0f;
        uint16_t nv = Read<uint16_t>(b, p, ok);
        for (uint16_t k = 0; k < nv; ++k) {
            VowelSeg g;
            g.vowel = Read<uint8_t>(b, p, ok);
            g.a = Read<uint16_t>(b, p, ok);
            g.b = Read<uint16_t>(b, p, ok);
            st.vowels.push_back(g);
        }
        _sent.push_back(std::move(st));
    }
    if (!ok) { error = "truncated"; _sent.clear(); return false; }
    return true;
}

WsrResult Wsr::Decode(const int16_t* pcm, size_t count) const {
    auto t0 = std::chrono::steady_clock::now();
    WsrResult res{std::string(), 0, 0, std::string(), 0};
    if (_sent.empty() || count < (size_t)WIN) return res;
    std::vector<float> cep, energy, dtwF, modF;
    WsrCepstra(pcm, count, cep, energy);
    int a, b;
    Features(cep, energy, dtwF, modF, a, b);
    const float* A = &dtwF[(size_t)a * D];
    int n = b - a;
    // pass 1: DTW cost against every sentence
    std::vector<std::pair<double, int>> sc;
    sc.reserve(_sent.size());
    for (size_t s = 0; s < _sent.size(); ++s)
        sc.push_back(std::make_pair(Dtw(A, n, _sent[s].feat.data(), _sent[s].frames, nullptr), (int)s));
    std::sort(sc.begin(), sc.end());
    // pass 2: vowel evidence for the best TopK (decode.vowel_evidence, with the template's own path)
    int k = std::min((int)sc.size(), TopK);
    std::vector<int> path;
    double logw = 0;
    for (int d = 0; d < D; ++d) logw += std::log(_width[d]);
    for (int r = 0; r < k; ++r) {
        const Sentence& st = _sent[sc[r].second];
        Dtw(A, n, st.feat.data(), st.frames, &path);
        std::vector<int> first(st.frames, -1), last(st.frames, -1);
        for (size_t q = 0; q < path.size(); q += 2) {
            int i = path[q], j = path[q + 1];
            if (first[j] < 0) first[j] = i;
            last[j] = i;
        }
        double tot = 0;
        int nseg = 0;
        for (const VowelSeg& g : st.vowels) {
            int ja = std::max(0, std::min(g.a, st.frames - 1)), jb = std::max(ja + 1, std::min(g.b, st.frames));
            int ia = first[ja], ib = last[jb - 1] + 1;
            if (ia < 0 || ib - ia < 3) continue;
            int lo = ia + (ib - ia) / 4, hi = std::max(ib - (ib - ia) / 4, lo + 1);
            std::vector<double> S(_nv, 0.0);
            for (int v = 0; v < _nv; ++v) {
                double s = 0;
                for (int t = lo; t < hi; ++t) {
                    const float* f = &modF[(size_t)(a + t) * D];
                    double q2 = 0;
                    for (int d = 0; d < D; ++d) { double z = (f[d] - _mu[v * D + d]) / _width[d]; q2 += z * z; }
                    s += -0.5 * q2 - logw;
                }
                S[v] = s + _prior[v];
            }
            double all = LogSumExp(S.data(), _nv);
            std::vector<double> grp;
            for (int v = 0; v < _nv; ++v)
                if (_group[v] == _group[g.vowel]) grp.push_back(S[v] - all);
            tot += LogSumExp(grp.data(), (int)grp.size());
            ++nseg;
        }
        double ev = nseg ? tot / nseg : -5.0;
        sc[r].first -= Beta * ev;
    }
    std::sort(sc.begin(), sc.begin() + k);
    res.text = _sent[sc[0].second].text;
    res.score = sc[0].first;
    if (sc.size() > 1) {
        res.gap = sc[1].first - sc[0].first;
        res.second = _sent[sc[1].second].text;
    }
    res.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

}  // namespace assist
