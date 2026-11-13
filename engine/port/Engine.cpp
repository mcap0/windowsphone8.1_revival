// The C API of lumiaweb.h. The Web thread owns WTF's main thread: every WebCore object lives there, and every API
// call only posts a task to it (or, for input from phase 3, to the scrolling thread), so the app never waits.
#include "config.h"
#include "Engine.h"
#include "Authentication.h"
#include "Blocker.h"
#include "media/MediaPlayerPrivateWP81.h"
#include "Permissions.h"
#include "AcceleratedSurface.h"
#include "Profiler.h"

#include "LayerTreeHost.h"
#include "PlatformStrategies.h"
#include "ScrollingTreeDispatcher.h"
#include "SocketStreamHandleImplCurl.h"
#include "ServiceWorkers.h"
#include "Storage.h"
#include "ThreadedCompositor.h"
#include "WebCoreInstanceHandle.h"
#include "WebPage.h"
#include <JavaScriptCore/ExecutableAllocator.h>
#include <JavaScriptCore/HeapInlines.h>
#include <JavaScriptCore/InitializeThreading.h>
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <WebCore/BackForwardController.h>
#include <WebCore/BitmapTexture.h>
#include <WebCore/CommonVM.h>
#include <WebCore/CoordinatedTileBuffer.h>
#include <WebCore/MemoryCache.h>
#include <WebCore/MemoryRelease.h>
#include <WebCore/NetworkStateNotifier.h>
#include <WebCore/CommonAtomStrings.h>
#include <WebCore/CurlContext.h>
#include <WebCore/CurlRequestScheduler.h>
#include <WebCore/DOMWrapperWorld.h>
#include <WebCore/Document.h>
#include <WebCore/DocumentFullscreen.h>
#include <WebCore/DocumentView.h>
#include <WebCore/Editor.h>
#include <WebCore/EditorClient.h>
#include <WebCore/EventHandler.h>
#include <WebCore/EventLoop.h>
#include <WebCore/ContainerNodeInlines.h>
#include <WebCore/ElementInlines.h>
#include <WebCore/HitTestResult.h>
#include <WebCore/NodeInlines.h>
#include <WebCore/RenderObjectInlines.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/HTMLInputElement.h>
#include <WebCore/HTMLTextFormControlElement.h>
#include <WebCore/HandleUserInputEventResult.h>
#include <WebCore/PlatformKeyboardEvent.h>
#include <WebCore/HTMLNames.h>
#include <wtf/text/MakeString.h>
#include <WebCore/PlatformMouseEvent.h>
#include <WebCore/PlatformTouchEvent.h>
#include <WebCore/PlatformTouchPoint.h>
#include <WebCore/RemoteFrameGeometryTransformer.h>
#include <wtf/Condition.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <JavaScriptCore/JSCConfig.h>
#include <JavaScriptCore/JSCast.h>
#include <JavaScriptCore/VMEntryScope.h>
#include <JavaScriptCore/VMTraps.h>
#include <WebCore/BackForwardCache.h>
#include <WebCore/FrameSelection.h>
#include <WebCore/JSDOMGlobalObject.h>
#include "BackForwardList.h"
#include "BytecodeCache.h"
#include "DiskCache.h"
#include "Downloads.h"
#include "EditorClient.h"
#include "VisitedLinks.h"
#include <WebCore/FileChooser.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/ProcessCapabilities.h>
#include <WebCore/ScriptController.h>
#include <WebCore/Settings.h>
#include <WebCore/TextIterator.h>
#include <WebCore/UserAgent.h>
#include <WebCore/VisibleSelection.h>
#include <WebCore/WebCoreJITOperations.h>
#include <WebCore/WindowProxy.h>
#include <csignal>
#include <atomic>
#include <roapi.h>
#include <string>
#include <unicode/putil.h>
#include <wtf/Activity.h>
#include <wtf/AvailableMemory.h>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/HexNumber.h>
#include <wtf/MainThread.h>
#include <wtf/MemoryFootprint.h>
#include <wtf/MemoryPressureHandler.h>
#include <wtf/RunLoop.h>
#include <wtf/Threading.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringView.h>

using namespace WebCore;

// JavaScriptCore/bytecode/ICCensus.h, same DLL: declared here to keep the header out of the framework headers.
namespace JSC::ICCensus {
bool describeHandler(uintptr_t pc, char* out, size_t outSize);
}

struct lw_view {
    RefPtr<WebKit::WebPage> page; // Web thread only, like everything below but `leaving` and `dialog`
    String userAgent;
    bool visible { true };
    // Set before the page is made, never changed.
    bool isPrivate { false };
    // lw_view_find: a new text searches from the current match on, the same text from the one after it.
    String lastFindText;
    // A command that leaves the page (Lumia::Leaving), set by the app's thread and cleared when the command runs: the
    // leave trap reads it to end the old page's script (leaveTrapCallback).
    std::atomic<int> leaving { 0 };
    // The page's alert/confirm/prompt: the Web thread waits here for lw_dialog_reply (Lumia::runDialog).
    struct Dialog {
        Lock lock;
        Condition condition;
        int id WTF_GUARDED_BY_LOCK(lock) { 0 };
        bool answered WTF_GUARDED_BY_LOCK(lock) { false };
        bool accepted WTF_GUARDED_BY_LOCK(lock) { false };
        String value WTF_GUARDED_BY_LOCK(lock);
    } dialog;
    // window.open: the Web thread waits here for lw_window_reply (Lumia::createWindow).
    struct Window {
        Lock lock;
        Condition condition;
        int id WTF_GUARDED_BY_LOCK(lock) { 0 };
        bool answered WTF_GUARDED_BY_LOCK(lock) { false };
        lw_view* view WTF_GUARDED_BY_LOCK(lock) { nullptr };
        void* panel WTF_GUARDED_BY_LOCK(lock) { nullptr };
        int width WTF_GUARDED_BY_LOCK(lock) { 0 };
        int height WTF_GUARDED_BY_LOCK(lock) { 0 };
        float scale WTF_GUARDED_BY_LOCK(lock) { 1 };
    } window;
};

namespace Lumia {

void leaveTrapCallback(JSC::VM&);

namespace {

// lw_init's config, with its strings copied: the app's may not outlive the call.
struct Config {
    lw_config config { };
    std::wstring resourceDirectory, dataDirectory, cacheDirectory, userAgent, downloadDirectory;
};

Config& globalConfig()
{
    static Config config;
    return config;
}

const wchar_t* copyString(std::wstring& storage, const wchar_t* string)
{
    if (!string)
        return nullptr;
    storage = string;
    return storage.c_str();
}

String toString(const wchar_t* string)
{
    if (!string)
        return { };
    return String(std::span(reinterpret_cast<const char16_t*>(string), wcslen(string)));
}

// Tasks posted before the Web thread has its run loop wait here, in order.
Lock s_webThreadLock;
bool s_webThreadReady WTF_GUARDED_BY_LOCK(s_webThreadLock) { false };
Vector<Function<void()>>& pendingTasks() WTF_REQUIRES_LOCK(s_webThreadLock)
{
    static NeverDestroyed<Vector<Function<void()>>> tasks;
    return tasks;
}

} // namespace

void postToWebThread(Function<void()>&& task, bool input)
{
    {
        Locker locker { s_webThreadLock };
        if (!s_webThreadReady) {
            pendingTasks().append(WTF::move(task));
            return;
        }
    }
    RunLoop::mainSingleton().dispatch(input ? RunLoop::Priority::Input : RunLoop::Priority::Default, WTF::move(task));
}

namespace {

// Every live view, for lw_suspend and lw_memory_pressure. Web thread.
HashSet<lw_view*>& views()
{
    static NeverDestroyed<HashSet<lw_view*>> views;
    return views;
}

// Between lw_suspend and lw_resume. Web thread.
bool s_suspended;

// A view's page renders, and is visible to its scripts (document.visibilityState), only while the view is shown and
// the app runs: its compositor is paused exactly while this is false (lw_view_set_visible, lw_suspend, lw_resume).
bool isActive(const lw_view& view)
{
    return view.visible && !s_suspended;
}

void setActive(lw_view& view, bool active)
{
    if (!view.page)
        return;
    // Hidden, the page gets visibilitychange, and requestAnimationFrame, CSS animations and media stop.
    if (auto* page = view.page->corePage())
        page->setIsVisible(active);
    if (auto* layerTreeHost = view.page->layerTreeHost()) {
        if (active)
            layerTreeHost->resumeRendering();
        else
            layerTreeHost->pauseRendering();
    }
    // The fingers now act on this tab: its scale and zoom limits.
    if (active)
        view.page->updateCompositorZoom();
}

// Runs its function when the last reference goes, on whichever thread drops it: lw_suspend hands one to each piece of
// work it waits for.
class Barrier final : public ThreadSafeRefCounted<Barrier> {
public:
    static Ref<Barrier> create(Function<void()>&& function) { return adoptRef(*new Barrier(WTF::move(function))); }
    ~Barrier() { m_function(); }

private:
    explicit Barrier(Function<void()>&& function)
        : m_function(WTF::move(function))
    {
    }

