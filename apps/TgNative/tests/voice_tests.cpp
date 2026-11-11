#include "../core/voice.h"

#include <cmath>
#include <iostream>
#include <string>

int runVoiceTests()
{
    int fails = 0;
    // 2 s of a 440 Hz tone at 44.1 kHz stereo
    std::string wav = "RIFF....WAVEfmt ";
    auto le = [](std::string& o, unsigned v, int n) { for (int i = 0; i < n; ++i) o += static_cast<char>((v >> (8 * i)) & 255); };
    le(wav, 16, 4); le(wav, 1, 2); le(wav, 2, 2); le(wav, 44100, 4); le(wav, 44100 * 4, 4); le(wav, 4, 2); le(wav, 16, 2);
    wav += "data"; le(wav, 44100 * 2 * 4, 4);
    for (int i = 0; i < 44100 * 2; ++i) {
        int s = static_cast<int>(8000 * std::sin(i * 2 * 3.14159265 * 440 / 44100));
        le(wav, static_cast<unsigned>(s) & 0xffff, 2); le(wav, static_cast<unsigned>(s) & 0xffff, 2);
    }
    std::string ogg, wf, back;
    int secs = 0, secs2 = 0;
    bool ok = tg::wavToOggOpus(wav, ogg, &secs, &wf);
    std::cout << (ok && secs == 2 && wf.size() == 63 && ogg.compare(0, 4, "OggS") == 0 ? "  ok" : "  FAIL") << " encode wav->ogg (" << ogg.size() << " bytes)\n";
    if (!ok) ++fails;
    ok = tg::oggOpusToWav(ogg, back, &secs2);
    bool sizeOk = back.size() > 44 + 16000 * 2 * 19 / 10 && back.size() < 44 + 16000 * 2 * 22 / 10;
    std::cout << (ok && sizeOk ? "  ok" : "  FAIL") << " decode ogg->wav (" << back.size() << " bytes, " << secs2 << " s)\n";
    if (!ok || !sizeOk) ++fails;
    return fails;
}
