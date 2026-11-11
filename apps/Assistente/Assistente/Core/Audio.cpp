#include "Audio.h"

#include <algorithm>
#include <cmath>

namespace assist {
namespace {
const int kRate = 16000;
const size_t kFrame = 320;            // 20 ms
const int kMinNoiseFrames = 10;       // first 200 ms calibrate the noise floor
const int kEndSilenceFrames = 90;     // 1.8 s of silence after speech ends the utterance
const int kMaxFrames = 1250;          // 25 s
const int kNoSpeechFrames = 400;      // 8 s without speech: give up

void le16(std::string& o, unsigned v) {
    o += static_cast<char>(v & 255);
    o += static_cast<char>((v >> 8) & 255);
}
void le32(std::string& o, unsigned v) {
    for (int i = 0; i < 4; ++i) o += static_cast<char>((v >> (8 * i)) & 255);
}

template <typename T>
double Sample(const T* s, size_t frame, int channels);
template <>
double Sample<float>(const float* s, size_t frame, int channels) {
    double sum = 0;
    for (int c = 0; c < channels; ++c) sum += s[frame * channels + c];
    return sum / channels;
}
template <>
double Sample<int16_t>(const int16_t* s, size_t frame, int channels) {
    double sum = 0;
    for (int c = 0; c < channels; ++c) sum += s[frame * channels + c] / 32768.0;
    return sum / channels;
}

template <typename T>
void Resample(const T* samples, size_t frames, int channels, int rate, std::vector<int16_t>& out, double& phase) {
    if (!samples || frames == 0 || channels <= 0 || rate <= 0) return;
    double step = static_cast<double>(rate) / kRate;  // input frames per output sample
    int box = std::max(1, static_cast<int>(step));  // averaging window against aliasing
    while (phase < frames) {
        size_t i = static_cast<size_t>(phase);
        double frac = phase - i;
        double acc = 0;
        int n = 0;
        for (int k = 0; k < box && i + k < frames; ++k, ++n) acc += Sample(samples, i + k, channels);
        double a = n ? acc / n : 0;
        double b = a;
        if (i + box < frames) {
            acc = 0;
            n = 0;
            for (int k = 0; k < box && i + 1 + k < frames; ++k, ++n) acc += Sample(samples, i + 1 + k, channels);
            b = n ? acc / n : a;
        }
        double v = (a + (b - a) * frac) * 32767.0;
        v = std::max(-32768.0, std::min(32767.0, v));
        out.push_back(static_cast<int16_t>(v));
        phase += step;
    }
    phase -= frames;
}
}  // namespace

void AppendMono16k(const float* samples, size_t frames, int channels, int rate, std::vector<int16_t>& out,
                   double& phase) {
    Resample(samples, frames, channels, rate, out, phase);
}

void AppendMono16k(const int16_t* samples, size_t frames, int channels, int rate, std::vector<int16_t>& out,
                   double& phase) {
    Resample(samples, frames, channels, rate, out, phase);
}

std::string WavBytes(const std::vector<int16_t>& pcm, int rate) {
    std::string w;
    unsigned bytes = static_cast<unsigned>(pcm.size() * 2);
    w += "RIFF";
    le32(w, 36 + bytes);
    w += "WAVE";
    w += "fmt ";
    le32(w, 16);
    le16(w, 1);
    le16(w, 1);
    le32(w, rate);
    le32(w, rate * 2);
    le16(w, 2);
    le16(w, 16);
    w += "data";
    le32(w, bytes);
    for (size_t i = 0; i < pcm.size(); ++i) le16(w, static_cast<uint16_t>(pcm[i]));
    return w;
}

Vad::Vad()
    : _noise(0), _level(0), _frames(0), _speechFrames(0), _silenceAfterSpeech(0), _done(false),
      _noSpeechTimeout(false), _speechThr(0), _silenceThr(0), _endSilenceFrames(kEndSilenceFrames),
      _noSpeechFrames(kNoSpeechFrames) {}

std::string Vad::Trace() const {
    std::string t = "noise " + std::to_string(static_cast<int>(_noise)) + " speech>" +
                    std::to_string(static_cast<int>(_speechThr)) + " silence<" +
                    std::to_string(static_cast<int>(_silenceThr)) + " frames " + _marks + " rms/100ms";
    for (int r : _rms100) t += " " + std::to_string(r);
    return t;
}

bool Vad::Feed(const int16_t* samples, size_t count) {
    for (size_t i = 0; i < count && !_done; ++i) {
        _frame.push_back(samples[i]);
        if (_frame.size() < kFrame) continue;
        double sum = 0;
        for (size_t k = 0; k < kFrame; ++k) sum += static_cast<double>(_frame[k]) * _frame[k];
        double rms = std::sqrt(sum / kFrame);
        _frame.clear();
        ++_frames;
        _level = std::min(1.0, rms / 6000.0);
        if (_frames % 5 == 0) _rms100.push_back(static_cast<int>(rms));
        if (_frames <= kMinNoiseFrames) {
            _noise = _noise + (rms - _noise) / _frames;
            _marks += 'c';
            continue;
        }
        double speechThreshold = std::max(_noise * 3.0, 500.0);
        double silenceThreshold = std::max(_noise * 2.0, 350.0);
        _speechThr = speechThreshold;
        _silenceThr = silenceThreshold;
        _marks += rms > speechThreshold ? '#' : rms < silenceThreshold ? '.' : '-';
        if (rms > speechThreshold) {
            ++_speechFrames;
            _silenceAfterSpeech = 0;
        } else {
            if (rms < silenceThreshold && _speechFrames < 5) _noise = _noise * 0.98 + rms * 0.02;
            // a quieter voice (above the silence line) is still talking: hold the count
            if (_speechFrames >= 5 && rms < silenceThreshold) ++_silenceAfterSpeech;
        }
        if (_speechFrames >= 5 && _silenceAfterSpeech >= _endSilenceFrames) _done = true;
        if (_frames >= kMaxFrames) _done = true;
        if (_speechFrames < 5 && _frames >= _noSpeechFrames) {
            _noSpeechTimeout = true;
            _done = true;
        }
    }
    return _done;
}

}  // namespace assist