    Function<void()> m_function;
};

// Runs the function once every document of these views has run the tasks queued on its event loop so far (the
// visibilitychange event that setIsVisible has just queued), or after the timeout if a page holds its event loop (a
// suspended document). Web thread.
void afterQueuedPageTasks(const Vector<lw_view*>& views, Seconds timeout, Function<void()>&& function)
{
    struct Pending : RefCounted<Pending> {
        explicit Pending(Function<void()>&& function)
            : function(WTF::move(function))
        {
        }
        void run()
        {
            if (auto function = std::exchange(this->function, nullptr))
                function();
        }
        Function<void()> function;
    };
    Ref pending = adoptRef(*new Pending(WTF::move(function)));
    {
        auto tasksRan = Barrier::create([pending] { pending->run(); });
        for (auto* view : views) {
            if (auto* page = view->page ? view->page->corePage() : nullptr) {
                page->forEachDocument([&](Document& document) {
                    document.eventLoop().queueTask(TaskSource::UserInteraction, [tasksRan] { });
                });
            }
        }
    }
    RunLoop::mainSingleton().dispatchAfter(timeout, [pending] { pending->run(); });
}

// Where the memory is, for the log: the process's private bytes, the JavaScript heap (and what its objects hold
// outside it, like ArrayBuffers), the decoded images and other resources in the memory cache, and the tile buffers
// waiting for raster or upload. What none of these explain is GPU textures, layer backing and malloc'd WebCore data.
String memoryReport()
{
    constexpr size_t MB = 1024 * 1024;
    auto& heap = WebCore::commonVM().heap;
    auto cache = WebCore::MemoryCache::singleton().getStatistics();
    auto cacheBytes = [](const WebCore::MemoryCache::TypeStatistic& statistic) { return static_cast<size_t>(statistic.size); };
    size_t cacheTotal = cacheBytes(cache.images) + cacheBytes(cache.cssStyleSheets) + cacheBytes(cache.scripts) + cacheBytes(cache.fonts);
    return makeString(WTF::memoryFootprint() / MB, " MB privati: JS "_s, heap.size() / MB, " MB (capacita' "_s, heap.capacity() / MB,
        ", esterni "_s, heap.extraMemorySize() / MB, "), cache "_s, cacheTotal / MB, " MB (immagini "_s, cacheBytes(cache.images) / MB,
        ", decodificate "_s, static_cast<size_t>(cache.images.decodedSize) / MB, "), tile in attesa "_s,
        static_cast<size_t>(WebCore::CoordinatedTileBuffer::currentMemoryUsage()) / MB, " MB, texture GL "_s,
        WebCore::BitmapTexture::liveTextureCount(), " ("_s, WebCore::BitmapTexture::liveTextureBytes() / MB, " MB)"_s);
}

// What memoryReport() cannot attribute, from the address space itself: committed private memory by protection (malloc
// heaps and driver buffers are read-write, JIT code executable), mapped and image memory, and the largest private
// allocations, whose sizes and protections tell their owners apart (lw_memory_report).
String addressSpaceReport()
{
    constexpr size_t MB = 1024 * 1024;
    struct Allocation {
        uintptr_t base;
        size_t committed;
        DWORD protection;
    };
    size_t readWrite = 0, executable = 0, otherPrivate = 0, mapped = 0, image = 0, reserved = 0;
    Vector<Allocation> allocations;
    MEMORY_BASIC_INFORMATION info;
    for (uintptr_t address = 0; VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)); ) {
        uintptr_t next = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
        if (next <= address)
            break;
        address = next;
        if (info.State == MEM_RESERVE)
            reserved += info.RegionSize;
        if (info.State != MEM_COMMIT)
            continue;
        if (info.Type == MEM_MAPPED) {
            mapped += info.RegionSize;
            continue;
        }
        if (info.Type == MEM_IMAGE) {
            image += info.RegionSize;
            continue;
        }
        DWORD protection = info.Protect & 0xff;
        if (protection & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
            executable += info.RegionSize;
        else if (protection & (PAGE_READWRITE | PAGE_WRITECOPY))
            readWrite += info.RegionSize;
        else
            otherPrivate += info.RegionSize;
        auto base = reinterpret_cast<uintptr_t>(info.AllocationBase);
        if (allocations.isEmpty() || allocations.last().base != base)
            allocations.append({ base, 0, protection });
        allocations.last().committed += info.RegionSize;
    }
    std::sort(allocations.begin(), allocations.end(), [](auto& a, auto& b) { return a.committed > b.committed; });
    StringBuilder builder;
    builder.append("privata "_s, (readWrite + executable + otherPrivate) / MB, " MB (lettura-scrittura "_s, readWrite / MB,
        ", eseguibile "_s, executable / MB, ", altra "_s, otherPrivate / MB, ") in "_s, allocations.size(), " allocazioni, mappata "_s,
        mapped / MB, " MB, moduli "_s, image / MB, " MB, riservata "_s, reserved / MB, " MB; le piu' grandi:"_s);
    for (size_t i = 0; i < std::min<size_t>(allocations.size(), 16); ++i) {
        auto& allocation = allocations[i];
        builder.append(' ', allocation.committed / 1024, " KB@"_s, hex(allocation.base), '/', hex(allocation.protection));
    }
    return builder.toString();
}

// The monitor asks for a release at every poll while the footprint is over a threshold. Done every time, a page whose
// memory is its own (claude.ai at 600 MB, nothing to give back) lost its JIT code, style resolvers, font and decoded
// image caches every 2 s and rebuilt them at once: jank and CPU for nothing. A poll's release (Synchronous::No) runs
// only when it can help: on crossing into a level, once the footprint has grown a step above its lowest point since
// the last release, within 10% of the limit, or 30 s after the last one. Explicit requests (suspension, the OS warning,
// shrinkOrDie) are synchronous and always run. Web thread.
bool shouldRunPeriodicRelease(Critical critical, size_t footprint, size_t limit)
{
    static MonotonicTime lastPoll;
    static MonotonicTime lastRelease;
    static size_t lowestSinceRelease;
    static bool lastReleaseWasCritical;

    auto now = MonotonicTime::now();
    // The monitor polls every 2 s while over a threshold and not at all under it: a gap means the footprint has just
    // come back over.
    bool crossedThreshold = !lastPoll || now - lastPoll > 5_s;
    lastPoll = now;
    lowestSinceRelease = std::min(lowestSinceRelease, footprint);
    size_t step = std::max<size_t>(limit / 20, 32 * 1024 * 1024);
    bool release = crossedThreshold
        || (critical == Critical::Yes && !lastReleaseWasCritical)
        || footprint > lowestSinceRelease + step
        || footprint > limit / 10 * 9
        || now - lastRelease >= 30_s;
    if (!release)
        return false;
    lastRelease = now;
    lowestSinceRelease = footprint;
    lastReleaseWasCritical = critical == Critical::Yes;
    return true;
}

// WebKit's own memory policy, measured every 2 s against the limit the OS gives this app (AppMemoryUsageLimit):
// over half of it, non-critical caches go (dead resources, layout caches); over 70%, critical release (memory
// cache, back/forward cache, a full garbage collection). Without a handler releaseMemory did nothing at all, and pages
// grew until the OS killed the app.
void installMemoryMonitor()
{
    auto& handler = MemoryPressureHandler::singleton();
    handler.setLowMemoryHandler([](Critical critical, Synchronous synchronous) {
        size_t limit = config().memory_limit;
        if (synchronous == Synchronous::No && limit && !shouldRunPeriodicRelease(critical, WTF::memoryFootprint(), limit))
            return;
        auto before = memoryReport();
        // The back/forward cache holds a whole page nobody sees (YouTube: hundreds of MB): the first memory to go.
        BackForwardCache::singleton().pruneToSizeNow(0, PruningReason::MemoryPressure);
        WebCore::releaseMemory(critical, synchronous);
        // The tile pixel buffers kept for the next frame of an animation.
        WebCore::CoordinatedUnacceleratedTileBuffer::releasePooledPixels();
        Lumia::log("memoria: rilascio %s, prima %s", critical == Critical::Yes ? "critico" : "normale", before.utf8().data());
    });
    if (size_t limit = config().memory_limit) {
        MemoryPressureHandler::Configuration configuration;
        configuration.baseThreshold = limit;
        configuration.conservativeThresholdFraction = 0.5;
        configuration.strictThresholdFraction = 0.7;
        configuration.pollInterval = 2_s;
        handler.setConfiguration(WTF::move(configuration));
    }
    handler.setShouldUsePeriodicMemoryMonitor(true);
}

// lw_trace_enable: read by the input thread on every pan.
std::atomic<bool> s_traceEnabled { false };

// F2.c: the line the Web thread's activity recorder hands to the log (see wtf/Activity.h).
void activityObserve(const char* line)
{
    Lumia::log("%s", line);
}

// std::abort() (WTF's CRASH() and RELEASE_ASSERT in this configuration, std::terminate) ends in __fastfail, which
// no exception handler sees: the SIGABRT handler runs first and logs the call stack as webkit.dll offsets
// (symbolized on the PC with the link map).
void onAbort(int)
{
    void* frames[40];
    USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    uintptr_t base = reinterpret_cast<uintptr_t>(WebCore::instanceHandle());
    char line[1024];
    int length = snprintf(line, sizeof(line), "ABORT thread %u, pila (webkit.dll+):", static_cast<unsigned>(GetCurrentThreadId()));
    for (USHORT i = 0; i < count && length < static_cast<int>(sizeof(line)) - 16; ++i) {
        uintptr_t address = reinterpret_cast<uintptr_t>(frames[i]);
        if (address >= base && address < base + 0x4000000)
            length += snprintf(line + length, sizeof(line) - length, " %X", static_cast<unsigned>(address - base));
        else
            length += snprintf(line + length, sizeof(line) - length, " [%08X]", static_cast<unsigned>(address));
    }
    Lumia::log("%s", line);

    // The ARM unwinder stops inside the CRT: scan the stack for return addresses into webkit.dll's code instead
    // (Thumb, so odd; some are stale, the call chain stands out once symbolized).
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    auto* top = static_cast<uintptr_t*>(tib->StackBase);
    auto* slot = reinterpret_cast<uintptr_t*>(&frames[0]);
    uintptr_t codeStart = base + 0x1000;
    uintptr_t codeEnd = base + 0x2D30000; // .text (webkit.map)
    length = snprintf(line, sizeof(line), "ABORT pila (scansione):");
    unsigned found = 0;
    for (; slot < top && found < 60 && length < static_cast<int>(sizeof(line)) - 12; ++slot) {
        uintptr_t value = *slot;
        if ((value & 1) && value >= codeStart && value < codeEnd) {
            length += snprintf(line + length, sizeof(line) - length, " %X", static_cast<unsigned>(value - base - 1));
            ++found;
        }
    }
    Lumia::log("%s", line);
}

// A fault inside JIT code: which handler IC stub it was (JSC records them, bytecode/ICCensus.h) and the registers.
// Only logs; the process still dies as before.
LONG CALLBACK onJITFault(EXCEPTION_POINTERS* info)
{
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    uintptr_t pc = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    if (pc < JSC::startOfFixedExecutableMemoryPool<uintptr_t>() || pc >= JSC::endOfFixedExecutableMemoryPool<uintptr_t>())
        return EXCEPTION_CONTINUE_SEARCH;
    static std::atomic<unsigned> reported;
    if (reported++ >= 3)
        return EXCEPTION_CONTINUE_SEARCH;
    char what[160];
    if (!JSC::ICCensus::describeHandler(pc, what, sizeof(what)))
        snprintf(what, sizeof(what), "codice JIT non di un gestore IC");
    Lumia::log("errore nel JIT a %08X: %s", static_cast<unsigned>(pc), what);
    const CONTEXT& c = *info->ContextRecord;
    Lumia::log("registri: r0 %08X r1 %08X r2 %08X r3 %08X r4 %08X r5 %08X r6 %08X r7 %08X r8 %08X r9 %08X r10 %08X r11 %08X r12 %08X sp %08X lr %08X",
        c.R0, c.R1, c.R2, c.R3, c.R4, c.R5, c.R6, c.R7, c.R8, c.R9, c.R10, c.R11, c.R12, c.Sp, c.Lr);
    // The instructions around the fault, to disassemble on the PC (Thumb-2, little endian halfwords).
    char bytes[200];
    int length = 0;
    const uint8_t* at = reinterpret_cast<const uint8_t*>((pc & ~1u) - 16);
    for (unsigned i = 0; i < 48; ++i)
        length += snprintf(bytes + length, sizeof(bytes) - length, "%02X", at[i]);
    Lumia::log("codice da %08X: %s", static_cast<unsigned>((pc & ~1u) - 16), bytes);
    return EXCEPTION_CONTINUE_SEARCH;
}

void webThreadMain()
{
    // F2.c: name this thread for the activity recorder; only named threads are measured (wtf/Activity.h).
    WTF::ActivityRecorder::setThreadTag("web");
    Lumia::setProfiledThread();
    // WinRT objects (the ANGLE property set of each tab's surface) are created on this thread.
    RoInitialize(RO_INIT_MULTITHREADED);
    // Before the first VM, which freezes JSC's configuration (lw_view_load_url and the other commands that leave a page).
    g_jscConfig.shellTimeoutCheckCallback = leaveTrapCallback;
    JSC::initialize();
    WTF::initializeMainThread();
    WebCore::initializeCommonAtomStrings();
    WebCore::populateJITOperations();
    WebKit::PlatformStrategies::initialize();
    // Tiles are rasterized by Skia on CPU workers (below normal priority) and uploaded by the compositor. GPU raster
    // would share one GL context across threads on ANGLE 2.1.8 at feature level 9_3: an A/B for phase 5, by the numbers.
    ProcessCapabilities::setCanUseAcceleratedBuffers(false);

    if (auto* resourceDirectory = config().resource_directory) {
        auto caBundle = FileSystem::pathByAppendingComponent(String(resourceDirectory), "cacert.pem"_s);
        if (FileSystem::fileExists(caBundle))
            CurlContext::singleton().sslHandle().setCACertPath(WTF::move(caBundle));
    }
    Lumia::initializeStorage();
    Lumia::initializeServiceWorkers();
    Lumia::DiskCache::initialize();
    Lumia::BytecodeCache::initialize();
    installMemoryMonitor();
    // Back is instant for the page just left (4c): one page, as WebKit on devices with this memory; released under
    // memory pressure and at suspension.
    BackForwardCache::singleton().setMaxSize(1);

    Vector<Function<void()>> tasks;
    {
        Locker locker { s_webThreadLock };
        tasks = std::exchange(pendingTasks(), { });
        s_webThreadReady = true;
    }
    for (auto& task : tasks)
        task();

    RunLoop::run();
}

void sendEvent(lw_view& view, const lw_event& event)
{
    auto& config = Lumia::config();
    if (config.callback)
        config.callback(config.context, &view, &event);
}

} // namespace

WebKit::WebPage* liveWebPage(lw_view* view)
{
    return views().contains(view) ? view->page.get() : nullptr;
}

const lw_config& config()
{
    return globalConfig().config;
}

void postEvent(lw_view& view, lw_event_type type, int64_t number, const String& text, const String& text2, int id)
{
    auto characters = text.charactersWithNullTermination();
    auto characters2 = text2.charactersWithNullTermination();
    lw_event event { };
    event.type = type;
    event.id = id;
    event.number = number;
    event.text = characters ? reinterpret_cast<const wchar_t*>(characters->span().data()) : L"";
    event.text2 = characters2 ? reinterpret_cast<const wchar_t*>(characters2->span().data()) : L"";
    sendEvent(view, event);
}

void log(const char* format, ...)
{
    auto& config = Lumia::config();
    if (!config.callback)
        return;
    char line[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    wchar_t wide[1024];
    if (!MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, 1024))
        return;
    // Trailing newline of WTF's lines.
    size_t length = wcslen(wide);
    while (length && (wide[length - 1] == L'\n' || wide[length - 1] == L'\r'))
        wide[--length] = 0;
    lw_event event { };
    event.type = LW_EVENT_TRACE;
    event.text = wide;
    event.text2 = L"";
    config.callback(config.context, nullptr, &event);
}

