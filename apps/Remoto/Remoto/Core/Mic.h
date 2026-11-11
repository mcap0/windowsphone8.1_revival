#pragma once

// The Lumia's microphone for the Pixel (PLAN §68): when an app on the Pixel records ('m' 1), WASAPI capture on the
// microphone that really records on the Lumia 1020 ("Enhanced Audio Recording Device", as apps/Assistente found;
// a headset or Bluetooth one wins) -> mono 48 kHz -> Opus (libopus of Remoto/Opus, VOIP, 24 kbit/s, 20 ms) ->
// packet callback on the capture thread, which sends 'M'. The Pixel injects it as its microphone (RemotoMic.java).

#include <atomic>
#include <functional>
#include <string>
#include <vector>

class Mic
{
public:
    typedef std::function<void(const unsigned char* packet, size_t size)> PacketFn;
    typedef std::function<void(const std::string& line)> LogFn;

    // UI thread (WASAPI capture activation must start there). deviceId empty = default capture device.
    static void Start(const std::wstring& deviceId, PacketFn packet, LogFn log);
    static void Stop();
    static bool Running();
};
