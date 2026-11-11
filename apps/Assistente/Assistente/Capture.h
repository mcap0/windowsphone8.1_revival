// Capture: microphone through WASAPI (ActivateAudioInterfaceAsync) on the device that really records on the
// Lumia 1020 ("Enhanced Audio Recording Device"), converted to 16 kHz mono and stopped by the voice detector.
#pragma once

#include <functional>
#include <string>
#include <vector>

class Capture {
public:
    // level: 0..1 for the meter (capture thread); done: PCM16 mono 16 kHz, heardSpeech, error text (capture thread).
    typedef std::function<void(double level)> LevelFn;
    typedef std::function<void(const std::vector<int16_t>& pcm, bool heardSpeech, const std::wstring& error)> DoneFn;

    // deviceId: from DeviceInformation (MediaDevice::GetAudioCaptureSelector), empty = default capture device.
    static void Start(const std::wstring& deviceId, LevelFn level, DoneFn done);
    // Long recording (tools/wsr "registra lungo"): no voice detector and no time limit; every chunkSamples the
    // PCM so far goes to chunk (capture thread), and done gets the rest after StopNow.
    typedef std::function<void(const std::vector<int16_t>& pcm)> ChunkFn;
    static void StartLong(const std::wstring& deviceId, unsigned chunkSamples, LevelFn level, ChunkFn chunk, DoneFn done);
    // Streaming (Vosk): every new piece of 16 kHz PCM goes to fn as soon as it is captured (capture thread), until
    // the recording ends. Set before Start; nullptr = off. It stays set for the following captures.
    // endsUtterance: the listener ends the recording itself with StopNow (Vosk's endpoint, which tells silence
    // phones from speech); the energy detector then never ends it, only the 25 s limit, a tap or StopNow.
    static void SetStream(ChunkFn fn, bool endsUtterance = false);
    // the next capture starts with ms of silence (the earcon plays while the microphone opens)
    static void MuteStart(unsigned ms);
    // Ends the recording now (tap on the screen): what was heard so far is sent.
    static void StopNow();
    static std::wstring LastFormat();

private:
    static void Begin(const std::wstring& deviceId, LevelFn level, DoneFn done, ChunkFn chunk, unsigned chunkSamples);
};
