#pragma once

#include <string>

// Decodes any audio file the phone can play (M4A/AAC, WMA, MP3, WAV) to 16-bit PCM WAV with Media Foundation's
// source reader. Blocking: call it off the UI thread. peakPercent = loudest sample, 0..100 (0 = silent recording).
bool DecodeToPcmWav(const std::wstring& path, std::string& wav, std::string& error, int* peakPercent, double* seconds);
