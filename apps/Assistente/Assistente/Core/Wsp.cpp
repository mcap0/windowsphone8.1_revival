#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS 1
#endif
#include "Wsp.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#if defined(_M_ARM) || defined(__ARM_NEON)
#include <arm_neon.h>
#define WSP_NEON 1
#endif

namespace assist {

namespace {

const char* const Units[] = {"zero", "uno", "due", "tre", "quattro", "cinque", "sei", "sette", "otto", "nove"};
const char* const Teens[] = {"dieci", "undici", "dodici", "tredici", "quattordici", "quindici", "sedici",
                             "diciassette", "diciotto", "diciannove"};
const char* const Tens[] = {"", "", "venti", "trenta", "quaranta", "cinquanta", "sessanta", "settanta", "ottanta",
                            "novanta"};

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

bool AllDigits(const std::string& s, size_t from, size_t to) {
    if (from >= to) return false;
    for (size_t i = from; i < to; ++i)
        if (!IsDigit(s[i])) return false;
    return true;
}

float Clip01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

// out[c] += x * row[c]: the inner loop of every layer (inputs are mostly 0/1, so rows are skipped when x == 0)
void AddRow(float* out, const float* row, float x, int n) {
    int c = 0;
#ifdef WSP_NEON
    for (; c + 4 <= n; c += 4) vst1q_f32(out + c, vmlaq_n_f32(vld1q_f32(out + c), vld1q_f32(row + c), x));
#endif
    for (; c < n; ++c) out[c] += x * row[c];
}

// out = bias + in * M (M is rows x cols, row-major)
void Dense(const float* in, int rows, const float* m, const float* bias, int cols, float* out) {
    std::memcpy(out, bias, sizeof(float) * cols);
    for (int r = 0; r < rows; ++r)
        if (in[r] != 0) AddRow(out, m + static_cast<size_t>(r) * cols, in[r], cols);
}

bool ReadFloats(const std::string& bytes, size_t& pos, size_t n, std::vector<float>& out) {
    if (pos + n * 4 > bytes.size()) return false;
    out.resize(n);
    if (n) std::memcpy(&out[0], bytes.data() + pos, n * 4);  // little-endian float32 on both ARM and x86
    pos += n * 4;
    return true;
}

}  // namespace

int ItalianNumber(const std::string& w) {
    if (w == "un" || w == "una" || w == "uno") return 1;
    for (int i = 0; i < 10; ++i)
        if (w == Units[i]) return i;
    for (int i = 0; i < 10; ++i)
        if (w == Teens[i]) return 10 + i;
    for (int t = 2; t < 10; ++t) {
        std::string name = Tens[t];
        if (w == name) return t * 10;
        for (int elided = 0; elided < 2; ++elided) {  // "venti"+"cinque", "vent"+"uno"/"otto"
            std::string stem = elided ? name.substr(0, name.size() - 1) : name;
            if (w.compare(0, stem.size(), stem) != 0) continue;
            std::string rest = w.substr(stem.size());
            for (int u = 1; u < 10; ++u) {
                if (rest != Units[u]) continue;
                bool vowel = u == 1 || u == 8;
                if (vowel != (elided == 1)) continue;
                return t * 10 + u;
            }
        }
    }
    return -1;
}

std::string Wsp::Fold(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c >= 'A' && c <= 'Z') {
            out += static_cast<char>(c + 32);
        } else if (c == 0xC3 && i + 1 < s.size()) {
            unsigned char d = static_cast<unsigned char>(s[i + 1]);
            if (d >= 0x80 && d <= 0x9E && d != 0x97) d = static_cast<unsigned char>(d + 0x20);  // uppercase -> lowercase
            char v = 0;
            if (d >= 0xA0 && d <= 0xA5) v = 'a';
            else if (d >= 0xA8 && d <= 0xAB) v = 'e';
            else if (d >= 0xAC && d <= 0xAF) v = 'i';
            else if (d >= 0xB2 && d <= 0xB6) v = 'o';
            else if (d >= 0xB9 && d <= 0xBC) v = 'u';
            if (v) out += v;
            else { out += static_cast<char>(c); out += static_cast<char>(d); }
            ++i;
        } else if (c == 0xE2 && i + 2 < s.size() && static_cast<unsigned char>(s[i + 1]) == 0x80 &&
                   (static_cast<unsigned char>(s[i + 2]) == 0x98 || static_cast<unsigned char>(s[i + 2]) == 0x99)) {
            out += '\'';
            i += 2;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}


// an unknown word with the ending of a polite frame (conditional, imperfect subjunctive, superlative, manner adverb):
// part of the frame, not a name or free text. Same rule as assistente.py frame_word.
static bool FrameWord(const std::string& w) {
    for (size_t i = 0; i < w.size(); ++i)
        if (w[i] < 'a' || w[i] > 'z') return false;
    static const struct { const char* end; size_t least; } ends[] = {
        {"rebbero", 8}, {"rebbe", 6}, {"resti", 7}, {"remmo", 7}, {"reste", 7}, {"rei", 6}, {"assero", 8},
        {"essero", 8}, {"assi", 7}, {"essi", 7}, {"issi", 8}, {"asse", 7}, {"esse", 7}, {"isse", 8}, {"issimo", 8},
        {"issima", 8}, {"issimi", 8}, {"issime", 8}, {"mente", 8}, {"evole", 7}, {"evoli", 7}, {0, 0}};
    for (int k = 0; ends[k].end; ++k) {
        size_t n = std::strlen(ends[k].end);
        if (w.size() >= ends[k].least && w.size() >= n && w.compare(w.size() - n, n, ends[k].end) == 0) return true;
    }
    return false;
}

std::vector<WspToken> Wsp::Tokenize(const std::string& text) const {
    // fold, remembering where each folded byte came from
    std::string f;
    std::vector<size_t> from;
    for (size_t i = 0; i < text.size();) {
        size_t len = 1;
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0xC0 && c < 0xE0) len = 2;
        else if (c >= 0xE0 && c < 0xF0) len = 3;
        else if (c >= 0xF0) len = 4;
        if (i + len > text.size()) len = text.size() - i;
        std::string piece = Fold(text.substr(i, len));
        for (size_t k = 0; k < piece.size(); ++k) {
            f += piece[k];
            from.push_back(i);
        }
        i += len;
    }
    from.push_back(text.size());
    // separators: whitespace , . ; : ! ? " ( ) ' and NBSP « » … ; '.'/':' between two digits stays (7:30)
    std::vector<bool> sep(f.size(), false);
    for (size_t i = 0; i < f.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(f[i]);
        if (std::strchr(" \t\n\r\f\v,.;:!?\"()'", c) && c != 0) {
            sep[i] = true;
            if ((c == ':' || c == '.') && i > 0 && i + 1 < f.size() && IsDigit(f[i - 1]) && IsDigit(f[i + 1])) {
                sep[i] = false;
                f[i] = ':';
            }
        } else if (c == 0xC2 && i + 1 < f.size()) {
            unsigned char d = static_cast<unsigned char>(f[i + 1]);
            if (d == 0xA0 || d == 0xAB || d == 0xBB) sep[i] = sep[i + 1] = true;
        } else if (c == 0xE2 && i + 2 < f.size() && static_cast<unsigned char>(f[i + 1]) == 0x80 &&
                   static_cast<unsigned char>(f[i + 2]) == 0xA6) {
            sep[i] = sep[i + 1] = sep[i + 2] = true;
        }
    }
    std::vector<WspToken> toks;
    size_t i = 0;
    while (i < f.size() && static_cast<int>(toks.size()) < _maxTokens) {
        if (sep[i]) { ++i; continue; }
        size_t j = i;
        while (j < f.size() && !sep[j]) ++j;
        WspToken t;
        t.folded = f.substr(i, j - i);
        t.begin = from[i];
        t.end = from[j];
        t.num = -1;
        t.hour = t.minute = -1;
        const std::string& p = t.folded;
        size_t colon = p.find(':');
        if (colon != std::string::npos && colon >= 1 && colon <= 2 && p.size() == colon + 3 &&
            AllDigits(p, 0, colon) && AllDigits(p, colon + 1, p.size()) && atoi(p.c_str()) < 24 &&
            atoi(p.c_str() + colon + 1) < 60) {
            t.word = "<time>";
            t.hour = atoi(p.c_str());
            t.minute = atoi(p.c_str() + colon + 1);
        } else if (p.size() <= 4 && AllDigits(p, 0, p.size())) {
            t.word = "<num>";
            t.num = atoi(p.c_str());
        } else if (p == "mezzogiorno") {
            t.word = "<time>";
            t.hour = 12;
            t.minute = 0;
        } else if (p == "mezzanotte") {
            t.word = "<time>";
            t.hour = 0;
            t.minute = 0;
        } else if (ItalianNumber(p) >= 0) {
            t.word = "<num>";
            t.num = ItalianNumber(p);
        } else {
            t.word = p;
        }
        t.vocab = VocabId(t.word);
        t.fuzzy = false;
        if (t.vocab < 0 && FrameWord(t.word)) t.vocab = VocabId("<frame>");  // "sarebbe", "riuscissi", "cortesemente"
        if (t.vocab < 0) {
            t.vocab = Fuzzy(t.word);
            t.fuzzy = t.vocab >= 0;
            if (t.fuzzy) t.word = _vocab[t.vocab];  // "telegramm" is read as "telegram" (folded keeps what was typed)
            if (t.vocab < 0) t.vocab = VocabId("<unk>");
        }
        toks.push_back(t);
        i = j;
    }
    // "un secondo" is a moment ("ti chiederei di un secondo dirmi l'ora"), not one second (assistente.py tokenize)
    for (size_t k = 0; k + 1 < toks.size(); ++k)
        if (toks[k].folded == "un" && toks[k + 1].word == "secondo") {
            toks[k].word = "un";
            toks[k].num = -1;
            toks[k].vocab = VocabId("un");
            toks[k + 1].word = "attimo";
            toks[k + 1].vocab = VocabId("attimo");
        }
    return toks;
}

