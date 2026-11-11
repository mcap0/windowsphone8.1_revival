// Host test of the patched Vosk: vosktest <model dir> <16 kHz mono 16-bit wav>...
// One wav: partial results (VOSK_PARTIAL=0 to turn off) and the final one, as before.
// Several wavs: one line per file "name<TAB>text<TAB>audio ms<TAB>compute ms", then the totals and the load time
// (the bench of PLAN.md §59.5).
#include "model.h"
#include "vosk_api.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
static double Ms(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}
static std::chrono::steady_clock::time_point g_t0;
static void LogMs(const char* m) { printf("+%6.1f ms %s\n", Ms(g_t0), m); }  // VOSK_TIMES=1: load phases
extern "C" void vosk_wp81_set_log_handler(void (*h)(const char*));
static std::string Text(const char* json) {  // {"text" : "..."} -> ...
    const char* p = strstr(json, "\"text\" : \"");
    if (!p) return "";
    p += 10;
    const char* e = strrchr(p, '"');
    return e && e > p ? std::string(p, e) : "";
}
int main(int argc, char** argv) {
    vosk_set_log_level(0);
    auto t0 = std::chrono::steady_clock::now();
    g_t0 = t0;
    if (getenv("VOSK_TIMES")) vosk_wp81_set_log_handler(&LogMs);
    VoskModel* m = vosk_model_new(argv[1]);
    if (!m) { printf("model failed\n"); return 1; }
    double load = Ms(t0);
    // VOSK_WRITE_PREP=1: write <model>/am/final.mdl.prep for the phone (PLAN.md §59.6)
    if (getenv("VOSK_WRITE_PREP")) {
        std::string prep = std::string(argv[1]) + "/am/final.mdl.prep";
        reinterpret_cast<Model*>(m)->WritePrepared(prep);
        printf("wrote %s\n", prep.c_str());
    }
    if (argc < 3) { printf("model ok %.0f ms\n", load); return 0; }
    const char* ce = getenv("VOSK_CHUNK");
    const char* pe = getenv("VOSK_PARTIAL");
    bool multi = argc > 3;
    bool partial = multi ? pe && atoi(pe) : !pe || atoi(pe);
    std::vector<short> buf(ce ? atoi(ce) : 1600);
    double audio = 0, busy = 0;
    for (int a = 2; a < argc; a++) {
        VoskRecognizer* r = vosk_recognizer_new(m, 16000.0f);
        // VOSK_ENDPOINT=<s>: the app's end-of-utterance (vosk_recognizer_set_endpointer_delays(8, s, 25)); prints
        // where the first endpoint fell and the text up to there, to check it never cuts a command short
        const char* ee = getenv("VOSK_ENDPOINT");
        if (ee) vosk_recognizer_set_endpointer_delays(r, 8.0f, (float)atof(ee), 25.0f);
        size_t endpoint_at = 0;
        std::string endpoint_text;
        FILE* f = fopen(argv[a], "rb");
        if (!f) { printf("cannot open %s\n", argv[a]); return 1; }
        fseek(f, 44, SEEK_SET);
        size_t n, samples = 0;
        auto t = std::chrono::steady_clock::now();
        while ((n = fread(buf.data(), 2, buf.size(), f)) > 0) {
            samples += n;
            if (vosk_recognizer_accept_waveform_s(r, buf.data(), (int)n) && ee && !endpoint_at) {
                endpoint_at = samples;
                endpoint_text = Text(vosk_recognizer_result(r));
            }
            if (partial) { const char* p = vosk_recognizer_partial_result(r); if (!multi) printf("partial %s\n", p); }
        }
        const char* fin = vosk_recognizer_final_result(r);
        double ms = Ms(t);
        fclose(f);
        if (!multi) { printf("final %s\n", fin); return 0; }
        const char* name = strrchr(argv[a], '/');
        printf("%s\t%s\t%.0f\t%.1f", name ? name + 1 : argv[a], Text(fin).c_str(), samples / 16.0, ms);
        if (ee) printf("\tendpoint %.0f\t%s", endpoint_at / 16.0, endpoint_text.c_str());
        printf("\n");
        audio += samples / 16.0;
        busy += ms;
        vosk_recognizer_free(r);
    }
    printf("TOTAL\taudio %.0f ms\tcompute %.0f ms\t%.3fx\tload %.0f ms\n", audio, busy, busy / audio, load);
}
