// jsctest: JavaScriptCore alone in a DLL for apps/EngineTest (milestone 1a). The app calls jsctest_init once with
// the folder holding icudt77l.dat, then jsctest_eval for each script; results come back as UTF-8 text.

#include "config.h"

#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/JavaScript.h>
#include <JavaScriptCore/Options.h>
#include <cstdio>
#include <unicode/putil.h>
#include <windows.h>

static JSGlobalContextRef context;

extern "C" __declspec(dllexport) int jsctest_init(const char* icuDataDirectory)
{
    u_setDataDirectory(icuDataDirectory);
    // Upstream leaves the RegExp JIT off on 32-bit; MetroSurf's patches make YARR's JIT work on Windows ARM.
    _putenv("JSC_useRegExpJIT=true");
    JSC::initialize();
    context = JSGlobalContextCreate(nullptr);
    return context ? 0 : -1;
}

extern "C" __declspec(dllexport) int jsctest_options(char* out, int capacity)
{
    return std::snprintf(out, capacity, "useJIT=%d useBaselineJIT=%d useDFGJIT=%d useRegExpJIT=%d useDOMJIT=%d jitMemory=%zu MB",
        JSC::Options::useJIT(), JSC::Options::useBaselineJIT(), JSC::Options::useDFGJIT(), JSC::Options::useRegExpJIT(),
        JSC::Options::useDOMJIT(), static_cast<size_t>(JSC::Options::jitMemoryReservationSize()) >> 20);
}

// Returns the length of the result text, or -1 when the script threw (the text is then the exception).
extern "C" __declspec(dllexport) int jsctest_eval(const char* script, char* out, int capacity, double* milliseconds)
{
    JSStringRef source = JSStringCreateWithUTF8CString(script);
    LARGE_INTEGER frequency, start, end;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    JSValueRef exception = nullptr;
    JSValueRef result = JSEvaluateScript(context, source, nullptr, nullptr, 1, &exception);
    QueryPerformanceCounter(&end);
    *milliseconds = static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    JSStringRelease(source);

    JSStringRef text = JSValueToStringCopy(context, exception ? exception : result, nullptr);
    int length = text ? static_cast<int>(JSStringGetUTF8CString(text, out, capacity)) - 1 : 0;
    if (text)
        JSStringRelease(text);
    return exception ? -1 : length;
}