Wsp::Wsp() : _v(0), _f(0), _c(0), _t(0), _r(0), _i(0), _off0(0), _off1(0), _maxTokens(32), _buckets(0), _fuzzyMinLen(4),
             _fuzzyMin(1) {}

// bucket ids of the character trigrams of ^word$ (FNV-1a 32 bit), each once: assistente.py trigrams()
std::vector<int> Wsp::Trigrams(const std::string& w) const {
    std::string b = "^" + w + "$";
    std::vector<int> grams;
    for (size_t i = 0; i + 2 < b.size(); ++i) {
        unsigned h = 2166136261u;
        for (size_t k = i; k < i + 3; ++k) h = (h ^ static_cast<unsigned char>(b[k])) * 16777619u;
        int g = static_cast<int>(h % static_cast<unsigned>(_buckets));
        if (std::find(grams.begin(), grams.end(), g) == grams.end()) grams.push_back(g);
    }
    return grams;
}

// the trigram layer: an unknown word -> the known word whose trigrams it shares most (cosine), same first letter
int Wsp::Fuzzy(const std::string& w) const {
    if (_buckets <= 0 || static_cast<int>(w.size()) < _fuzzyMinLen) return -1;
    for (size_t i = 0; i < w.size(); ++i)
        if (!((w[i] >= 'a' && w[i] <= 'z') || w[i] == '\'' || w[i] == '-')) return -1;
    std::vector<int> grams = Trigrams(w);
    std::vector<float> scores(_v, 0.0f);
    float x = 1.0f / std::sqrt(static_cast<float>(grams.size()));
    for (size_t i = 0; i < grams.size(); ++i) AddRow(&scores[0], &_m[static_cast<size_t>(grams[i]) * _v], x, _v);
    int best = -1;
    for (int v = 0; v < _v; ++v) {
        if (_vocab[v].empty() || _vocab[v][0] != w[0]) continue;  // typos keep the first letter
        // and about the length: "contento" is not "conto" (assistente.py fuzzy, giro 11)
        int d = static_cast<int>(_vocab[v].size()) - static_cast<int>(w.size());
        if (d < 0) d = -d;
        if (d > std::max(1, static_cast<int>(w.size()) / 4)) continue;
        if (best < 0 || scores[v] > scores[best]) best = v;
    }
    return best >= 0 && scores[best] >= _fuzzyMin ? best : -1;
}

