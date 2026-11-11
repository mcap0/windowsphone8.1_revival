// VoskEngine: free Italian speech-to-text on the phone (vosk/ : vosk.dll ARM32 + vosk-model-small-it-0.22).
// The model (88 MB) is downloaded once from the PC inbox (share/vosk/) into LocalFolder; the DLL is in the package.
// While listening, Capture streams PCM to Feed(); a worker thread runs Vosk and reports partial text as it grows,
// then the final text when End() is called.
#pragma once

#include <functional>
#include <string>
#include <vector>

class VoskEngine {
public:
    typedef std::function<void(const std::wstring& text, bool final)> TextFn;  // called on the UI thread

    static void Prepare();     // download what is missing, load the DLL and the model (background)
    static bool Ready();
    static bool Coming();         // ready, or still downloading/loading: Begin() may start before the model is ready
    static std::wstring State();  // for the log / the screen while not ready
    static void Begin(TextFn fn); // a new utterance (audio queues while the model loads)
    static void Feed(const std::vector<int16_t>& pcm);  // capture thread
    static void End();            // no more audio: the final text follows
};
