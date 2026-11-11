// Wsr: the hand-built recognizer of tools/wsr on the phone (plain C++11, tested with g++ on the PC).
// It chooses among the sentences in Assets/voce.wsr (tools/wsr/export.py): MFCC of the recording (the same fixed
// formulas as tools/wsr/ear.py), DTW against each sentence's MBROLA template, then for the best TopK the vowel
// model's evidence on the template's vowel segments mapped through the DTW path (speaker's voice, Lumia microphone).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace assist {

struct WsrResult {
    std::string text;   // utf-8, the chosen sentence ("" if none)
    double score;       // DTW cost - Beta * vowel evidence (lower is better)
    double gap;         // second best score - best score: the confidence
    std::string second;
    double ms;          // decoding time
};

class Wsr {
public:
    bool Load(const std::string& bytes, std::string& error);
    bool Loaded() const { return !_sent.empty(); }
    size_t Sentences() const { return _sent.size(); }
    // pcm: 16 kHz mono
    WsrResult Decode(const int16_t* pcm, size_t count) const;

    static const int D = 12;
    static const int TopK = 20;
    static double Beta;

private:
    struct VowelSeg { int vowel, a, b; };
    struct Sentence {
        std::string text;
        int frames;
        std::vector<float> feat;    // frames x D
        std::vector<VowelSeg> vowels;
    };
    std::vector<Sentence> _sent;
    int _nv = 0;
    std::vector<float> _mu;         // nv x D
    std::vector<float> _width;      // D
    std::vector<float> _prior;      // nv
    std::vector<int> _group;        // nv
};

// The front end, exposed for the parity test: c0..c12 per frame and the log energy per frame.
void WsrCepstra(const int16_t* pcm, size_t count, std::vector<float>& cep, std::vector<float>& energy);

}  // namespace assist