void postConsoleMessage(lw_view& view, int level, const String& message, const String& source)
{
    postEvent(view, LW_EVENT_CONSOLE, level, message, source);
}

void postFieldEvent(lw_view& view, lw_event_type type, const lw_field& field, const String& text)
{
    auto characters = text.charactersWithNullTermination();
    lw_field copy = field;
    copy.text = characters ? reinterpret_cast<const wchar_t*>(characters->span().data()) : L"";
    lw_event event { };
    event.type = type;
    event.number = copy.kind;
    event.field = &copy;
    sendEvent(view, event);
}

bool traceEnabled()
{
    return s_traceEnabled.load(std::memory_order_relaxed);
}

namespace {

// lw_set_site_user_agent: lower-case host -> agent, for the host and its subdomains. Web thread.
HashMap<String, String>& siteUserAgents()
{
    static NeverDestroyed<HashMap<String, String>> agents;
    return agents;
}

String siteUserAgent(const URL& url)
{
    auto& agents = siteUserAgents();
    if (agents.isEmpty() || !url.protocolIsInHTTPFamily())
        return { };
    String host = url.host().convertToASCIILowercase();
    while (!host.isEmpty()) {
        auto agent = agents.get(host);
        if (!agent.isNull())
            return agent;
        size_t dot = host.find('.');
        if (dot == notFound)
            break;
        host = host.substring(dot + 1);
    }
    return { };
}

} // namespace

String userAgent(const lw_view& view, const URL& url)
{
    if (!view.userAgent.isEmpty())
        return view.userAgent;
    return userAgent(url);
}

String userAgent(const URL& url)
{
    if (auto agent = siteUserAgent(url); !agent.isEmpty())
        return agent;
    static NeverDestroyed<String> defaultUserAgent = [] {
        auto userAgent = toString(config().user_agent);
        return userAgent.isEmpty() ? standardUserAgent() : userAgent;
    }();
    return defaultUserAgent;
}

// What navigator.platform says next to that agent: Safari's own values, so a page (or a captcha's consistency check)
// does not see an iPhone that calls itself Win32.
String navigatorPlatform(const lw_view& view, const URL& url)
{
    auto agent = userAgent(view, url);
    if (agent.contains("iPhone"_s))
        return "iPhone"_s;
    if (agent.contains("iPad"_s))
        return "iPad"_s;
    if (agent.contains("Macintosh"_s))
        return "MacIntel"_s;
    // The other identities the app can give a site ("ua" in the address box) say what their browsers say.
    if (agent.contains("Android"_s))
        return "Linux armv8l"_s;
    if (agent.contains("Linux"_s))
        return "Linux x86_64"_s;
    if (agent.contains("Windows NT 10"_s))
        return "Win32"_s;
    return { };
}

bool isViewActive(const lw_view& view)
{
    return isActive(view);
}

bool isViewPrivate(const lw_view& view)
{
    return view.isPrivate;
}

// A page put text on the engine's clipboard (shims/wp81shims.c): the app hears of it (LW_EVENT_CLIPBOARD), since only
// the app can show it to the user for the phone's own copy. Web thread.
static void clipboardTextChanged(const wchar_t* text)
{
    static constexpr size_t maximum = 65536;
    size_t length = wcsnlen(text, maximum);
    for (auto* view : views()) {
        if (isActive(*view)) {
            postEvent(*view, LW_EVENT_CLIPBOARD, 0, String(std::span<const char16_t> { reinterpret_cast<const char16_t*>(text), length }));
            return;
        }
    }
}

// ---- dialogs ----

std::optional<String> runDialog(lw_view& view, lw_dialog_kind kind, const String& message, const String& defaultValue)
{
    // One Web thread runs every tab: a dialog nobody can see (a hidden tab, the app in the background) would freeze
    // all of them, so it is dismissed at once, as browsers do with background tabs.
    if (!isActive(view)) {
        log("dialogo %d saltato: scheda nascosta o app sospesa", static_cast<int>(kind));
        return std::nullopt;
    }
    static int lastDialogID;
    int id = ++lastDialogID;
    {
        Locker locker { view.dialog.lock };
        view.dialog.id = id;
        view.dialog.answered = false;
        view.dialog.accepted = false;
        view.dialog.value = { };
    }
    auto start = MonotonicTime::now();
    postEvent(view, LW_EVENT_DIALOG, kind, message, defaultValue, id);
    // The page's own run loop stays stopped, as with a modal panel: none of its timers or events may run while a
    // script is inside alert(). Scrolling and the compositor have threads of their own and go on.
    Locker locker { view.dialog.lock };
    while (!view.dialog.answered)
        view.dialog.condition.wait(view.dialog.lock);
    view.dialog.id = 0;
    log("dialogo %d chiuso dopo %.0f ms: %s", static_cast<int>(kind), (MonotonicTime::now() - start).milliseconds(),
        view.dialog.accepted ? "ok" : "annullato");
    if (!view.dialog.accepted)
        return std::nullopt;
    return view.dialog.value.isolatedCopy();
}

RefPtr<Page> createWindow(lw_view& opener, const URL& url, const String& referrer)
{
    // As with dialogs: a tab nobody sees gets no window (the page sees a blocked popup).
    if (!isActive(opener)) {
        log("finestra saltata: scheda nascosta o app sospesa");
        return nullptr;
    }
    static int lastWindowID;
    int id = ++lastWindowID;
    {
        Locker locker { opener.window.lock };
        opener.window.id = id;
        opener.window.answered = false;
        opener.window.view = nullptr;
        opener.window.panel = nullptr;
    }
    auto start = MonotonicTime::now();
    postEvent(opener, LW_EVENT_NEW_WINDOW, 1, url.string(), referrer, id);
    lw_view* view = nullptr;
    void* panel = nullptr;
    IntSize size;
    float scale = 1;
    {
        Locker locker { opener.window.lock };
        auto deadline = start + 3_s;
        while (!opener.window.answered) {
            auto now = MonotonicTime::now();
            if (now >= deadline)
                break;
            opener.window.condition.waitFor(opener.window.lock, deadline - now);
        }
        // From here a late lw_window_reply finds no window waiting.
        opener.window.id = 0;
        if (!opener.window.answered) {
            log("finestra %d: l'app non ha risposto in 3 s", id);
            return nullptr;
        }
        view = opener.window.view;
        panel = opener.window.panel;
        size = { opener.window.width, opener.window.height };
        scale = opener.window.scale;
    }
    if (!view) {
        log("finestra %d: rifiutata dall'app", id);
        return nullptr;
    }
    // As lw_view_create, here on the Web thread already: WebCore loads the page's URL into it and sets its opener. A
    // private tab's windows are private (they share its cookies).
    view->isPrivate = opener.isPrivate;
    views().add(view);
    view->page = WebKit::WebPage::create(*view, panel, size, scale, view->isPrivate);
    if (!isActive(*view))
        setActive(*view, false);
    log("finestra %d: scheda pronta dopo %.0f ms per %s", id, (MonotonicTime::now() - start).milliseconds(), url.string().utf8().data());
    return view->page->corePage();
}

namespace {

// Any thread: a window.open still waiting gets no window (its opener is going away).
void refuseWindow(lw_view& view)
{
    Locker locker { view.window.lock };
    if (!view.window.id || view.window.answered)
        return;
    view.window.answered = true;
    view.window.view = nullptr;
    view.window.condition.notifyAll();
}

// Any thread: answers the view's open dialog, if `id` is it (0 = whichever is open).
void answerDialog(lw_view& view, int id, bool accepted, String&& value)
{
    Locker locker { view.dialog.lock };
    if (!view.dialog.id || view.dialog.answered || (id && id != view.dialog.id))
        return;
    view.dialog.answered = true;
    view.dialog.accepted = accepted;
    view.dialog.value = WTF::move(value);
    view.dialog.condition.notifyAll();
}

} // namespace

} // namespace Lumia

using namespace Lumia;

// WTF's log lines (WTFLogAlways, LOG_ERROR...): set in WTF/Assertions.cpp for WP81.
extern "C" WTF_EXPORT_PRIVATE void WTFSetLogHookForWP81(void (*)(const char*));
extern "C" void (*wp81ClipboardChanged)(const wchar_t*);

// libxml2 is built to live inside a DLL (LIBXML_STATIC_FOR_DLL): it frees each thread's state when the thread exits.
extern "C" int xmlDllMain(void* instance, unsigned long reason, void* reserved);

// The DLL's only entry point (WebCore's own DllMain is left out on WP81).
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH)
        WebCore::setInstanceHandle(instance);
    xmlDllMain(instance, reason, reserved);
    return TRUE;
}

// ---- engine ----

int lw_init(const lw_config* appConfig)
{
    static std::once_flag once;
    int result = -1;
    std::call_once(once, [&] {
        if (!appConfig)
            return;
        wp81ClipboardChanged = Lumia::clipboardTextChanged;
        Lumia::installBlocker();
        Lumia::installMediaEngine();
        auto& stored = globalConfig();
        stored.config = *appConfig;
        stored.config.resource_directory = copyString(stored.resourceDirectory, appConfig->resource_directory);
        stored.config.data_directory = copyString(stored.dataDirectory, appConfig->data_directory);
        stored.config.cache_directory = copyString(stored.cacheDirectory, appConfig->cache_directory);
        stored.config.user_agent = copyString(stored.userAgent, appConfig->user_agent);
        stored.config.download_directory = copyString(stored.downloadDirectory, appConfig->download_directory);

        if (appConfig->memory_limit)
            WTF::setAppMemoryLimitForWP81(static_cast<size_t>(appConfig->memory_limit));
        if (appConfig->resource_directory) {
            auto directory = toString(appConfig->resource_directory).utf8();
            u_setDataDirectory(directory.data());
        }
        // Upstream leaves the RegExp JIT off on 32-bit; MetroSurf's patches make YARR's JIT work on Windows ARM.
        _putenv("JSC_useRegExpJIT=true");
        // Two CPU raster workers instead of upstream's "half the cores" (= one here). They run below normal priority,
        // so with the Web thread busy they only take what the compositor leaves of the second core; with the Web
        // thread idle (most of a scroll) they raster on both cores and new tiles fill in about half the time. The
        // compositor never waits for them (CoordinatedBackingStoreTile::processPendingUpdates). A/B: set it to 1.
        _putenv("WEBKIT_SKIA_CPU_PAINTING_THREADS=2");
        // Tiles as wide as the screen (768 device px) instead of upstream's 256x256 CPU tiles: one draw call per tile,
        // and at 256 px a frame took 53-116 draws, about 0.14 ms each through ANGLE (7-10 ms of the 16.7). Uploads only
        // send the dirty rectangle, so a bigger tile does not mean a bigger upload. A/B: remove this line.
        _putenv("WEBKIT_LAYERS_TILE_SIZE=768x512");
        // Window without the D24S8 depth-stencil buffer (GLContext::getEGLConfig). A/B of its cost on the Adreno:
        // remove this line.
        _putenv("WEBKIT_EGL_STENCIL=0");
        // curl's default is 17 connections in all: a page with a few long-lived requests (event streams, captcha and
        // telemetry iframes) held enough of them that the next navigation, on any host, waited for good. Per host
        // curl stays at 6, as browsers do for HTTP/1 (HTTP/2 multiplexes on one connection anyway).
        _putenv("WEBKIT_CURL_MAX_TOTAL_CONNECTIONS=48");

        WTFSetLogHookForWP81([](const char* line) { Lumia::log("%s", line); });
        signal(SIGABRT, onAbort);
        AddVectoredExceptionHandler(1, onJITFault);
        WTF::initialize();
        // Normal priority (plan: scrolling and compositor above it, raster and helpers below).
    Thread::create("Web"_s, webThreadMain)->detach();
        result = 0;
    });
    return result;
}

