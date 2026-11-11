#include "config.h"
#include "Profiler.h"

#include "Engine.h"
#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <JavaScriptCore/ExecutableAllocator.h>
#include <windows.h>
#include <wtf/Activity.h>
#include <wtf/MonotonicTime.h>
#include <wtf/Threading.h>

// JavaScriptCore/bytecode/ICCensus.h, same DLL: declared here to keep the header out of the framework headers.
namespace JSC::ICCensus {
void start();
void stop();
void dump(void (*log)(const char*));
}

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace Lumia {

namespace {

constexpr unsigned maxMilliseconds = 30000;
constexpr unsigned maxSamples = 20000;
constexpr unsigned maxFrames = 32;
// A sample's first slot when the Web thread was not in a webkit.dll function the unwind data describes. The next
// slot, if any, is the nearest webkit.dll function found on the stack (scanStack): who called the system, who is
// running the JavaScript.
constexpr uint32_t inSystem = 0xFFFFFFFF; // another DLL: system calls, waits, the CRT
constexpr uint32_t inJIT = 0xFFFFFFFE; // JavaScriptCore's JIT code (baseline, DFG, FTL, thunks, regexps)
constexpr uint32_t inInterpreter = 0xFFFFFFFD; // webkit.dll code without unwind data: LLInt, the offline assembler
constexpr unsigned maxScanWords = 2048;
// Hang sampler: a Web thread task running for this long gets sampled, every hangSampleMs, until it ends or the samples
// run out (10 s), at most one report every hangReportGapMs.
constexpr unsigned hangThresholdMs = 500;
constexpr unsigned hangSampleMs = 10;
constexpr unsigned maxHangSamples = 1000;
constexpr unsigned hangReportGapMs = 5000;

DWORD s_webThreadId;
uintptr_t s_jitStart;
uintptr_t s_jitEnd;
uintptr_t s_webStackBase;
std::atomic<bool> s_running { false };

struct Module {
    uintptr_t base { 0 };
    uintptr_t end { 0 };
    const IMAGE_ARM_RUNTIME_FUNCTION_ENTRY* functions { nullptr };
    size_t functionCount { 0 };
};

Module thisModule()
{
    Module module;
    module.base = reinterpret_cast<uintptr_t>(&__ImageBase);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module.base + __ImageBase.e_lfanew);
    module.end = module.base + nt->OptionalHeader.SizeOfImage;
    auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    module.functions = reinterpret_cast<const IMAGE_ARM_RUNTIME_FUNCTION_ENTRY*>(module.base + directory.VirtualAddress);
    module.functionCount = directory.Size / sizeof(IMAGE_ARM_RUNTIME_FUNCTION_ENTRY);
    return module;
}

// Length in bytes of the function a .pdata entry describes (ARM exception data: packed in the entry itself, or the
// first word of its .xdata record), both in 2-byte units.
uint32_t functionLength(const Module& module, const IMAGE_ARM_RUNTIME_FUNCTION_ENTRY& entry)
{
    if (entry.UnwindData & 3)
        return ((entry.UnwindData >> 2) & 0x7FF) * 2;
    return (*reinterpret_cast<const uint32_t*>(module.base + entry.UnwindData) & 0x3FFFF) * 2;
}

// The .pdata entry of the function containing rva, or null (a gap: the offline assembler's interpreter, thunks).
// Lock-free and allocation-free: it runs while the Web thread is suspended, so it must not touch anything that
// thread could be holding (the heap, the loader's function tables).
const IMAGE_ARM_RUNTIME_FUNCTION_ENTRY* findFunction(const Module& module, uint32_t rva)
{
    size_t low = 0, high = module.functionCount;
    while (low < high) {
        size_t middle = (low + high) / 2;
        if ((module.functions[middle].BeginAddress & ~1u) <= rva)
            low = middle + 1;
        else
            high = middle;
    }
    if (!low)
        return nullptr;
    auto& entry = module.functions[low - 1];
    uint32_t begin = entry.BeginAddress & ~1u;
    return rva - begin < functionLength(module, entry) ? &entry : nullptr;
}

// True when `value` is a Thumb return address into a webkit.dll function: odd, and right after a BL/BLX (32-bit,
// immediate) or a BLX register (16-bit). Reading the code is safe: it is our own image.
const IMAGE_ARM_RUNTIME_FUNCTION_ENTRY* returnAddressFunction(const Module& module, uintptr_t value)
{
    if (!(value & 1))
        return nullptr;
    uintptr_t address = value & ~1u;
    if (address < module.base + 0x1004 || address >= module.end)
        return nullptr;
    auto* code = reinterpret_cast<const uint16_t*>(address);
    bool call = ((code[-2] & 0xF800) == 0xF000 && (code[-1] & 0xC000) == 0xC000) || (code[-1] & 0xFF87) == 0x4780;
    return call ? findFunction(module, static_cast<uint32_t>(address - 2 - module.base)) : nullptr;
}

// The nearest webkit.dll caller of code that cannot be unwound (another DLL without our unwind data, JIT code): the
// first word on the stack that is a return address into one of our functions. Bounded: the thread is suspended.
uint32_t scanStack(const Module& module, const CONTEXT& context)
{
    if (auto* entry = returnAddressFunction(module, context.Lr))
        return entry->BeginAddress & ~1u;
    auto* slot = reinterpret_cast<const uintptr_t*>(context.Sp);
    auto* end = reinterpret_cast<const uintptr_t*>(s_webStackBase);
    for (unsigned i = 0; slot < end && i < maxScanWords; ++slot, ++i) {
        if (auto* entry = returnAddressFunction(module, *slot))
            return entry->BeginAddress & ~1u;
    }
    return 0;
}

// One stack: function starts (module offsets), innermost first. Runs with the Web thread suspended.
unsigned walkStack(const Module& module, CONTEXT& context, uint32_t* frames)
{
    unsigned count = 0;
    uintptr_t pc = context.Pc & ~1u;
    bool inModule = pc >= module.base && pc < module.end;
    if (!inModule || !findFunction(module, static_cast<uint32_t>(pc - module.base))) {
        frames[count++] = !inModule ? (pc >= s_jitStart && pc < s_jitEnd ? inJIT : inSystem) : inInterpreter;
        if (uint32_t caller = scanStack(module, context))
            frames[count++] = caller;
        return count;
    }
    for (bool innermost = true; count < maxFrames; innermost = false) {
        // A return address points after the call: look up the call itself, which may be a function's last
        // instruction.
        uintptr_t lookup = innermost ? pc : pc - 2;
        if (lookup < module.base || lookup >= module.end)
            break;
        auto* entry = findFunction(module, static_cast<uint32_t>(lookup - module.base));
        if (!entry)
            break;
        frames[count++] = entry->BeginAddress & ~1u;

        DWORD sp = context.Sp;
        PVOID handlerData = nullptr;
        DWORD establisherFrame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, static_cast<DWORD>(module.base), static_cast<DWORD>(lookup),
            const_cast<PRUNTIME_FUNCTION>(entry), &context, &handlerData, &establisherFrame, nullptr);
        uintptr_t next = context.Pc & ~1u;
        // The stack only unwinds upwards and stays inside the thread's stack; anything else is a frame the unwind
        // data does not describe (JIT code, hand-written assembly), where the walk stops.
        if (context.Sp < sp || context.Sp >= s_webStackBase || (context.Sp == sp && next == pc) || !next)
            break;
        pc = next;
    }
    return count;
}

