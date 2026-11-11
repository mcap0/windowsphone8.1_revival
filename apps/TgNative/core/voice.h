#ifndef TG_CORE_VOICE_H
#define TG_CORE_VOICE_H

// Voice notes: Telegram sends OGG/Opus, which WP8.1 cannot play; we decode to WAV for MediaElement and
// encode the microphone's WAV recording to OGG/Opus (16 kHz mono, 20 ms frames) with its waveform.

#include <string>

namespace tg {

// OGG/Opus -> 16 kHz mono 16-bit WAV. seconds may be null.
bool oggOpusToWav(const std::string& ogg, std::string& wav, int* seconds);

// PCM WAV (any rate, 8/16-bit, mono/stereo) -> OGG/Opus voice note. waveform = Telegram 5-bit packed, 100 values.
bool wavToOggOpus(const std::string& wav, std::string& ogg, int* seconds, std::string* waveform);

} // namespace tg

#endif