// In order: the pages hear visibilitychange (where sites save their state, often into localStorage), then localStorage
// goes to disk and the memory the pages can rebuild is released; meanwhile each compositor finishes the frame it may be
// drawing. The GPU trim comes after all of it, and then `done`: until then the app holds its suspension deferral.
void lw_suspend(void (*done)(void*), void* context)
{
    postToWebThread([done, context] {
        auto start = MonotonicTime::now();
        auto suspension = Barrier::create([done, context, start] {
            WebKit::AcceleratedSurface::trimGPUMemory();
            log("sospensione: pronta in %.0f ms", (MonotonicTime::now() - start).milliseconds());
            if (done)
                done(context);
        });
        Vector<lw_view*> stopped;
        for (auto* view : views()) {
            if (isActive(*view))
                stopped.append(view);
        }
        s_suspended = true;
        for (auto* view : stopped) {
            setActive(*view, false);
            // After the frame the compositor may be drawing, it sends the GPU nothing more.
            if (auto* layerTreeHost = view->page ? view->page->layerTreeHost() : nullptr)
                layerTreeHost->dispatchAfterCurrentFrame([suspension] { });
        }
        afterQueuedPageTasks(stopped, 500_ms, [suspension] {
            Lumia::flushStorage([suspension] { });
            Lumia::flushServiceWorkers([suspension] { });
            Lumia::saveVisitedLinks();
            // The functions the pages compiled since the last commit: the app may not come back from this suspension.
            Lumia::BytecodeCache::commit("sospensione", [suspension] { });
            // As WebKit does when iOS suspends its Web process: caches, JIT code and decoded images go (the tiles keep
            // what is on screen) and a full collection runs, so the suspended app holds as little memory as it can and
            // the system is less likely to end it.
            MemoryPressureHandler::singleton().releaseMemory(Critical::Yes, Synchronous::Yes);
        });
    });
}

void lw_resume(void)
{
    postToWebThread([] {
        if (!std::exchange(s_suspended, false))
            return;
        for (auto* view : views()) {
            if (isActive(*view))
                setActive(*view, true);
        }
        log("ripresa");
    });
}

void lw_memory_pressure(int critical)
{
    // Synchronous: an explicit request, never skipped like the monitor's polls (installMemoryMonitor).
    postToWebThread([critical] {
        // The encoded functions waiting for a commit are memory too, and the data is worth keeping.
        Lumia::BytecodeCache::commit("memoria");
        MemoryPressureHandler::singleton().releaseMemory(critical ? Critical::Yes : Critical::No, Synchronous::Yes);
    });
}

void lw_network_changed(int online, int switched)
{
    postToWebThread([online = !!online, switched = !!switched] {
        log("rete: %s%s", online ? "connessa" : "assente",
            switched ? ", cambiata: le connessioni della rete di prima non si riusano, i WebSocket aperti si chiudono" : "");
        if (switched) {
            WebCore::CurlContext::singleton().scheduler().networkChanged();
            WebCore::SocketStreamHandleImpl::failAllForNetworkChange();
        }
        WebCore::NetworkStateNotifier::singleton().setOnLineFromApp(online);
    }, true);
}

void lw_clear_visited_links(void)
{
    postToWebThread([] {
        Lumia::clearVisitedLinks();
    });
}

void lw_clear_data(unsigned what)
{
    postToWebThread([what] {
        Lumia::clearData(what);
    });
}

void lw_set_option(int option, int value)
{
    postToWebThread([option, value] {
        switch (option) {
        case LW_OPTION_BLOCKER:
            Lumia::setBlockerEnabled(!!value);
            break;
        default:
            log("opzione sconosciuta: %d", option);
        }
    });
}

void lw_memory_report(void)
{
    postToWebThread([] {
        log("memoria: %s", memoryReport().utf8().data());
        log("memoria: %s", addressSpaceReport().utf8().data());
        log("%s", DiskCache::describe().utf8().data());
        log("%s", BytecodeCache::describe().utf8().data());
    });
}

void lw_trace_enable(int enabled)
{
    s_traceEnabled.store(!!enabled, std::memory_order_relaxed);
    // F2.c: the Web thread's run loop reports its long tasks and its periodic busy summary through WTF's own hook,
    // which ends up here in the same log. With the tracer off the hook is null and every entry point is one atomic
    // load, so the instrumentation costs nothing in normal use.
    WTF::ActivityRecorder::setObserver(enabled ? &activityObserve : nullptr);
}

namespace Lumia {

namespace {
struct TimelineEntry {
    float milliseconds;
    char kind;
};
constexpr unsigned timelineCapacity = 16384;
TimelineEntry s_timeline[timelineCapacity];
std::atomic<unsigned> s_timelineCount;
std::atomic<bool> s_timelineOn;
MonotonicTime s_timelineStart;
}

void timelineMark(char kind)
{
    if (!s_timelineOn.load(std::memory_order_relaxed))
        return;
    unsigned index = s_timelineCount.fetch_add(1, std::memory_order_relaxed);
    if (index < timelineCapacity)
        s_timeline[index] = { static_cast<float>((MonotonicTime::now() - s_timelineStart).milliseconds()), kind };
}

bool timelineRecording()
{
    return s_timelineOn.load(std::memory_order_relaxed);
}

void startGPUTest()
{
    if (s_timelineOn.exchange(true))
        return;
    s_timelineCount = 0;
    s_timelineStart = MonotonicTime::now();
    Thread::create("Timeline"_s, [] {
        ::Sleep(6000);
        s_timelineOn = false;
        ::Sleep(50);
        unsigned count = std::min(s_timelineCount.load(), timelineCapacity);
        std::sort(s_timeline, s_timeline + count, [](auto& a, auto& b) { return a.milliseconds < b.milliseconds; });
        log("linea: %u eventi in 6 s", count);
        StringBuilder line;
        for (unsigned i = 0; i < count; ++i) {
            line.append(s_timeline[i].kind, String::number(std::round(s_timeline[i].milliseconds * 10) / 10), ' ');
            if (line.length() > 400 || i + 1 == count) {
                log("linea: %s", line.toString().utf8().data());
                line.clear();
            }
        }
    })->detach();
}

} // namespace Lumia

void lw_gpu_test()
{
    Lumia::startGPUTest();
}

void lw_profile(int milliseconds)
{
    Lumia::startProfile(milliseconds > 0 ? static_cast<unsigned>(milliseconds) : 10000);
}

#if defined(WP81_PGO)
// engine/pgo/wp81profile.c, in clang_rt.profile.lib.
extern "C" int wp81ProfileWrite(int (*write)(void* context, const void* data, size_t size), void* context);
extern "C" void wp81ProfileReset(void);
extern "C" int wp81ProfileUploadBegin(void);
extern "C" int wp81ProfileUploadData(const void* data, size_t size);
extern "C" int wp81ProfileUploadEnd(void);
#endif

namespace JSC {
extern bool wp81LimitHandlerICs; // InlineCacheCompiler.cpp
String wp81ResolveReport(); // JSScope.cpp
}

namespace WebCore {
void setDevelopmentTrustworthyHostWP81(const String&); // SecurityOrigin.cpp
}

#if ENABLE(CSS_SELECTOR_JIT)
// "cssjit": the CSS selector JIT (WebCore/cssjit, on ARMv7 since this port brought it back) off or on, in every tab and
// in the tabs opened after. Off, style matching and querySelector take WebCore's interpreter (SelectorChecker): the
// page's cssSelectorJITCompilerEnabled setting, read by ElementRuleCollector, :has() and (WP81) SelectorQuery; selectors
// already compiled stay, unused. On at every start. Web thread.
static bool s_cssSelectorJITEnabled = true;

static void applyCSSSelectorJIT(WebKit::WebPage& page)
{
    if (auto* corePage = page.corePage())
        corePage->settings().setCSSSelectorJITCompilerEnabled(s_cssSelectorJITEnabled);
}
#endif

void lw_command(const char* command)
{
    postToWebThread([command = String::fromUTF8(command)] {
        if (command == "bytecode"_s)
            log("bytecode: %s per gli script da ora", Lumia::BytecodeCache::toggle() ? "acceso" : "spento");
        else if (command == "ic"_s) {
            JSC::wp81LimitHandlerICs = !JSC::wp81LimitHandlerICs;
            log("ic: %s per il codice compilato da ora", JSC::wp81LimitHandlerICs ? "casi limitati" : "tutti i casi");
        } else if (command == "scope"_s)
            log("%s", JSC::wp81ResolveReport().utf8().data());
#if defined(WP81_PGO)
        else if (command.startsWith("pgo"_s)) {
            // Only in the instrumented engine (ENGINE_OUT=out/webkit-pgo): "pgo" writes the counters gathered so far
            // to webkit.profraw in the data folder (tools/pgo fetches it), "pgo azzera" zeroes them.
            auto argument = command.substring(3).trim(isASCIIWhitespace);
            if (argument == "azzera"_s) {
                wp81ProfileReset();
                log("pgo: contatori azzerati");
            } else if (argument == "invia"_s) {
                // The file written by "pgo" (by this run or an earlier one) to the PC's tools/inbox, as /profraw.
                auto path = makeString(String(globalConfig().dataDirectory.c_str()), "\\webkit.profraw"_s);
                uint64_t sent = 0;
                bool failed = true;
                if (auto handle = FileSystem::openFile(path, FileSystem::FileOpenMode::Read)) {
                    failed = wp81ProfileUploadBegin();
                    Vector<uint8_t> buffer(256 * 1024);
                    while (!failed) {
                        auto read = handle.read(buffer.mutableSpan());
                        if (!read || !*read)
                            break;
                        failed = wp81ProfileUploadData(buffer.span().data(), static_cast<size_t>(*read));
                        sent += *read;
                    }
                    failed = wp81ProfileUploadEnd() || failed;
                }
                log("pgo: %s, %.1f MB", failed ? "profilo NON inviato" : "profilo inviato", sent / 1048576.0);
            } else {
                auto path = makeString(String(globalConfig().dataDirectory.c_str()), "\\webkit.profraw"_s);
                auto temporary = makeString(path, ".tmp"_s);
                uint64_t size = 0;
                bool written = false;
                if (auto handle = FileSystem::openFile(temporary, FileSystem::FileOpenMode::Truncate)) {
                    struct Sink { FileSystem::FileHandle& handle; uint64_t& size; } sink { handle, size };
                    written = !wp81ProfileWrite([](void* context, const void* data, size_t length) -> int {
                        auto& sink = *static_cast<Sink*>(context);
                        sink.size += length;
                        auto written = sink.handle.write(std::span { static_cast<const uint8_t*>(data), length });
                        return written && *written == length ? 0 : 1;
                    }, &sink);
                }
                written = written && FileSystem::moveFile(temporary, path);
                log("pgo: %s, %.1f MB", written ? "profilo scritto" : "profilo NON scritto", size / 1048576.0);
            }
        }
#endif
        else if (command.startsWith("blocco"_s)) {
            // "blocco": the ad and tracker blocker off or on; "blocco <host>": that site excluded; "blocco stato".
            Lumia::blockerCommand(command.substring(6).trim(isASCIIWhitespace));
        } else if (command == "sonda video"_s) {
            // "sonda video": the GPU questions of media/VideoProbe.cpp, answered in the log by the next tab's compositor.
            Lumia::requestVideoProbe();
            log("sonda video: alla prossima scheda");
        } else if (command == "sw"_s) {
            // "sw": the service workers that run and the loads they are answering.
            log("%s", Lumia::describeServiceWorkers().utf8().data());
        } else if (command == "cssjit"_s) {
#if ENABLE(CSS_SELECTOR_JIT)
            // Every tab's style is computed again at once, so a style the JIT got wrong shows (or goes) right away.
            s_cssSelectorJITEnabled = !s_cssSelectorJITEnabled;
            for (auto* view : views()) {
                if (!view->page)
                    continue;
                applyCSSSelectorJIT(*view->page);
                if (auto* corePage = view->page->corePage())
                    corePage->setNeedsRecalcStyleInAllFrames();
            }
            log("cssjit: selettori CSS compilati %s, stili ricalcolati", s_cssSelectorJITEnabled ? "accesi" : "spenti");
#else
            log("cssjit: non compilato in questo motore");
#endif
        } else if (command == "ritmo"_s) {
            // "ritmo": the pacing of rendering updates while a page loads (LayerTreeHost::loadPacingDelay) off or on,
            // for a bench of the same engine with and without.
            bool enabled = !Lumia::loadPacingEnabled();
            Lumia::setLoadPacingEnabled(enabled);
            log("ritmo: %s", enabled ? "acceso" : "spento");
        } else if (command.startsWith("sicuro"_s)) {
            // "sicuro 192.168.1.148": that host's http pages are a secure context until the app ends; "sicuro": none.
            auto host = command.substring(6).trim(isASCIIWhitespace);
            WebCore::setDevelopmentTrustworthyHostWP81(host);
            log("sicuro: %s", host.isEmpty() ? "nessun host" : host.utf8().data());
        } else
            log("comando sconosciuto: %s", command.utf8().data());
    });
}

// ---- views ----

// The app's commands (creating and sizing a view, navigation) go on the input lane, like taps: a heavy page keeps a
// hundred tasks of its own queued (parser chunks, network callbacks, scripts), and behind them the address typed on
// YouTube waited 6 s, 15 s on claude.ai's login. The input lane keeps their order (a view is created before its first
// load); lw_view_destroy stays behind the page's own work.