void report(const Module&, const std::vector<uint32_t>& frames, const std::vector<uint8_t>& counts, double seconds)
{
    unsigned samples = static_cast<unsigned>(counts.size());
    if (!samples) {
        log("profilo: nessun campione");
        return;
    }
    std::unordered_map<uint32_t, unsigned> self;
    std::unordered_map<uint32_t, unsigned> total;
    std::unordered_map<uint32_t, unsigned> callers;
    unsigned outside = 0;
    unsigned jit = 0;
    unsigned interpreter = 0;
    std::vector<uint32_t> seen;
    for (unsigned i = 0; i < samples; ++i) {
        const uint32_t* stack = &frames[i * maxFrames];
        unsigned count = counts[i];
        if (!count)
            continue;
        unsigned first = 0;
        if (stack[0] >= inInterpreter) {
            (stack[0] == inJIT ? jit : stack[0] == inInterpreter ? interpreter : outside)++;
            // The caller found on the stack counts as "incluso" only: the time is not its own.
            if (count > 1)
                callers[stack[1]]++;
            first = 1;
        }
        if (!first)
            self[stack[0]]++;
        // Inclusive: each function once per sample, however deep the recursion.
        seen.clear();
        for (unsigned j = first; j < count; ++j) {
            if (std::find(seen.begin(), seen.end(), stack[j]) != seen.end())
                continue;
            seen.push_back(stack[j]);
            total[stack[j]]++;
        }
    }

    log("profilo: %.1f s, %u campioni (uno ogni %.1f ms), JS compilato %.0f%%, interprete JS %.0f%%, sistema e attese %.0f%%",
        seconds, samples, seconds * 1000 / samples, 100.0 * jit / samples, 100.0 * interpreter / samples,
        100.0 * outside / samples);
    auto emit = [&](const char* title, const std::unordered_map<uint32_t, unsigned>& counts, size_t limit) {
        std::vector<std::pair<uint32_t, unsigned>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](auto& a, auto& b) { return a.second > b.second; });
        if (sorted.size() > limit)
            sorted.resize(limit);
        // Six per line: the log stays short, tools/profile.py splits them again.
        for (size_t i = 0; i < sorted.size(); i += 6) {
            char line[512];
            int length = snprintf(line, sizeof(line), "profilo %s:", title);
            for (size_t j = i; j < std::min(i + 6, sorted.size()) && length < static_cast<int>(sizeof(line)) - 24; ++j)
                length += snprintf(line + length, sizeof(line) - length, " %X %.1f%%", sorted[j].first, 100.0 * sorted[j].second / samples);
            log("%s", line);
        }
    };
    emit("proprio", self, 30);
    emit("incluso", total, 48);
    // Who was waiting, calling the system or running JavaScript, by its nearest webkit.dll caller.
    emit("chiamante", callers, 24);
}

