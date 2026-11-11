#pragma once

#include <string>
#include <vector>

// Decodes any audio file the phone can play (M4A/AAC, WMA, MP3, WAV) to 16-bit PCM WAV with Media Foundation's
// source reader. Blocking: call it off the UI thread. peakPercent = loudest sample, 0..100 (0 = silent recording).
// meanPercent = average level, 0..100 (a live mic never sits at exactly 0: it has a noise floor).
// slicePeaks = loudest sample of each 250 ms, 0..100: a loose flex shows up as gaps in the middle of a recording.
bool DecodeToPcmWav(const std::wstring& path, std::string& wav, std::string& error, int* peakPercent, double* seconds,
                    int* meanPercent = nullptr, std::vector<int>* slicePeaks = nullptr);