static lw_view* createView(void* swapChainPanel, int width, int height, float deviceScale, bool isPrivate)
{
    auto* view = new lw_view;
    view->isPrivate = isPrivate;
    postToWebThread([view, swapChainPanel, width, height, deviceScale] {
        views().add(view);
        view->page = WebKit::WebPage::create(*view, swapChainPanel, { width, height }, deviceScale, view->isPrivate);
#if ENABLE(CSS_SELECTOR_JIT)
        if (!s_cssSelectorJITEnabled)
            applyCSSSelectorJIT(*view->page);
#endif
        if (!isActive(*view))
            setActive(*view, false);
    }, true);
    return view;
}

lw_view* lw_view_create(void* swapChainPanel, int width, int height, float deviceScale)
{
    return createView(swapChainPanel, width, height, deviceScale, false);
}

lw_view* lw_view_create_private(void* swapChainPanel, int width, int height, float deviceScale)
{
    return createView(swapChainPanel, width, height, deviceScale, true);
}

void lw_view_destroy(lw_view* view)
{
    // A dialog or a window.open would keep the Web thread waiting for a tab that is gone.
    answerDialog(*view, 0, false, { });
    refuseWindow(*view);
    postToWebThread([view] {
        views().remove(view);
        cancelPasswordQuestions(*view);
        deviceFeaturesViewWillClose(*view);
        if (RefPtr page = std::exchange(view->page, nullptr))
            page->close();
        bool wasPrivate = view->isPrivate;
        delete view;
        if (wasPrivate) {
            bool anotherPrivate = false;
            for (auto* other : views())
                anotherPrivate = anotherPrivate || other->isPrivate;
            if (!anotherPrivate)
                Lumia::endPrivateSession();
        }
    });
}

void lw_view_resize(lw_view* view, int width, int height)
{
    postToWebThread([view, width, height] {
        if (view->page)
            view->page->resize({ width, height });
    }, true);
}

void lw_view_set_visible(lw_view* view, int visible)
{
    postToWebThread([view, visible = !!visible] {
        if (view->visible == visible)
            return;
        bool wasActive = isActive(*view);
        view->visible = visible;
        if (isActive(*view) != wasActive)
            setActive(*view, !wasActive);
    }, true);
}

void lw_view_set_user_agent(lw_view* view, const wchar_t* userAgent)
{
    postToWebThread([view, userAgent = toString(userAgent).isolatedCopy()] mutable {
        view->userAgent = WTF::move(userAgent);
    }, true);
}

// ---- leaving a busy page ----
// The commands above go on the input lane, but a lane only runs between tasks: inside one long task (YouTube's 9 s of
// startup script, 2026-10-28) the typed address waited until the task ended. A command that leaves the page also
// fires a JavaScriptCore trap from the app's thread; the Web thread takes it at the script's next loop or call, still
// inside that task, and leaveTrapCallback ends the script if it belongs to the page being left. The termination
// unwinds to the task's entry, as the JSC watchdog's does, and the next script runs normally (VM entry scope exit
// clears the request). The page is paused at that same moment (suspendLeavingDocuments: timers, event loop and
// microtasks of every frame), so the rest of its work queued in the task does not run either: Cloudflare's challenge is
// a chain of promise jobs, each a script of its own, and ending only the current one left reload waiting 16 s
// (2026-10-28). If the command does not take the page away after all, it runs again (resumeIfStaying).
// JSC's trap with a callback of its own is the jsc shell's timeout check; it is free in the engine, so the port uses it.

enum class Leaving : int { No, NewDocument, Back, Forward };

// Web thread, inside JavaScript (a polling trap check). Installed as g_jscConfig.shellTimeoutCheckCallback.
void Lumia::leaveTrapCallback(JSC::VM& vm)
{
    auto* globalObject = vm.entryScope ? vm.entryScope->globalObject() : nullptr;
    auto* domGlobalObject = globalObject ? dynamicDowncast<JSDOMGlobalObject>(globalObject) : nullptr;
    RefPtr document = domGlobalObject ? dynamicDowncast<Document>(domGlobalObject->scriptExecutionContext()) : nullptr;
    auto* page = document ? document->page() : nullptr;
    if (!page)
        return;
    for (auto* view : views()) {
        if (!view->page || view->page->corePage() != page)
            continue;
        auto leaving = static_cast<Leaving>(view->leaving.load());
        if (leaving == Leaving::No)
            return;
        if (leaving == Leaving::Back || leaving == Leaving::Forward) {
            // Back within the same document (pushState, #anchors) is the page's own navigation: its script stays.
            auto& list = view->page->backForwardList();
            RefPtr target = list.itemAtOffset(leaving == Leaving::Back ? -1 : 1);
            RefPtr current = list.currentItem();
            if (!target || (current && current->shouldDoSameDocumentNavigationTo(*target)))
                return;
        }
        log("uscita: interrompo lo script della pagina lasciata (%s)", document->url().host().utf8().data());
        vm.notifyNeedTermination();
        view->page->suspendLeavingDocuments();
        return;
    }
}

namespace {

// Any thread (the app's): marks the view as being left and asks the running script, if any, to check.
void requestLeave(lw_view* view, Leaving leaving)
{
    view->leaving.store(static_cast<int>(leaving));
    // A dialog the page is showing is answered "cancel": the user went elsewhere.
    answerDialog(*view, 0, false, { });
    if (auto* vm = WebCore::commonVMOrNull())
        vm->traps().fireTrap(JSC::VMTraps::NeedShellTimeoutCheck);
}

} // namespace

static RefPtr<LocalFrame> mainFrame(lw_view* view)
{
    return view->page ? view->page->localMainFrame() : nullptr;
}

// After a leaving command: a page the leave trap paused, which no navigation is taking away (a link to the same page,
// a download, an address that did not load), runs again.
static void resumeIfStaying(lw_view* view)
{
    RefPtr frame = mainFrame(view);
    if (frame && !frame->loader().provisionalDocumentLoader())
        view->page->resumeLeavingDocuments();
}

void lw_view_load_url(lw_view* view, const wchar_t* url)
{
    requestLeave(view, Leaving::NewDocument);
    postToWebThread([view, url = toString(url).isolatedCopy()] {
        view->leaving.store(0);
        if (!view->page)
            return;
        view->page->loadURL(url);
        resumeIfStaying(view);
    }, true);
}

void lw_view_reload(lw_view* view)
{
    requestLeave(view, Leaving::NewDocument);
    postToWebThread([view] {
        view->leaving.store(0);
        if (RefPtr frame = mainFrame(view)) {
            frame->loader().reload();
            resumeIfStaying(view);
        }
    }, true);
}

// Stop only stops loading: the page stays, and so does its script.
void lw_view_stop(lw_view* view)
{
    postToWebThread([view] {
        if (RefPtr frame = mainFrame(view))
            frame->loader().stopForUserCancel();
    }, true);
}

void lw_view_clear_site_data(lw_view* view)
{
    // Everything the current site stored, then the page again from the network as on a first visit.
    requestLeave(view, Leaving::NewDocument);
    postToWebThread([view] {
        view->leaving.store(0);
        if (!view->page)
            return;
        view->page->clearSiteDataAndReload();
        resumeIfStaying(view);
    }, true);
}

void lw_view_go_back(lw_view* view)
{
    requestLeave(view, Leaving::Back);
    postToWebThread([view] {
        view->leaving.store(0);
        if (auto* page = view->page ? view->page->corePage() : nullptr) {
            page->backForward().goBack();
            resumeIfStaying(view);
        }
    }, true);
}

void lw_view_go_forward(lw_view* view)
{
    requestLeave(view, Leaving::Forward);
    postToWebThread([view] {
        view->leaving.store(0);
        if (auto* page = view->page ? view->page->corePage() : nullptr) {
            page->backForward().goForward();
            resumeIfStaying(view);
        }
    }, true);
}

void lw_set_site_user_agent(const wchar_t* host, const wchar_t* userAgent)
{
    postToWebThread([host = toString(host).convertToASCIILowercase().isolatedCopy(), userAgent = toString(userAgent).isolatedCopy()] mutable {
        if (host.isEmpty())
            return;
        if (userAgent.isEmpty())
            siteUserAgents().remove(host);
        else
            siteUserAgents().set(host, WTF::move(userAgent));
    }, true);
}

void lw_view_find(lw_view* view, int id, const wchar_t* text, int backwards)
{
    postToWebThread([view, id, backwards, text = toString(text).isolatedCopy()] {
        auto* page = view->page ? view->page->corePage() : nullptr;
        if (!page)
            return;
        page->unmarkAllTextMatches();
        if (text.isEmpty()) {
            view->lastFindText = { };
            if (RefPtr frame = mainFrame(view))
                frame->selection().clear();
            postEvent(*view, LW_EVENT_FIND_RESULT, 0, { }, { }, id);
            return;
        }
        FindOptions options { FindOption::CaseInsensitive, FindOption::WrapAround };
        if (backwards)
            options.add(FindOption::Backwards);
        // Typing one more letter keeps the match under the selection if it still matches.
        if (text != std::exchange(view->lastFindText, text))
            options.add(FindOption::StartInSelection);
        constexpr unsigned maxMatches = 1000;
        unsigned matches = page->markAllMatchesForText(text, options, true, maxMatches);
        if (matches)
            page->findString(text, options);
        postEvent(*view, LW_EVENT_FIND_RESULT, matches, { }, { }, id);
    }, true);
}

void lw_view_run_script(lw_view* view, int id, const wchar_t* script)
{
    postToWebThread([view, id, script = toString(script).isolatedCopy()] {
        RefPtr frame = mainFrame(view);
        if (!frame) {
            postEvent(*view, LW_EVENT_SCRIPT_RESULT, 1, "no page"_s, { }, id);
            return;
        }
        auto& world = mainThreadNormalWorldSingleton();
        auto result = frame->script().executeUserAgentScriptInWorld(world, script, false);
        if (!result) {
            postEvent(*view, LW_EVENT_SCRIPT_RESULT, 1, result.error().message, { }, id);
            return;
        }
        auto* globalObject = frame->script().globalObject(world);
        auto text = globalObject ? result.value().toWTFString(globalObject) : String();
        postEvent(*view, LW_EVENT_SCRIPT_RESULT, 0, text, { }, id);
    });
}

// Back on the phone while a page element is fullscreen: the document leaves it (every level, as Esc on a PC), and the
// app hears LW_EVENT_FULLSCREEN 0 from the chrome client like for the page's own exitFullscreen().
void lw_view_exit_fullscreen(lw_view* view)
{
    if (!view)
        return;
    postToWebThread([view] {
        if (!liveWebPage(view))
            return;
        RefPtr frame = mainFrame(view);
        if (RefPtr document = frame ? frame->document() : nullptr)
            Ref { document->fullscreen() }->fullyExitFullscreen();
    }, true);
}

// "Save image" / "save link" of the app's menu: a download in the page's context (its cookies, its address as referrer).
void lw_view_download(lw_view* view, const wchar_t* url)
{
    if (!view || !url)
        return;
    postToWebThread([view, address = toString(url).isolatedCopy()] {
        if (!liveWebPage(view))
            return;
        RefPtr frame = mainFrame(view);
        RefPtr document = frame ? frame->document() : nullptr;
        if (!document)
            return;
        ResourceRequest request { URL { document->url(), address } };
        request.setFirstPartyForCookies(document->firstPartyForCookies());
        if (request.url().protocolIsInHTTPFamily() && document->url().protocolIsInHTTPFamily())
            request.setHTTPReferrer(document->url().strippedForUseAsReferrer().string);
        Lumia::startDownload(*view, frame->loader().networkingContext(), request, { });
    }, true);
}

// ---- input, text bar, replies: phase 3 (input surgery) and phase 4 (dialogs, files, downloads) ----

