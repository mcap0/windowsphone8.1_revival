// Wsp: the hand-written command network (tools/wsp/assistente.py writes Assets/intent.wsp; nothing is trained).
// Plain C++11, tested on the PC with g++ (Core/tests). Same tokenizer and forward pass as the Python script:
// embedding -> context convolution -> per-word tags (with a causal-mean head) -> AND rules -> intent logits.
#pragma once

#include <string>
#include <vector>

namespace assist {

struct WspToken {
    std::string word;   // vocabulary word ("<num>", "<time>", "<unk>" or the folded word itself)
    std::string folded; // the piece after folding (lowercase, accents removed)
    size_t begin, end;  // the piece's bytes in the original text
    int num;            // value of a <num>
    int hour, minute;   // value of a <time>
    int vocab;          // row in the embedding
    bool fuzzy;         // an unknown word read as the nearest known one (trigram layer)
    std::vector<float> tags;
};

struct WspResult {
    std::string intent;   // "online" = not a command: a Google search
    float margin;         // best logit minus the runner-up
    std::string best;     // the best intent even when it did not win clearly (for the log)
    std::vector<WspToken> tokens;
    std::string text;     // the original text
};

class Wsp {
public:
    Wsp();
    // the bytes of intent.wsp; false (and error) if the file does not match what the code expects
    bool Load(const std::string& bytes, std::string& error);
    bool Loaded() const { return _v > 0; }
    WspResult Run(const std::string& utf8) const;

    // the word has this feature (layer 0 of the network: word classes)
    bool Has(const WspToken& t, const char* feature) const;
    // the value of tag (qty, hour, minute, span, unexplained) on a word
    float Tag(const WspToken& t, const char* tag) const;
    // UTF-8 text -> words, exactly as tools/wsp/assistente.py tokenize()
    std::vector<WspToken> Tokenize(const std::string& utf8) const;
    static std::string Fold(const std::string& utf8);

private:
    int _v, _f, _c, _t, _r, _i, _off0, _off1, _maxTokens;
    int _buckets, _fuzzyMinLen;
    float _fuzzyMin;
    std::vector<float> _m;  // trigram bucket x vocab: 1/sqrt(n) on each known word's trigrams
    int Fuzzy(const std::string& word) const;
    std::vector<int> Trigrams(const std::string& word) const;
    std::vector<std::string> _vocab, _intents, _tagNames, _features;
    std::vector<float> _e, _k, _bk, _a, _ba, _b, _bb, _o, _bo;
    int VocabId(const std::string& w) const;
};

// Italian number word 0-99 ("venticinque", "un") -> value, else -1
int ItalianNumber(const std::string& folded);

}  // namespace assist