int Wsp::VocabId(const std::string& w) const {
    std::vector<std::string>::const_iterator it = std::lower_bound(_vocab.begin(), _vocab.end(), w);
    return it != _vocab.end() && *it == w ? static_cast<int>(it - _vocab.begin()) : -1;
}

bool Wsp::Load(const std::string& bytes, std::string& error) {
    _v = 0;
    _vocab.clear();
    _intents.clear();
    _tagNames.clear();
    _features.clear();
    size_t pos = 0;
    bool done = false;
    int v = 0, f = 0, c = 0, t = 0, r = 0, i = 0, o0 = 0, o1 = 0;
    while (pos < bytes.size() && !done) {
        size_t nl = bytes.find('\n', pos);
        if (nl == std::string::npos) break;
        std::string line = bytes.substr(pos, nl - pos);
        pos = nl + 1;
        if (line == "---") done = true;
        else if (line.compare(0, 5, "dims ") == 0) sscanf(line.c_str() + 5, "%d %d %d %d %d %d %d %d", &v, &f, &c, &t, &r, &i, &o0, &o1);
        else if (line.compare(0, 11, "max_tokens ") == 0) _maxTokens = atoi(line.c_str() + 11);
        else if (line.compare(0, 6, "fuzzy ") == 0) sscanf(line.c_str() + 6, "%d %f %d", &_buckets, &_fuzzyMin, &_fuzzyMinLen);
        else if (line.compare(0, 2, "v ") == 0) _vocab.push_back(line.substr(2));
        else if (line.compare(0, 2, "i ") == 0) _intents.push_back(line.substr(2));
        else if (line.compare(0, 2, "t ") == 0) _tagNames.push_back(line.substr(2));
        else if (line.compare(0, 2, "f ") == 0) _features.push_back(line.substr(2));
    }
    if (!done || bytes.compare(0, 5, "WSP1\n") != 0) { error = "not a WSP1 file"; return false; }
    if (static_cast<int>(_vocab.size()) != v || static_cast<int>(_intents.size()) != i ||
        static_cast<int>(_tagNames.size()) != t || static_cast<int>(_features.size()) != f || o1 < o0) {
        error = "header and dims differ";
        return false;
    }
    if (!std::is_sorted(_vocab.begin(), _vocab.end())) { error = "vocabulary not sorted"; return false; }
    int w = o1 - o0 + 1, g = c + 2 * t;
    if (!ReadFloats(bytes, pos, static_cast<size_t>(v) * f, _e) ||
        !ReadFloats(bytes, pos, static_cast<size_t>(w) * f * c, _k) || !ReadFloats(bytes, pos, c, _bk) ||
        !ReadFloats(bytes, pos, static_cast<size_t>(2 * c) * t, _a) || !ReadFloats(bytes, pos, t, _ba) ||
        !ReadFloats(bytes, pos, static_cast<size_t>(g) * r, _b) || !ReadFloats(bytes, pos, r, _bb) ||
        !ReadFloats(bytes, pos, static_cast<size_t>(r) * i, _o) || !ReadFloats(bytes, pos, i, _bo)) {
        error = "weights truncated";
        return false;
    }
    size_t buckets = static_cast<size_t>(_buckets > 0 ? _buckets : 0);
    if (pos == bytes.size()) {
        // the trigram layer is not stored: it is written here from the vocabulary, as assistente.py fuzzy_rows() does
        // (1/sqrt(n) on the n trigram buckets of each known word of FUZZY_MIN_LEN+ letters)
        _m.assign(buckets * v, 0.0f);
        int nofuzzy = -1;  // verb forms made by the script: not targets of the typo layer
        for (size_t fi = 0; fi < _features.size(); ++fi)
            if (_features[fi] == "nofuzzy") nofuzzy = static_cast<int>(fi);
        for (int w = 0; w < v && buckets; ++w) {
            const std::string& word = _vocab[w];
            if (word.empty() || word[0] == '<' || static_cast<int>(word.size()) < _fuzzyMinLen) continue;
            if (nofuzzy >= 0 && _e[static_cast<size_t>(w) * f + nofuzzy] > 0.5f) continue;
            std::vector<int> grams = Trigrams(word);
            float x = 1.0f / std::sqrt(static_cast<float>(grams.size()));
            for (size_t g = 0; g < grams.size(); ++g) _m[static_cast<size_t>(grams[g]) * v + w] = x;
        }
    } else if (!ReadFloats(bytes, pos, buckets * v, _m) || pos != bytes.size()) {  // older files carry it
        error = "weights truncated or too long";
        return false;
    }
    _f = f; _c = c; _t = t; _r = r; _i = i; _off0 = o0; _off1 = o1;
    _v = v;
    return true;
}