// ---- touch events for pages (phase 4) ----
//
// Every finger goes to the page as touchstart/touchmove/touchend (and, from them, WebCore's pointer events) on the Web
// thread's input lane, and nothing waits for the page: the scrolling thread decides about each pan from the page's
// event regions (ScrollingTreeDispatcher::beginTouchGesture) and holds it only where the page has a non-passive
// touchstart or touchmove listener under the finger, until the page answers (100 ms at most). A page that cancels
// touchstart or touchend gets no click either (the mouse compatibility events of the tap).
namespace {

class TouchPoint final : public PlatformTouchPoint {
public:
    TouchPoint(unsigned id, State state, FloatPoint position)
    {
        m_id = id;
        m_state = state;
        m_pos = DoublePoint(position.x(), position.y());
        m_screenPos = m_pos;
        m_radius = DoubleSize(10, 10);
        m_force = state == TouchReleased || state == TouchCancelled ? 0 : 0.5f;
    }
};

class TouchEvent final : public PlatformTouchEvent {
public:
    TouchEvent(PlatformEvent::Type type, Vector<PlatformTouchPoint>&& points, MonotonicTime time)
    {
        m_type = type;
        m_timestamp = time;
        m_touchPoints = WTF::move(points);
    }
};

struct TouchPointState {
    unsigned id;
    FloatPoint position;
    PlatformTouchPoint::State state;
};

// One touchmove: the input thread keeps adding moves to it until the Web thread takes it, so a busy page gets one
// touchmove with the latest positions instead of a backlog. Each event carries its own fingers, so a new gesture never
// waits for the Web thread to finish the last one.
struct TouchMoveBatch : ThreadSafeRefCounted<TouchMoveBatch> {
    Lock lock;
    Vector<TouchPointState> points WTF_GUARDED_BY_LOCK(lock);
    bool taken WTF_GUARDED_BY_LOCK(lock) { false };
};

// Input thread only (the app has one): the fingers down and the gesture they make.
struct InputTouches {
    unsigned generation { 0 };
    Vector<TouchPointState> points;
    RefPtr<TouchMoveBatch> openMove;
    bool pinchSuppressed { false };
};

InputTouches& inputTouches()
{
    static NeverDestroyed<InputTouches> touches;
    return touches;
}

// Web thread only: the page's answers for the gesture it is receiving.
struct PageTouches {
    unsigned generation { 0 };
    bool startAnswered { false };
    bool moveAnswered { false };
    bool suppressClick { false };
};

PageTouches& pageTouches()
{
    static NeverDestroyed<PageTouches> touches;
    return touches;
}

// Web thread: one touch event to the page, and its answer to the scrolling thread.
void dispatchTouch(lw_view* view, unsigned generation, PlatformEvent::Type type, Vector<TouchPointState>&& points)
{
    WTF::ActivityRecorder::Scope activityScope(WTF::Activity::Port);
    auto& page = pageTouches();
    if (page.generation != generation)
        page = { generation };
    bool handled = false;
    RefPtr frame = mainFrame(view);
    RefPtr document = frame ? frame->document() : nullptr;
    if (document && document->hasTouchEventHandlers()) {
        Vector<PlatformTouchPoint> platformPoints;
        platformPoints.reserveInitialCapacity(points.size());
        for (auto& point : points)
            platformPoints.append(TouchPoint(point.id, point.state, point.position));
        auto result = frame->eventHandler().handleTouchEvent(TouchEvent(type, WTF::move(platformPoints), MonotonicTime::now()));
        handled = result && *result;
    }
    using TouchAnswer = WebKit::ScrollingTreeDispatcher::TouchAnswer;
    auto& dispatcher = WebKit::ScrollingTreeDispatcher::singleton();
    switch (type) {
    case PlatformEvent::Type::TouchStart:
        page.suppressClick |= handled;
        // The first finger's answer gates the pan; a later finger the page cancels still takes the gesture (a pinch
        // the page handles itself).
        if (!page.startAnswered || handled) {
            page.startAnswered = true;
            dispatcher.pageAnsweredTouch(generation, TouchAnswer::Start, handled);
        }
        break;
    case PlatformEvent::Type::TouchMove:
        if (!page.moveAnswered) {
            page.moveAnswered = true;
            dispatcher.pageAnsweredTouch(generation, TouchAnswer::Move, handled);
        }
        break;
    case PlatformEvent::Type::TouchEnd:
        page.suppressClick |= handled;
        break;
    default:
        break;
    }
    if (handled && Lumia::traceEnabled())
        Lumia::log("touch: la pagina ha annullato %s", type == PlatformEvent::Type::TouchStart ? "touchstart" : type == PlatformEvent::Type::TouchMove ? "touchmove" : "touchend");
}

// Input thread: the fingers as one event shows them, `changed` in `state` and the others stationary.
Vector<TouchPointState> touchSnapshot(const InputTouches& touches, unsigned changed, PlatformTouchPoint::State state)
{
    return WTF::map(touches.points, [&](auto& point) {
        return TouchPointState { point.id, point.position, point.id == changed ? state : PlatformTouchPoint::TouchStationary };
    });
}

void postTouch(lw_view* view, unsigned generation, PlatformEvent::Type type, Vector<TouchPointState>&& points)
{
    postToWebThread([view, generation, type, points = WTF::move(points)] mutable {
        dispatchTouch(view, generation, type, WTF::move(points));
    }, true);
}

// Web thread, before the click of a tap: the page cancelled touchstart or touchend.
bool touchSuppressesClick()
{
    return pageTouches().suppressClick;
}

// ---- where a tap lands: the finger, not the point ----
// A finger touches ~8 mm of glass and the screen reports one point anywhere inside that, so a tap is meant for what
// answers clicks under the finger, not under the point. iOS does this in LocalFrame::nodeRespondingToClickEvents
// (Cocoa-only in WebCore), Chrome with its touch adjustment; without it a 40 px field missed by a few pixels at its edge
// was not touched at all (the poste.it password, tapped ten times on 2026-10-28). Same rules as iOS: the element under
// the point wins if it answers clicks; otherwise, of the elements that answer clicks within the radius, the one whose
// box the finger covers most. The click then goes to the nearest point inside that box.

// Root view coordinates (CSS pixels of the view): the finger's size on the glass does not change with the zoom. iOS uses
// 15 points (2.3 mm on an iPhone); a CSS pixel here is 0.146 mm, so 16 is the same reach.
constexpr int tapSearchRadius = 16;

String describeForLog(const Node* node)
{
    auto* element = dynamicDowncast<Element>(node);
    if (!element)
        return node ? node->nodeName() : "niente"_s;
    StringBuilder builder;
    builder.append(element->localName());
    if (auto* input = dynamicDowncast<HTMLInputElement>(*element))
        builder.append('[', input->type(), ']');
    if (element->hasID())
        builder.append('#', element->getIdAttribute());
    if (element->hasClass() && element->classNames().size())
        builder.append('.', element->classNames()[0]);
    if (auto label = element->getAttribute(HTMLNames::aria_labelAttr); !label.isEmpty())
        builder.append(" \""_s, StringView(label).left(40), '"');
    return builder.toString();
}

// The element of the hit that answers a tap: a link, a button, a field, a label, anything with a click listener or that
// takes the focus on a click. Null when nothing between the hit node and the document does.
RefPtr<Element> tapTargetOf(const HitTestResult& result)
{
    for (RefPtr node = result.innerNode(); node && !node->isDocumentNode(); node = node->parentInComposedTree()) {
        RefPtr element = dynamicDowncast<Element>(*node);
        if (!element)
            continue;
        if (element->willRespondToMouseClickEvents() || element->isMouseFocusable())
            return element;
    }
    return nullptr;
}

struct TapTarget {
    FloatPoint point; // root view coordinates, where the click goes
    RefPtr<Element> element;
    RefPtr<Node> hit; // what the finger's own point hit
};

TapTarget tapTarget(LocalFrame& frame, LocalFrameView& frameView, FloatPoint point)
{
    constexpr OptionSet<HitTestRequest::Type> hitType { HitTestRequest::Type::ReadOnly, HitTestRequest::Type::Active,
        HitTestRequest::Type::DisallowUserAgentShadowContent, HitTestRequest::Type::AllowChildFrameContent };
    auto hitTest = [&](FloatPoint rootViewPoint) {
        return frame.eventHandler().hitTestResultAtPoint(frameView.rootViewToContents(roundedIntPoint(rootViewPoint)), hitType);
    };
    auto center = hitTest(point);
    TapTarget target { point, tapTargetOf(center), center.innerNode() };
    if (target.element)
        return target;

    // The points iOS tries, nearest first, as fractions of the radius.
    static constexpr std::array<std::array<float, 2>, 12> offsets { {
        { -.3f, -.3f }, { +.3f, -.3f }, { -.3f, +.3f }, { +.3f, +.3f },
        { -.6f, -.6f }, { +.6f, -.6f }, { -.6f, +.6f }, { +.6f, +.6f },
        { -1, 0 }, { +1, 0 }, { 0, +1 }, { 0, -1 },
    } };
    FloatRect finger(point.x() - tapSearchRadius, point.y() - tapSearchRadius, 2 * tapSearchRadius, 2 * tapSearchRadius);
    float bestArea = 0;
    FloatRect bestBox;
    for (auto& offset : offsets) {
        auto candidate = tapTargetOf(hitTest({ point.x() + offset[0] * tapSearchRadius, point.y() + offset[1] * tapSearchRadius }));
        if (!candidate || candidate == target.element)
            continue;
        FloatRect box = candidate->boundingBoxInRootViewCoordinates();
        auto covered = intersection(box, finger);
        float area = covered.width() * covered.height();
        if (area <= bestArea)
            continue;
        bestArea = area;
        bestBox = box;
        target.element = WTF::move(candidate);
    }
    if (!target.element)
        return target;
    // The nearest point inside the box, a pixel in from its edge.
    bestBox.inflate(-1);
    target.point = { std::clamp(point.x(), bestBox.x(), std::max(bestBox.x(), bestBox.maxX())),
        std::clamp(point.y(), bestBox.y(), std::max(bestBox.y(), bestBox.maxY())) };
    return target;
}

} // namespace

namespace Lumia {

static std::atomic<double> lastUserInputSeconds { 0 };

void noteUserInput()
{
    lastUserInputSeconds.store(MonotonicTime::now().secondsSinceEpoch().value(), std::memory_order_relaxed);
}

MonotonicTime lastUserInput()
{
    return MonotonicTime::fromRawSeconds(lastUserInputSeconds.load(std::memory_order_relaxed));
}

static std::atomic<bool> loadPacing { true };

bool loadPacingEnabled()
{
    return loadPacing.load(std::memory_order_relaxed);
}

void setLoadPacingEnabled(bool enabled)
{
    loadPacing.store(enabled, std::memory_order_relaxed);
}

} // namespace Lumia

void lw_input_touch(lw_view* view, int pointerId, int phase, float x, float y, uint64_t)
{
    Lumia::noteUserInput();
    if (!view)
        return;
    auto& touches = inputTouches();
    unsigned id = static_cast<unsigned>(pointerId);
    FloatPoint position { x, y };
    auto index = touches.points.findIf([&](auto& point) { return point.id == id; });
    auto& dispatcher = WebKit::ScrollingTreeDispatcher::singleton();
    switch (phase) {
    case LW_PHASE_BEGIN:
        if (touches.points.isEmpty()) {
            // A new gesture: what the page and the scrolling thread said about the last one no longer counts.
            touches.generation++;
            touches.pinchSuppressed = false;
            WebKit::ThreadedCompositor::tapHighlightPress(position);
            dispatcher.beginTouchGesture(*view, touches.generation, position);
        } else
            WebKit::ThreadedCompositor::tapHighlightClear(); // a second finger: no tap
        if (index != notFound)
            touches.points[index].position = position;
        else
            touches.points.append({ id, position, PlatformTouchPoint::TouchPressed });
        touches.openMove = nullptr;
        postTouch(view, touches.generation, PlatformEvent::Type::TouchStart, touchSnapshot(touches, id, PlatformTouchPoint::TouchPressed));
        return;
    case LW_PHASE_UPDATE: {
        if (index == notFound || touches.points[index].position == position)
            return;
        touches.points[index].position = position;
        // Nobody listens under the finger (as the scrolling thread read the regions): the moves would wake the Web
        // thread for nothing.
        auto policy = dispatcher.touchPolicy(touches.generation);
        if (policy.known && !policy.listeners)
            return;
        if (RefPtr batch = touches.openMove) {
            Locker locker { batch->lock };
            if (!batch->taken) {
                for (auto& point : batch->points) {
                    if (point.id == id) {
                        point.position = position;
                        point.state = PlatformTouchPoint::TouchMoved;
                    }
                }
                return;
            }
        }
        Ref batch = adoptRef(*new TouchMoveBatch);
        {
            Locker locker { batch->lock };
            batch->points = touchSnapshot(touches, id, PlatformTouchPoint::TouchMoved);
        }
        touches.openMove = batch.ptr();
        postToWebThread([view, generation = touches.generation, batch = WTF::move(batch)] {
            Vector<TouchPointState> points;
            {
                Locker locker { batch->lock };
                batch->taken = true;
                points = WTF::move(batch->points);
            }
            dispatchTouch(view, generation, PlatformEvent::Type::TouchMove, WTF::move(points));
        }, true);
        return;
    }
    case LW_PHASE_END:
    case LW_PHASE_CANCEL: {
        if (index == notFound)
            return;
        if (phase == LW_PHASE_CANCEL && touches.points.size() == 1)
            WebKit::ThreadedCompositor::tapHighlightClear();
        touches.points[index].position = position;
        touches.openMove = nullptr;
        bool ended = phase == LW_PHASE_END;
        auto points = touchSnapshot(touches, id, ended ? PlatformTouchPoint::TouchReleased : PlatformTouchPoint::TouchCancelled);
        touches.points.removeAt(index);
        postTouch(view, touches.generation, ended ? PlatformEvent::Type::TouchEnd : PlatformEvent::Type::TouchCancel, WTF::move(points));
        return;
    }
    default:
        return;
    }
}

