// PCM out through XAudio2 2.8 (the phone's), and the clock of a playing media element: the count of samples the
// device has played (PLAN §75). One source voice per player on one engine for the process.

#pragma once

#include <wtf/Lock.h>
#include <wtf/Noncopyable.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

struct IXAudio2SourceVoice;

namespace Lumia {

class AudioOutput {
    WTF_MAKE_NONCOPYABLE(AudioOutput);
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(AudioOutput);
public:
    AudioOutput() = default;
    ~AudioOutput();

    // Any thread; the calls of one object are serialized inside.
    bool open(int rate, int channels, String& error);
    void close();
    bool isOpen() const;
    int rate() const { return m_rate; }
    int channels() const { return m_channels; }

    // Interleaved 16-bit samples; played in the order given.
    void submit(Vector<int16_t>&&);
    void start();
    void stop();
    void setVolume(float);
    void setRate(double);

    // Frames (one sample of every channel) since open(): given, and heard.
    uint64_t submittedFrames() const;
    uint64_t playedFrames() const;
    unsigned queuedBuffers() const;
    // Blocks until the voice finishes a buffer (or the timeout): a feeder thread waits here instead of polling.
    void waitForBufferEnd(unsigned milliseconds);

private:
    struct Callback;
    Callback* m_callback { nullptr }; // Made by open(), deleted with the object (after the voice): its callbacks use it.
    mutable Lock m_lock;
    IXAudio2SourceVoice* m_voice WTF_GUARDED_BY_LOCK(m_lock) { nullptr };
    int m_rate { 0 }, m_channels { 0 };
    uint64_t m_submitted WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    float m_volume WTF_GUARDED_BY_LOCK(m_lock) { 1 };
    double m_playbackRate WTF_GUARDED_BY_LOCK(m_lock) { 1 };
    bool m_running WTF_GUARDED_BY_LOCK(m_lock) { false };
};

} // namespace Lumia
