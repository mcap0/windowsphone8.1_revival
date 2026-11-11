#pragma once

#include <functional>
#include <string>

// PLAN §69 step 0 on the Lumia: call audio. Plays a 1 kHz tone (500 ms on, 500 ms off) and records at the same time,
// with both streams in one audio category; the recording's level while the tone plays vs while it is silent says how
// much of our own output comes back into the microphone (what the far end would hear as echo).
namespace Probe
{
    typedef std::function<void(const std::wstring&)> LogFn;

    // category: AudioCategory value (3 = Communications, 0 = Other). endpoint: -1 leave, else AudioRoutingEndpoint.
    // Blocks for about seconds + 1 s: call it off the UI thread.
    void Duplex(int category, int endpoint, int seconds, LogFn log);
}