void lw_input_pan(lw_view* view, int phase, float x, float y, float dx, float dy, uint64_t timestampMicroseconds)
{
    Lumia::noteUserInput();
    if (!view)
        return;
    // PointerPoint.Timestamp and MonotonicTime both count QueryPerformanceCounter time; a bogus value falls back
    // to now, so the fling velocity never comes from mixed clocks.
    auto now = MonotonicTime::now();
    auto time = MonotonicTime::fromRawSeconds(timestampMicroseconds / 1e6);
    if (!timestampMicroseconds || std::abs((time - now).seconds()) > 1)
        time = now;
    if (phase == LW_PHASE_BEGIN)
        WebKit::ThreadedCompositor::tapHighlightClear();
    // Whether the page lets it scroll is decided on the scrolling thread (ScrollingTreeDispatcher::gatePan).
    WebKit::ScrollingTreeDispatcher::singleton().handlePan(*view, phase, { x, y }, { dx, dy }, time, inputTouches().generation);
}
// The end of a pinch or of a double-tap zoom, as the screen shows it: the page takes the new page scale with the scroll
// position that keeps the same content under the same pixels, and the compositor drops its own scale on the frame
// that has it (LayerTreeHost::commitPinch).
static void applyPinch(lw_view* view, const WebKit::ThreadedCompositor::PinchResult& pinch)
{
    postToWebThread([view, pinch] {
        WTF::ActivityRecorder::Scope activityScope(WTF::Activity::Port);
        auto* page = view->page ? view->page->corePage() : nullptr;
        RefPtr frame = mainFrame(view);
        RefPtr frameView = frame ? frame->view() : nullptr;
        auto* layerTreeHost = view->page ? view->page->layerTreeHost() : nullptr;
        if (!page || !frameView || !layerTreeHost) {
            WebKit::ThreadedCompositor::pinchCommitted(pinch.generation);
            return;
        }
        float oldScale = page->pageScaleFactor();
        float newScale = oldScale * pinch.scale;
        // The screen showed view point u at c + s (u - c0), so the new top-left is at c0 - c / s in the old view, in
        // content scaled by the old page scale; scaled by s it is the new scroll position.
        FloatPoint scroll = frameView->scrollPosition();
        FloatPoint origin = scroll + toFloatSize(pinch.startCenter) - toFloatSize(pinch.center).scaled(1 / pinch.scale);
        origin.scale(pinch.scale);
        page->setPageScaleFactor(newScale, roundedIntPoint(origin));
        view->page->didUserScale();
        // The new position is applied by the scrolling thread: the frame view still reports the old one here.
        Lumia::log("zoom: scala %.2f -> %.2f, scorrimento %.0f,%.0f -> richiesto %.0f,%.0f", oldScale, page->pageScaleFactor(),
            scroll.x(), scroll.y(), origin.x(), origin.y());
        layerTreeHost->commitPinch(pinch.generation);
    }, true);
}

void lw_input_zoom(lw_view* view, int phase, float scale, float centerX, float centerY)
{
    Lumia::noteUserInput();
    // Two fingers: the compositor scales the screen as they move (no Web thread); when they lift, the page takes the
    // new page scale with the scroll position that keeps the same content under the same pixels. Not where the page
    // keeps the gesture: touch-action without pinch-zoom, or a cancelled touchstart (a map that zooms itself).
    auto& touches = inputTouches();
    auto& dispatcher = WebKit::ScrollingTreeDispatcher::singleton();
    if (phase == LW_PHASE_BEGIN) {
        touches.pinchSuppressed = !dispatcher.touchPolicy(touches.generation).pinchZoom;
        if (touches.pinchSuppressed) {
            Lumia::log("zoom: il gesto e' della pagina");
            return;
        }
        WebKit::ThreadedCompositor::pinchBegin({ centerX, centerY });
        return;
    }
    if (touches.pinchSuppressed)
        return;
    if (phase == LW_PHASE_UPDATE) {
        if (dispatcher.touchPolicy(touches.generation).pageOwns) {
            // The page cancelled the second finger's touchstart after the pinch began.
            touches.pinchSuppressed = true;
            WebKit::ThreadedCompositor::pinchCancel();
            Lumia::log("zoom: annullato, il gesto e' della pagina");
            return;
        }
        WebKit::ThreadedCompositor::pinchUpdate(scale, { centerX, centerY });
        return;
    }
    if (phase != LW_PHASE_END) {
        WebKit::ThreadedCompositor::pinchCancel();
        return;
    }
    auto pinch = WebKit::ThreadedCompositor::pinchEnd();
    if (pinch.valid)
        applyPinch(view, pinch);
}
void lw_input_tap(lw_view* view, float x, float y, uint64_t timestampMicroseconds)
{
    Lumia::noteUserInput();
    // One tap = one press + release, on the Web thread: hit testing, focus, click and navigation are WebCore's own.
    // The app calls this when the finger lifts without having panned.
    // Measured: how long the tap waits in the Web thread's queue. With a FIFO loop this is the rest of whatever
    // the page is doing; on the input lane it should be ~0 between two page tasks.
    // Double tap without delaying single taps: every tap clicks at once, and a second tap within 300 ms and 30 px of a
    // first one that hit no link, button or field (so its click did nothing visible) zooms instead of clicking:
    // from the page's initial scale (its fit) to twice that, and at least to 1:1 CSS pixels (a desktop page's text,
    // fitted at ~0.5, reads there); zoomed in, back to the initial scale. Input thread only.
    // Pages with touch-action: manipulation (or none) under the finger keep their double taps.
    static MonotonicTime lastTapTime;
    static FloatPoint lastTapPoint;
    auto tapTime = MonotonicTime::now();
    bool doubleTap = tapTime - lastTapTime < 300_ms && std::hypot(x - lastTapPoint.x(), y - lastTapPoint.y()) < 30
        && !WebKit::ThreadedCompositor::tapHighlightHadTarget()
        && WebKit::ScrollingTreeDispatcher::singleton().touchPolicy(inputTouches().generation).doubleTapZoom;
    lastTapTime = doubleTap ? MonotonicTime() : tapTime;
    lastTapPoint = { x, y };
    if (doubleTap) {
        WebKit::ThreadedCompositor::tapHighlightClear();
        auto zoom = WebKit::ThreadedCompositor::zoomLimits();
        float target = zoom.pageScale > zoom.initialScale * 1.05f ? zoom.initialScale : std::max(2 * zoom.pageScale, 1.0f);
        float scale = target / zoom.pageScale;
        bool started = WebKit::ThreadedCompositor::zoomAnimate({ x, y }, scale, [view](const WebKit::ThreadedCompositor::PinchResult& result) {
            applyPinch(view, result);
        });
        Lumia::log("doppio tocco a %.0f,%.0f: zoom x%.2f%s", x, y, scale, started ? "" : " (ignorato: zoom in corso)");
        return;
    }

    auto queued = MonotonicTime::now();
    // The box on what was tapped shows at once, from the compositor; the click below hides it when done.
    WebKit::ThreadedCompositor::tapHighlightTap({ x, y });
    postToWebThread([view, x, y, timestampMicroseconds, queued] {
        // F2.c: the port's own share of the task (hit test, event dispatch set-up, click). A tap whose port time is
        // large means our code, not the page.
        WTF::ActivityRecorder::Scope activityScope(WTF::Activity::Port);
        Lumia::log("tap: eseguito %.1f ms dopo l'accodamento", (MonotonicTime::now() - queued).milliseconds());
        RefPtr frame = mainFrame(view);
        if (!frame)
            return;
        auto now = MonotonicTime::now();
        auto time = MonotonicTime::fromRawSeconds(timestampMicroseconds / 1e6);
        if (!timestampMicroseconds || std::abs((time - now).seconds()) > 1)
            time = now;
        auto& handler = frame->eventHandler();
        if (touchSuppressesClick()) {
            WebKit::ThreadedCompositor::tapHighlightClickDone();
            return;
        }
        // What the finger meant (tapTarget): the click goes inside it even when the reported point is a little off.
        FloatPoint point { x, y };
        if (RefPtr frameView = frame->view()) {
            auto target = tapTarget(*frame, *frameView, point);
            if (Lumia::traceEnabled()) {
                if (!target.element)
                    Lumia::log("tap: niente di cliccabile entro %d px (sotto il dito %s)", tapSearchRadius, describeForLog(target.hit.get()).utf8().data());
                else if (target.point != point)
                    Lumia::log("tap: bersaglio %s, spostato di %.0f,%.0f (sotto il dito %s)", describeForLog(target.element.get()).utf8().data(),
                        target.point.x() - x, target.point.y() - y, describeForLog(target.hit.get()).utf8().data());
                else
                    Lumia::log("tap: bersaglio %s", describeForLog(target.element.get()).utf8().data());
            }
            point = target.point;
        }
        auto mouseEvent = [&](PlatformEvent::Type type) {
            return PlatformMouseEvent(DoublePoint(point.x(), point.y()), DoublePoint(point.x(), point.y()), MouseButton::Left, type, 1, { }, time, 1.0,
                SyntheticClickType::NoTap, MouseEventInputSource::UserDriven);
        };
        // The compatibility mouse events of a tap, as mobile browsers send them: the mouse first moves there
        // (mouseover, :hover, menus that open on hover), then press, release, click.
        handler.handleMouseMoveEvent(PlatformMouseEvent(DoublePoint(point.x(), point.y()), DoublePoint(point.x(), point.y()), MouseButton::None,
            PlatformEvent::Type::MouseMoved, 0, { }, time, 0, SyntheticClickType::NoTap, MouseEventInputSource::UserDriven));
        handler.handleMousePressEvent(mouseEvent(PlatformEvent::Type::MousePressed));
        handler.handleMouseReleaseEvent(mouseEvent(PlatformEvent::Type::MouseReleased));
        WebKit::ThreadedCompositor::tapHighlightClickDone();
        if (Lumia::traceEnabled()) {
            RefPtr document = frame->document();
            Lumia::log("tap: fuoco su %s", describeForLog(document ? document->focusedElement() : nullptr).utf8().data());
        }
        // End-to-end for the "tap -> click" requirement: from the finger's own timestamp to the end of the release
        // dispatch, page handlers included. The line above says how much of it was queueing; the difference is
        // hit testing, the click, and the page's own click handler.
        if (Lumia::traceEnabled())
            Lumia::log("tap: click dopo %.1f ms dal dito", (MonotonicTime::now() - time).milliseconds());
    }, true);
}
void lw_input_double_tap(lw_view*, float, float) { }
void lw_input_long_press(lw_view* view, float x, float y)
{
    Lumia::noteUserInput();
    // What is under the finger, for the app's menu: the link and its text, the image, or the paragraph of text
    // (the app shows it in a panel with the system's own text selection, since WP8.1 apps cannot write the clipboard).
    WebKit::ThreadedCompositor::tapHighlightPress({ x, y });
    postToWebThread([view, x, y] {
        WTF::ActivityRecorder::Scope activityScope(WTF::Activity::Port);
        RefPtr frame = mainFrame(view);
        RefPtr frameView = frame ? frame->view() : nullptr;
        if (!frameView) {
            WebKit::ThreadedCompositor::tapHighlightClear();
            return;
        }
        constexpr OptionSet<HitTestRequest::Type> hitType { HitTestRequest::Type::ReadOnly, HitTestRequest::Type::Active,
            HitTestRequest::Type::DisallowUserAgentShadowContent, HitTestRequest::Type::AllowChildFrameContent };
        auto result = frame->eventHandler().hitTestResultAtPoint(frameView->rootViewToContents(IntPoint(x, y)), hitType);
        String link = result.absoluteLinkURL().string();
        String image = result.absoluteImageURL().string();
        String text;
        IntRect rect;
        if (RefPtr element = result.URLElement()) {
            text = element->innerText();
            rect = element->boundingBoxInRootViewCoordinates();
        } else if (RefPtr node = result.innerNonSharedNode()) {
            // The nearest block with text: a paragraph, a list item, a cell.
            for (RefPtr ancestor = node->isElementNode() ? downcast<Element>(node.get()) : node->parentElement(); ancestor; ancestor = ancestor->parentElement()) {
                auto* renderer = ancestor->renderer();
                if (!renderer || !renderer->isRenderBlock())
                    continue;
                String candidate = ancestor->innerText();
                if (candidate.trim(isASCIIWhitespace).isEmpty())
                    continue;
                text = candidate;
                rect = ancestor->boundingBoxInRootViewCoordinates();
                break;
            }
        }
        constexpr unsigned maximumLength = 20000;
        if (text.length() > maximumLength)
            text = text.left(maximumLength);
        Lumia::log("pressione lunga a %.0f,%.0f: link %u, immagine %u, testo %u caratteri", x, y, link.length(), image.length(), text.length());
        auto characters = link.charactersWithNullTermination();
        auto characters2 = image.charactersWithNullTermination();
        auto characters3 = text.charactersWithNullTermination();
        lw_field field { };
        field.kind = LW_FIELD_NONE;
        field.text = characters3 ? reinterpret_cast<const wchar_t*>(characters3->span().data()) : L"";
        field.x = rect.x();
        field.y = rect.y();
        field.width = rect.width();
        field.height = rect.height();
        field.max_length = -1;
        lw_event event { };
        event.type = LW_EVENT_CONTEXT_MENU;
        event.text = characters ? reinterpret_cast<const wchar_t*>(characters->span().data()) : L"";
        event.text2 = characters2 ? reinterpret_cast<const wchar_t*>(characters2->span().data()) : L"";
        event.field = &field;
        sendEvent(*view, event);
        WebKit::ThreadedCompositor::tapHighlightClear();
    }, true);
}
void lw_input_key(lw_view*, int, int) { }

