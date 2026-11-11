// The fatal path of the no-exceptions build (wp81-vosk.h): log, call the app's handler, abort.
#include "wp81-vosk.h"
#ifdef _WIN32
#include <windows.h>
#else
#define OutputDebugStringA(s) fputs(s, stderr)
#define __declspec(x) __attribute__((visibility("default")))
#endif

namespace wp81 {
FatalHandler& Handler() {
    static FatalHandler h = nullptr;
    return h;
}
void Die(const char* what) {
    OutputDebugStringA("vosk fatal: ");
    OutputDebugStringA(what ? what : "?");
    OutputDebugStringA("\n");
    if (Handler()) Handler()(what ? what : "?");
    abort();
}
}  // namespace wp81

extern "C" __declspec(dllexport) void vosk_wp81_set_fatal_handler(void (*h)(const char*)) { wp81::Handler() = h; }

// Kaldi's log lines (KALDI_LOG/WARN) to the app, for the load-phase timestamps of PLAN.md §59.5 step 4
#include "base/kaldi-error.h"
static void (*g_log)(const char*) = nullptr;
static void LogToApp(const kaldi::LogMessageEnvelope& e, const char* msg) {
    if (!g_log) return;
    std::string s = (e.func ? std::string(e.func) + ": " : std::string()) + (msg ? msg : "");
    g_log(s.c_str());
}
extern "C" __declspec(dllexport) void vosk_wp81_set_log_handler(void (*h)(const char*)) {
    g_log = h;
    kaldi::SetLogHandler(h ? &LogToApp : nullptr);
}
