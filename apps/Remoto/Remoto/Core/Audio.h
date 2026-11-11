#pragma once

// Remoto's sound: Opus packets from the Pixel (48 kHz stereo, 20 ms each; ponte/agent Remoto.java 'U') -> libopus
// (Remoto/Opus, fixed point) -> an XAudio2 source voice. Called on the network thread, one packet at a time.
// Adaptive jitter buffer: the target depth is how much the packets' delay varied over the last 2 s (on the LAN a
// couple of frames, through the relay on 4G a few hundred ms, when video bursts share the connection). After a gap
// the cushion is that target; packets are dropped only past target + 60 ms, so the delay can never grow for good.
// (A fixed 20 ms cushion and a 100 ms cap made the sound choppy through the relay: 500 drops in 3 minutes.)

#include <deque>
#include <string>
#include <vector>
#include <wrl/client.h>
#include <xaudio2.h>

struct OpusDecoder;

class Audio
{
public:
    Audio();
    ~Audio();
    bool Start(std::string& err);
    // arrivalUs: local clock; serverUs: the Pixel's timestamp of the packet (only differences are used)
    void Packet(const unsigned char* data, size_t size, long long arrivalUs, long long serverUs);
    int TargetMs() const { return m_target * 20; }
    void Stop();
    int Dropped() const { return m_dropped; }

private:
    void Submit(const short* pcm, int samples);

    Microsoft::WRL::ComPtr<IXAudio2> m_xaudio;
    IXAudio2MasteringVoice* m_master;
    IXAudio2SourceVoice* m_voice;
    OpusDecoder* m_decoder;
    std::vector<std::vector<short>> m_buffers; // ring: XAudio2 reads them until played
    std::vector<short> m_silence;
    size_t m_next;
    int m_dropped;
    int m_target;                              // depth in 20 ms buffers
    std::deque<std::pair<long long, long long>> m_delays; // (arrival, arrival - server) of the last 2 s
};