namespace {

// keydown, keypress and keyup of one key of the on-screen keyboard. The text goes in through the editor (lw_text_edit)
// between press() and release(); press() says false if the page prevented the key.
class KeystrokeEvents {
public:
    KeystrokeEvents(LocalFrame& frame, const String& text)
        : m_frame(frame)
        , m_text(text)
    {
        UChar character = text.isEmpty() ? 0 : text[0];
        m_key = text.isEmpty() ? "Backspace"_s : text;
        m_code = text.isEmpty() ? "Backspace"_s : isASCIIDigit(character) ? makeString("Digit"_s, text)
            : isASCIIAlpha(character) ? makeString("Key"_s, text.convertToASCIIUppercase()) : character == ' ' ? "Space"_s : String { emptyString() };
        m_virtualKey = text.isEmpty() ? VK_BACK : isASCIIAlphanumeric(character) ? toASCIIUpper(character) : character == ' ' ? VK_SPACE : 0;
    }

    bool press()
    {
        auto& handler = m_frame->eventHandler();
        if (handler.keyEvent(event(PlatformEvent::Type::RawKeyDown, { })))
            return false;
        return m_text.isEmpty() || !handler.keyEvent(event(PlatformEvent::Type::Char, m_text));
    }

    void release() { m_frame->eventHandler().keyEvent(event(PlatformEvent::Type::KeyUp, { })); }

private:
    PlatformKeyboardEvent event(PlatformEvent::Type type, const String& text)
    {
        return PlatformKeyboardEvent(type, text, text, m_key, m_code, "Unidentified"_s, m_virtualKey, false, false, false, { }, MonotonicTime::now());
    }

    Ref<LocalFrame> m_frame;
    String m_text;
    String m_key;
    String m_code;
    int m_virtualKey { 0 };
};

} // namespace

void lw_text_edit(lw_view* view, uint32_t sequence, int start, int end, const wchar_t* text, int selectionStart, int selectionEnd)
{
    Lumia::noteUserInput();
    // Replace [start, end) with `text`, as an input method committing a string: it goes through WebCore's editing
    // commands (beforeinput/input, typing style, form value update), never through setValue, which the page would
    // not see. The sequence is stored before the edit so the FIELD_UPDATE events that the edit itself triggers carry
    // the number the app already knows.
    // Measured on the same line as the edit: how long the keystroke waited in the input lane (must stay ~0 even
    // during a JS block, exactly like lw_input_tap) and how long WebCore took to apply it.
    auto queued = MonotonicTime::now();
    postToWebThread([view, sequence, start, end, selectionStart, selectionEnd, queued, text = toString(text).isolatedCopy()] {
        WTF::ActivityRecorder::Scope activityScope(WTF::Activity::Port);
        auto started = MonotonicTime::now();
        RefPtr frame = mainFrame(view);
        if (!frame)
            return;
        RefPtr document = frame->document();
        RefPtr element = document ? document->focusedElement() : nullptr;
        if (!element)
            return;
        view->page->setLastTextSequence(sequence);
        view->page->setTextEditInProgress(true);
        auto& editor = frame->editor();

        if (auto* control = dynamicDowncast<HTMLTextFormControlElement>(*element)) {
            // The bar's range is in UTF-16 units of the value the app was given; clamp against what the page has now
            // (a page script may have changed it since), so a stale bar cannot corrupt the field.
            unsigned length = String(control->value()).length();
            unsigned from = std::min(static_cast<unsigned>(std::max(start, 0)), length);
            unsigned to = std::min(static_cast<unsigned>(std::max(end, 0)), length);
            if (to < from)
                to = from;
            control->setSelectionRange(from, to);
            // One key pressed gives the page its key events, as on any phone: code boxes move to the next box on them,
            // and a page can refuse the key. Longer edits (a suggested word, a paste) are input method commits.
            std::optional<KeystrokeEvents> keystroke;
            if ((text.length() == 1 && from == to) || (text.isEmpty() && to == from + 1))
                keystroke.emplace(*frame, text);
            if (keystroke && !keystroke->press()) {
                keystroke->release();
                view->page->setTextEditInProgress(false);
                if (RefPtr page = frame->page())
                    page->editorClient().respondToChangedContents();
                return;
            }
            if (text.isEmpty())
                editor.deleteSelectionWithSmartDelete(false, EditAction::Delete);
            else
                editor.insertText(text, nullptr);
            // The page may have moved the focus already; the caret below is for the field that was edited.
            if (document->focusedElement() != element) {
                view->page->setTextEditInProgress(false);
                if (keystroke)
                    keystroke->release();
                return;
            }
            if (keystroke)
                keystroke->release();
            if (document->focusedElement() != element) {
                view->page->setTextEditInProgress(false);
                return;
            }
            unsigned afterLength = String(control->value()).length();
            unsigned caret = std::min(static_cast<unsigned>(std::max(selectionStart, 0)), afterLength);
            unsigned caretEnd = std::min(static_cast<unsigned>(std::max(selectionEnd, 0)), afterLength);
            if (caretEnd < caret)
                caretEnd = caret;
            control->setSelectionRange(caret, caretEnd);
        } else if (element->isRootEditableElement()) {
            // contenteditable: the bar's offsets are positions in the root's plain text (WebKit::editableText, the text
            // the bar was given). The range becomes the DOM selection unless it already is (setting it fires
            // selectionchange, which editors listen to), then the text goes in as an input method commit. The caret
            // after it is WebCore's; the update below tells the bar where it is.
            auto current = WebKit::editableText(*element);
            int length = static_cast<int>(current.text.length());
            int from = std::clamp(start, 0, length);
            int to = std::clamp(end, from, length);
            if (from != current.selectionStart || to != current.selectionEnd) {
                auto range = resolveCharacterRange(makeRangeSelectingNodeContents(*element),
                    CharacterRange { static_cast<uint64_t>(from), static_cast<uint64_t>(to - from) });
                frame->selection().setSelection(VisibleSelection { range });
            }
            if (!text.isEmpty())
                editor.insertText(text, nullptr);
            else if (to > from)
                editor.deleteSelectionWithSmartDelete(false, EditAction::Delete);
        } else {
            view->page->setTextEditInProgress(false);
            return;
        }

        // The edit is complete: one FIELD_UPDATE with the text and the caret the page really has (it may have
        // clamped a maxlength or refused the change), so the bar resyncs once per commit instead of per step.
        view->page->setTextEditInProgress(false);
        if (RefPtr page = frame->page())
            page->editorClient().respondToChangedContents();

        Lumia::log("testo: edit seq=%u [%d,%d) -> %d UTF-16, sel %d..%d, attesa %.1f ms, applicato in %.1f ms",
            sequence, start, end, static_cast<int>(text.length()), selectionStart, selectionEnd,
            (started - queued).milliseconds(), (MonotonicTime::now() - started).milliseconds());
    }, true);
}

void lw_text_enter(lw_view* view)
{
    Lumia::noteUserInput();
    // A real Enter through EventHandler, not a form submission: the page's keydown/keypress handlers run first and a
    // prevented key does nothing — the same path a hardware keyboard takes on Windows (text "\r" on the keydown, as
    // TranslateMessage produces for WM_KEYDOWN + VK_RETURN).
    postToWebThread([view] {
        WTF::ActivityRecorder::Scope activityScope(WTF::Activity::Port);
        RefPtr frame = mainFrame(view);
        if (!frame)
            return;
        auto now = MonotonicTime::now();
        auto makeEvent = [&](PlatformEvent::Type type, const String& text) {
            return PlatformKeyboardEvent(type, text, text, "Enter"_s, "Enter"_s, "Enter"_s, VK_RETURN,
                false, false, false, { }, now);
        };
        auto& handler = frame->eventHandler();
        handler.keyEvent(makeEvent(PlatformEvent::Type::KeyDown, "\r"_s));
        handler.keyEvent(makeEvent(PlatformEvent::Type::KeyUp, { }));
        Lumia::log("testo: enter");
    }, true);
}

void lw_text_blur(lw_view* view)
{
    // The bar was closed by the user (keyboard dismissed): drop the focus, which comes back as LW_EVENT_FIELD_FOCUS
    // with kind LW_FIELD_NONE from the editor client.
    postToWebThread([view] {
        RefPtr frame = mainFrame(view);
        if (!frame)
            return;
        RefPtr document = frame->document();
        RefPtr element = document ? document->focusedElement() : nullptr;
        if (!element)
            return;
        element->blur();
        Lumia::log("testo: blur");
    }, true);
}

void lw_dialog_reply(lw_view* view, int id, int accepted, const wchar_t* value)
{
    // Straight from the app's thread: the Web thread is the one waiting.
    if (view)
        answerDialog(*view, id, !!accepted, toString(value).isolatedCopy());
}
namespace {

// lw_file_chooser_reply's pending choosers, by id. Web thread.
HashMap<int, Ref<FileChooser>>& fileChoosers()
{
    static NeverDestroyed<HashMap<int, Ref<FileChooser>>> choosers;
    return choosers;
}

} // namespace

void Lumia::runFileChooser(lw_view& view, FileChooser& chooser)
{
    static int lastID;
    int id = ++lastID;
    auto& settings = chooser.settings();
    StringBuilder accept;
    for (auto& type : settings.acceptMIMETypes)
        accept.append(accept.isEmpty() ? ""_s : ","_s, type);
    for (auto& extension : settings.acceptFileExtensions)
        accept.append(accept.isEmpty() ? ""_s : ","_s, extension);
    fileChoosers().set(id, chooser);
    log("file: scelta %d, tipi \"%s\"%s", id, accept.toString().utf8().data(), settings.allowsMultipleFiles ? ", piu' file" : "");
    postEvent(view, LW_EVENT_FILE_CHOOSER, settings.allowsMultipleFiles ? 1 : 0, accept.toString(), { }, id);
}

void lw_file_chooser_reply(lw_view*, int id, const wchar_t* const* paths, int count)
{
    Vector<String> files;
    for (int i = 0; paths && i < count; ++i) {
        if (paths[i])
            files.append(toString(paths[i]).isolatedCopy());
    }
    postToWebThread([id, files = WTF::move(files)] {
        RefPtr chooser = fileChoosers().take(id);
        if (!chooser)
            return;
        if (files.isEmpty())
            chooser->cancelFileChoosing();
        else
            chooser->chooseFiles(files);
    }, true);
}

lw_view* lw_window_reply(lw_view* opener, int id, void* swapChainPanel, int width, int height, float deviceScale)
{
    if (!opener)
        return nullptr;
    Locker locker { opener->window.lock };
    if (!opener->window.id || opener->window.answered || opener->window.id != id)
        return nullptr;
    opener->window.answered = true;
    opener->window.view = swapChainPanel ? new lw_view : nullptr;
    opener->window.panel = swapChainPanel;
    opener->window.width = width;
    opener->window.height = height;
    opener->window.scale = deviceScale;
    opener->window.condition.notifyAll();
    return opener->window.view;
}

void lw_download_reply(lw_view*, int id, const wchar_t* path)
{
    if (path)
        return;
    postToWebThread([id] {
        Lumia::cancelDownload(id);
    }, true);
}