void run(unsigned milliseconds)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, s_webThreadId);
    if (!thread) {
        log("profilo: OpenThread fallita (%u)", static_cast<unsigned>(GetLastError()));
        s_running = false;
        return;
    }
    Module module = thisModule();
    s_jitStart = JSC::startOfFixedExecutableMemoryPool<uintptr_t>();
    s_jitEnd = JSC::endOfFixedExecutableMemoryPool<uintptr_t>();
    // Everything the sampling loop writes is allocated here: nothing may allocate while the Web thread is suspended.
    std::vector<uint32_t> frames(maxSamples * maxFrames);
    std::vector<uint8_t> counts;
    counts.reserve(maxSamples);

    log("profilo: avvio, %u ms sul thread Web", milliseconds);
    JSC::ICCensus::start();
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < milliseconds && counts.size() < maxSamples) {
        if (SuspendThread(thread) == static_cast<DWORD>(-1))
            break;
        CONTEXT context { };
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        unsigned count = 0;
        if (GetThreadContext(thread, &context))
            count = walkStack(module, context, &frames[counts.size() * maxFrames]);
        ResumeThread(thread);
        counts.push_back(static_cast<uint8_t>(count));
        Sleep(1);
    }
    double seconds = (GetTickCount64() - start) / 1000.0;
    CloseHandle(thread);
    JSC::ICCensus::stop();
    report(module, frames, counts, seconds);
    JSC::ICCensus::dump([](const char* line) { log("%s", line); });
    s_running = false;
}

// A freeze on the phone explains itself: the task the Web thread has been running for half a second (a dispatched
// function or a timer, as the activity recorder sees them) is sampled like lw_profile does, and the samples go to the
// log as a profile that tools/profile.py names. Polls four times a second; never samples while lw_profile does. Only
// while the tracer runs (the recorder publishes task starts only then).
void sampleHangs()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, s_webThreadId);
    if (!thread) {
        log("blocco: OpenThread fallita (%u), niente campionamento dei blocchi", static_cast<unsigned>(GetLastError()));
        return;
    }
    Module module = thisModule();
    // Allocated once: nothing may allocate while the Web thread is suspended.
    std::vector<uint32_t> frames(maxHangSamples * maxFrames);
    std::vector<uint8_t> counts;
    counts.reserve(maxHangSamples);
    MonotonicTime sampledTask;
    MonotonicTime lastReport;
    for (;;) {
        Sleep(250);
        auto taskStart = WTF::ActivityRecorder::currentTaskStart();
        auto now = MonotonicTime::now();
        if (!taskStart || taskStart == sampledTask || s_running.load()
            || (now - taskStart).milliseconds() < hangThresholdMs
            || (lastReport && (now - lastReport).milliseconds() < hangReportGapMs))
            continue;
        sampledTask = taskStart;
        s_jitStart = JSC::startOfFixedExecutableMemoryPool<uintptr_t>();
        s_jitEnd = JSC::endOfFixedExecutableMemoryPool<uintptr_t>();
        counts.clear();
        auto sampling = MonotonicTime::now();
        while (counts.size() < maxHangSamples && WTF::ActivityRecorder::currentTaskStart() == taskStart && !s_running.load()) {
            if (SuspendThread(thread) == static_cast<DWORD>(-1))
                break;
            CONTEXT context { };
            context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            unsigned count = 0;
            if (GetThreadContext(thread, &context))
                count = walkStack(module, context, &frames[counts.size() * maxFrames]);
            ResumeThread(thread);
            counts.push_back(static_cast<uint8_t>(count));
            Sleep(hangSampleMs);
        }
        now = MonotonicTime::now();
        lastReport = now;
        bool stillRunning = WTF::ActivityRecorder::currentTaskStart() == taskStart;
        log("blocco: task del thread Web di %.0f ms%s; campionato dal %.0f° ms",
            (now - taskStart).milliseconds(), stillRunning ? " (ancora in corso)" : "", (sampling - taskStart).milliseconds());
        report(module, frames, counts, (now - sampling).seconds());
    }
}

} // namespace

void setProfiledThread()
{
    s_webThreadId = GetCurrentThreadId();
    s_webStackBase = reinterpret_cast<uintptr_t>(reinterpret_cast<NT_TIB*>(NtCurrentTeb())->StackBase);
    Thread::create("HangSampler"_s, [] { sampleHangs(); })->detach();
}

void startProfile(unsigned milliseconds)
{
    if (!s_webThreadId || s_running.exchange(true))
        return;
    milliseconds = std::min(std::max(milliseconds, 100u), maxMilliseconds);
    Thread::create("Profiler"_s, [milliseconds] { run(milliseconds); })->detach();
}

} // namespace Lumia
