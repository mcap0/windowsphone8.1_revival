// Audio helpers for the assistant (plain C++11, tested with g++ on the PC):
// capture format -> 16 kHz mono PCM16, WAV bytes for the recordings, and a simple energy voice detector.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace assist {

// Interleaved float32 [-1,1] or int16 frames at any rate -> 16 kHz mono int16 (box low-pass + linear interpolation).
void AppendMono16k(const float* samples, size_t frames, int channels, int rate, std::vector<int16_t>& out,
                   double& phase);
void AppendMono16k(const int16_t* samples, size_t frames, int channels, int rate, std::vector<int16_t>& out,
                   double& phase);

// RIFF/WAVE PCM16 mono.
std::string WavBytes(const std::vector<int16_t>& pcm, int rate);

// End-of-speech detector on 16 kHz mono, 20 ms frames.
class Vad {
public:
    Vad();
    // silence after speech that ends the utterance (default 1.8 s); longer when Vosk's endpoint decides (PLAN.md §59.6)
    void SetEndSilenceMs(int ms) { _endSilenceFrames = ms / 20; }
    // no end on energy at all (Vosk's endpoint decides, also "nothing said in 8 s"): only the 25 s limit
    void EndOnlyAtLimit() { _endSilenceFrames = 1 << 30; _noSpeechFrames = 1 << 30; }
    // Feeds new samples; returns true once the utterance is over (speech then ~1 s of silence, or limits).
    bool Feed(const int16_t* samples, size_t count);
    bool HeardSpeech() const { return _speechFrames >= 5; }
    bool TimedOutWithoutSpeech() const { return _noSpeechTimeout; }
    double Level() const { return _level; }  // 0..1 for the UI meter
    // for the log: noise floor, thresholds and the energy every 100 ms ("." frame under the silence line,
    // "-" between the lines, "#" speech), to see why an utterance ended (PLAN.md §59.6)
    std::string Trace() const;

private:
    std::vector<int16_t> _frame;
    double _noise;
    double _level;
    int _frames;
    int _speechFrames;
    int _silenceAfterSpeech;
    bool _done;
    bool _noSpeechTimeout;
    std::string _marks;           // one char per 20 ms frame
    std::vector<int> _rms100;     // rms every 5 frames
    double _speechThr, _silenceThr;
    int _endSilenceFrames;
    int _noSpeechFrames;
};

}  // namespace assist
