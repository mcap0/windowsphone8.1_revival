// Sounds: the Bing voice screen's earcons through XAudio2. A MediaElement held them back while the
// microphone was open and played them when the app went away.
#pragma once

#include <string>

class Sounds {
public:
    // speechuxres.dll WAVE 240/241/245, as SpeechUX's EarConManager plays them ("Thinking" 242 left out: the user did not like it)
    enum Id { Listening, DoneListening, Results, Count };
    // XAudio2 and the three WAVs at app start: the first Play is not delayed by the initialization
    static void Prepare();
    static void Play(Id id);
    static void Stop(Id id);
};
