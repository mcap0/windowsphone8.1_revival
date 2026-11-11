// OfflineCommand: the phone's own speech recognizer (it-IT, offline) listens next to our capture with the command
// grammar (Assets/comandi.grxml, from tools/wsp/grammar.py) plus the contact names and the music library's artists
// and albums. What it hears goes through the hand-written network (Core/Wsp, Assets/intent.wsp); when that is sure,
// the command runs on the phone.
//
// The names are cached in LocalFolder (contatti.txt, brani.txt): reading 650 contacts takes ~6 s, so the
// recognizer is built from the cache at start and the cache is refreshed afterwards for the next time.
#pragma once

#include <functional>
#include <string>

#include "Core/Wsp.h"

class OfflineCommand {
public:
    // loads the network, builds the grammar from the cache and compiles it (once per run, in the background)
    static void Prepare();
    static const assist::Wsp& Net();
    // then() as soon as the recognizer is ready, or after maxMs (on the UI thread)
    static void WhenReady(int maxMs, std::function<void()> then);
    // starts listening, if the recognizer is ready; called when our capture opens the microphone
    static void Start();
    // once our capture has finished: waits a little for the recognizer, then gives what it heard
    // (empty = nothing sure: a Google search). Runs `then` on the UI thread.
    static void Finish(std::function<void(const std::wstring& text)> then);
    // what happened last, for the screen: "non pronto", "sentito \"...\" fiducia alta", "grammatica NON compilata"...
    static std::wstring Status();
    // the recognizer alone, without our capture (test of 2026-11-03: with both, RecognizeAsync fails at once with
    // 0x800455A0). then(text) on the UI thread: text = what it heard with high/medium confidence, else empty.
    static void ListenAlone(std::function<void(const std::wstring& text)> then);
    static void CancelAlone();
};
