// Native sampling profiler (plan §52, phase 2): where the Web thread spends its time, by webkit.dll function.
//
// Off unless asked for (lw_profile): no thread, no timer, nothing sampled. While it runs, a sampler thread suspends
// the Web thread about every millisecond, reads its registers and walks its stack with the DLL's own unwind data
// (.pdata), then lets it go: the Web thread loses a few microseconds per sample. At the end the counts go to the log
// as webkit.dll offsets of function starts, and tools/profile.py names them with the link map.
#pragma once

namespace Lumia {

// Web thread, once at startup: the thread lw_profile samples.
void setProfiledThread();

// Any thread: samples the Web thread for `milliseconds` (at most 30 s), then logs the profile. A second call while
// one is running is ignored.
void startProfile(unsigned milliseconds);

} // namespace Lumia