WspResult Wsp::Run(const std::string& text) const {
    WspResult res;
    res.text = text;
    res.intent = res.best = "online";
    res.margin = 0;
    if (!Loaded()) return res;
    res.tokens = Tokenize(text);
    const int n = static_cast<int>(res.tokens.size());
    if (n == 0) return res;
    // layer 1: context convolution over the embeddings
    std::vector<float> h1(static_cast<size_t>(n) * _c);
    for (int p = 0; p < n; ++p) {
        float* out = &h1[static_cast<size_t>(p) * _c];
        std::memcpy(out, &_bk[0], sizeof(float) * _c);
        for (int off = _off0; off <= _off1; ++off) {
            int s = p + off;
            if (s < 0 || s >= n) continue;
            const float* e = &_e[static_cast<size_t>(res.tokens[s].vocab) * _f];
            const float* k = &_k[static_cast<size_t>(off - _off0) * _f * _c];
            for (int fi = 0; fi < _f; ++fi)
                if (e[fi] != 0) AddRow(out, k + static_cast<size_t>(fi) * _c, e[fi], _c);
        }
        for (int ci = 0; ci < _c; ++ci) out[ci] = Clip01(out[ci]);
    }
    // layer 2: per-word tags from [h1 ; causal mean of h1 over the earlier words]
    std::vector<float> in2(2 * _c), sum(_c, 0.0f), h2(static_cast<size_t>(n) * _t);
    for (int p = 0; p < n; ++p) {
        std::memcpy(&in2[0], &h1[static_cast<size_t>(p) * _c], sizeof(float) * _c);
        for (int ci = 0; ci < _c; ++ci) in2[_c + ci] = p > 0 ? sum[ci] / p : 0.0f;
        float* out = &h2[static_cast<size_t>(p) * _t];
        Dense(&in2[0], 2 * _c, &_a[0], &_ba[0], _t, out);
        for (int ti = 0; ti < _t; ++ti) out[ti] = Clip01(out[ti]);
        for (int ci = 0; ci < _c; ++ci) sum[ci] += h1[static_cast<size_t>(p) * _c + ci];
        res.tokens[p].tags.assign(out, out + _t);
    }
    // pooling: [max h1 ; max h2 ; mean h2]
    std::vector<float> g(_c + 2 * _t, 0.0f);
    for (int p = 0; p < n; ++p) {
        for (int ci = 0; ci < _c; ++ci) g[ci] = std::max(g[ci], h1[static_cast<size_t>(p) * _c + ci]);
        for (int ti = 0; ti < _t; ++ti) {
            float x = h2[static_cast<size_t>(p) * _t + ti];
            g[_c + ti] = std::max(g[_c + ti], x);
            g[_c + _t + ti] += x / n;
        }
    }
    // layer 3: rules (AND of features), layer 4: intent logits
    std::vector<float> rules(_r), logits(_i);
    Dense(&g[0], _c + 2 * _t, &_b[0], &_bb[0], _r, &rules[0]);
    for (int ri = 0; ri < _r; ++ri) rules[ri] = Clip01(rules[ri]);
    Dense(&rules[0], _r, &_o[0], &_bo[0], _i, &logits[0]);
    int best = 0;
    for (int ii = 1; ii < _i; ++ii)
        if (logits[ii] > logits[best]) best = ii;
    float second = -1e9f;
    for (int ii = 0; ii < _i; ++ii)
        if (ii != best) second = std::max(second, logits[ii]);
    res.margin = logits[best] - second;
    res.best = _intents[best];
    if (_intents[best] != "online" && res.margin >= 0.5f) res.intent = _intents[best];
    return res;
}

bool Wsp::Has(const WspToken& t, const char* feature) const {
    for (size_t fi = 0; fi < _features.size(); ++fi)
        if (_features[fi] == feature) return _e[static_cast<size_t>(t.vocab) * _f + fi] > 0.5f;
    return false;
}

float Wsp::Tag(const WspToken& t, const char* tag) const {
    for (size_t ti = 0; ti < _tagNames.size() && ti < t.tags.size(); ++ti)
        if (_tagNames[ti] == tag) return t.tags[ti];
    return 0;
}

}  // namespace assist
