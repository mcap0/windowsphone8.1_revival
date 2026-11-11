// g++ -std=c++11 -O2 -I.. ../Audio.cpp ../Text.cpp ../Wsp.cpp ../Command.cpp core_tests.cpp -o core_tests && ./core_tests
// (run from this directory: it reads ../../Assets/intent.wsp and tools/wsp/golden.tsv)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "Audio.h"
#include "Command.h"
#include "Text.h"
#include "Wsp.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

static std::string ReadFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

// golden.tsv: every sentence through the network (intent) and Command (arguments), "now" = Tue 2026-11-03 10:00
static void WspTests() {
    using namespace assist;
    CHECK(ItalianNumber("venticinque") == 25 && ItalianNumber("ventuno") == 21 && ItalianNumber("trentotto") == 38);
    CHECK(ItalianNumber("ventiuno") == -1 && ItalianNumber("un") == 1 && ItalianNumber("cinquantanove") == 59);
    CHECK(Wsp::Fold("Lunedì È") == "lunedi e");
    Wsp net;
    std::string error;
    bool loaded = net.Load(ReadFile("../../Assets/intent.wsp"), error);
    if (!loaded) std::printf("intent.wsp: %s\n", error.c_str());
    CHECK(loaded);
    if (!loaded) return;
    std::vector<WspToken> tk = net.Tokenize("Svegliami alle 7:30, domani");
    CHECK(tk.size() == 4 && tk[2].word == "<time>" && tk[2].hour == 7 && tk[2].minute == 30);
    tk = net.Tokenize("chiama mamma\xe2\x80\x99s lunedì");
    CHECK(tk.size() == 4 && tk[3].folded == "lunedi" && tk[3].end == std::string("chiama mamma\xe2\x80\x99s lunedì").size());
    const char* targetNames[] = {"wi-fi", "bluetooth", "modalit\xc3\xa0 aereo", "rete cellulare", "posizione",
                                 "risparmio batteria", "schermata di blocco", "rotazione schermo", "account email",
                                 "musica", "orologio", "sveglie", "calendario", "meteo", "telegram", "posta",
                                 "registratore", "file", "perle tv", "authenticator", "appunti", "esplora"};
    std::vector<std::string> targets(targetNames, targetNames + sizeof(targetNames) / sizeof(targetNames[0]));
    LocalTime now = {2026, 11, 3, 10, 0, 0, 2};
    std::istringstream golden(ReadFile("../../../../../tools/wsp/golden.tsv"));
    std::string line;
    int rows = 0, bad = 0;
    double slowest = 0, total = 0;
    while (std::getline(golden, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> col;
        std::stringstream cs(line);
        std::string part;
        while (std::getline(cs, part, '\t')) col.push_back(part);
        if (col.size() < 2) continue;
        ++rows;
        auto t0 = std::chrono::steady_clock::now();
        WspResult r = net.Run(col[0]);
        double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
        slowest = std::max(slowest, us);
        total += us;
        Command c;
        bool ok = BuildCommand(net, r, now, targets, c);
        std::string got = ok ? c.name : "online";
        bool good = got == col[1];
        std::string detail;
        if (good && col.size() > 2) {
            std::stringstream as(col[2]);
            std::string kv;
            while (std::getline(as, kv, ';')) {
                size_t eq = kv.find('=');
                std::string k = kv.substr(0, eq), want = kv.substr(eq + 1), have = c.Arg(k);
                if (Wsp::Fold(have) != Wsp::Fold(want)) {
                    good = false;
                    detail += " " + k + "=\"" + have + "\" (want \"" + want + "\")";
                }
            }
        }
        if (!good) {
            ++bad;
            std::printf("golden FAIL: \"%s\" -> %s%s (net %s %.2f, %s)\n", col[0].c_str(), got.c_str(), detail.c_str(),
                        r.best.c_str(), r.margin, c.why.c_str());
        }
    }
    std::printf("golden: %d/%d, network %.0f us average, %.0f us slowest\n", rows - bad, rows, total / rows, slowest);
    CHECK(rows > 100 && bad == 0);
    Command c;
    WspResult r = net.Run("che giorno è oggi");
    CHECK(BuildCommand(net, r, now, targets, c) && c.say == "Oggi \xc3\xa8 marted\xc3\xac 3 novembre.");
    r = net.Run("che ore sono");
    CHECK(BuildCommand(net, r, now, targets, c) && c.say == "Sono le 10:00.");
}

int main() {
    using namespace assist;
    // resampling 48 kHz stereo float -> 16 kHz mono: 1 s in, ~16000 out
    std::vector<float> in(48000 * 2);
    for (size_t i = 0; i < 48000; ++i) in[2 * i] = in[2 * i + 1] = static_cast<float>(0.5 * std::sin(i * 2 * 3.14159 * 440 / 48000));
    std::vector<int16_t> out;
    double phase = 0;
    AppendMono16k(&in[0], 24000, 2, 48000, out, phase);
    AppendMono16k(&in[48000], 24000, 2, 48000, out, phase);
    CHECK(out.size() >= 15990 && out.size() <= 16010);
    int peak = 0;
    for (size_t i = 0; i < out.size(); ++i) peak = std::max(peak, std::abs(static_cast<int>(out[i])));
    CHECK(peak > 15000 && peak < 17000);
    std::string wav = WavBytes(out, 16000);
    CHECK(wav.size() == 44 + out.size() * 2);
    CHECK(wav.compare(0, 4, "RIFF") == 0 && wav.compare(8, 4, "WAVE") == 0);

    // VAD: silence, 1 s of tone, silence -> done after ~1.8 s of silence
    Vad vad;
    std::vector<int16_t> silence(320, 20), tone(320);
    for (size_t i = 0; i < tone.size(); ++i) tone[i] = static_cast<int16_t>(8000 * std::sin(i * 0.3));
    bool done = false;
    int frames = 0;
    for (int i = 0; i < 20 && !done; ++i, ++frames) done = vad.Feed(&silence[0], silence.size());
    for (int i = 0; i < 50 && !done; ++i, ++frames) done = vad.Feed(&tone[0], tone.size());
    CHECK(!done && vad.HeardSpeech());
    int after = 0;
    while (!done && after < 200) { done = vad.Feed(&silence[0], silence.size()); ++after; }
    CHECK(done && after >= 85 && after <= 100);
    Vad quiet;
    int n = 0;
    while (!quiet.Feed(&silence[0], silence.size()) && n < 1000) ++n;
    CHECK(quiet.TimedOutWithoutSpeech());

    // URLs
    CHECK(PercentEncode("a b/é") == "a%20b%2F%C3%A9");
    CHECK(GoogleSearchUrl("meteo palermo") == "https://www.google.com/search?q=meteo+palermo");
    CHECK(GoogleSearchUrl("c&a") == "https://www.google.com/search?q=c%26a");

    // alarm times
    int h = -1, m = -1;
    CHECK(ParseHourMinute("7", h, m) && h == 7 && m == 0);
    CHECK(ParseHourMinute("07:30", h, m) && h == 7 && m == 30);
    CHECK(ParseHourMinute("19.05", h, m) && h == 19 && m == 5);
    CHECK(!ParseHourMinute("25:00", h, m));
    CHECK(!ParseHourMinute("sette", h, m));
    LocalTime now = {2026, 10, 21, 13, 20, 0, 3};
    LocalTime when;
    CHECK(AlarmMoment(now, 7, 30, "", when) && when.day == 22 && when.month == 10 && when.hour == 7);
    CHECK(AlarmMoment(now, 18, 0, "", when) && when.day == 21);
    CHECK(AlarmMoment(now, 7, 0, "domani", when) && when.day == 22 && when.weekday == 4);
    CHECK(!AlarmMoment(now, 9, 0, "oggi", when));
    CHECK(AlarmMoment(now, 8, 0, "2026-11-01", when) && when.month == 11 && when.day == 1 && when.weekday == 0);
    LocalTime eom = {2026, 12, 31, 23, 50, 0, 4};
    CHECK(AlarmMoment(eom, 6, 0, "", when) && when.year == 2027 && when.month == 1 && when.day == 1);
    CHECK(ItalianNow(now) == "Oggi è mercoledì 21 ottobre 2026, ore 13:20");

    WspTests();

    std::printf(g_fail ? "%d FAILED\n" : "all ok\n", g_fail);
    return g_fail ? 1 : 0;
}
