#include "config.h"
#include "BytecodeCache.h"

#include "Engine.h"
#include "ScriptBytecodeCacheWP81.h"
#include "WebCoreInstanceHandle.h"
#include <JavaScriptCore/VMEntryScope.h>
#include <WebCore/CommonVM.h>
#include <WebCore/DOMWindow.h>
#include <WebCore/Document.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/JSDOMWindowBase.h>
#include <WebCore/Page.h>
#include <JavaScriptCore/BytecodeCacheError.h>
#include <JavaScriptCore/CachedBytecode.h>
#include <JavaScriptCore/CachedTypes.h>
#include <JavaScriptCore/JSCellInlines.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/Strong.h>
#include <JavaScriptCore/StrongInlines.h>
#include <JavaScriptCore/UnlinkedFunctionCodeBlock.h>
#include <JavaScriptCore/UnlinkedFunctionExecutable.h>
#include <array>
#include <cstring>
#include <wtf/CrossThreadCopier.h>
#include <wtf/CryptographicallyRandomNumber.h>
#include <wtf/Deque.h>
#include <wtf/FileHandle.h>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/MainThread.h>
#include <wtf/MonotonicTime.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/StdLibExtras.h>
#include <wtf/WallTime.h>
#include <wtf/text/StringToIntegerConversion.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/MakeString.h>
#include <windows.h>

// The flash, the policy and the bookkeeping of the bytecode cache. A script's file holds exactly the image JSC's
// CachedBytecode describes: the encoded program first, then each function code block appended as it was compiled, with
// the records of those functions patched to point at it (CachedBytecode::commitUpdates). Files are named
// <key>-<generation>.jsc and never changed once written: a page may have one mapped, and JSC decodes from it for as long
// as the page runs. A commit writes the next generation instead, as a copy of the file the script's image is based on
// plus the new functions, to a temporary name, flushed, then renamed; the script goes on collecting on top of the new
// file (CachedBytecode::takeUpdates). All the copying and writing is on a background queue, and nothing of an image is
// kept in memory but the functions not written yet.
//
// A function is not encoded when JSC compiles it, in the middle of the page's own work: it waits, its code kept alive,
// and the encoder takes the waiting functions at rest, in the order they were compiled (a function's update needs the
// update of the one it sits in first: that one makes it a leaf of the image), a few milliseconds at a time, on the Web
// thread's low lane (encodeSlice). On Gemini's first visit encoding was 15% of the Web thread during the load (profile of
// 11/11, PLAN.md \u00a777). Each function is encoded on its own (JSC's encodeFunctionCodeBlock, shallow in WP81), as at
// compile time: nothing it contains goes in with it.

namespace WebCore {

// What the cache knows of one script, for as long as its provider lives (ScriptBytecodeCacheWP81.h).
struct ScriptBytecodeCacheState::Script {
    // A function compiled and not encoded yet, its executable and code kept alive until then. Moved by swapping the
    // handles (a Strong copy allocates a new one).
    struct Deferred {
        Deferred(JSC::VM& vm, JSC::UnlinkedFunctionExecutable* executable, JSC::UnlinkedFunctionCodeBlock* codeBlock, JSC::CodeSpecializationKind kind)
            : executable(vm, executable)
            , codeBlock(vm, codeBlock)
            , kind(kind)
        {
        }
        Deferred(Deferred&& other)
            : kind(other.kind)
        {
            executable.swap(other.executable);
            codeBlock.swap(other.codeBlock);
        }
        Deferred& operator=(Deferred&& other)
        {
            executable.swap(other.executable);
            codeBlock.swap(other.codeBlock);
            kind = other.kind;
            return *this;
        }

        JSC::Strong<JSC::UnlinkedFunctionExecutable> executable;
        JSC::Strong<JSC::UnlinkedFunctionCodeBlock> codeBlock;
        JSC::CodeSpecializationKind kind;
    };

    uint64_t key { 0 };
    Origin origin { Origin::External };
    // The file JSC decodes from, when the script came from the disk. JSC adds to its leaf map each function it decodes
    // whose code is not in the file, lazily, while the page runs.
    RefPtr<JSC::CachedBytecode> decoded;
    // The functions compiled since the last commit, on top of the file that commit wrote (or `decoded`'s file, or
    // nothing for a script compiled from its text). Null until something is compiled.
    RefPtr<JSC::CachedBytecode> collector;
    // The generation of the file under the collector (0: none). A commit moves it to the file it writes.
    unsigned baseGeneration { 0 };
    // Encoded bytes in the collector, not written yet.
    size_t pendingBytes { 0 };
    bool pending { false };
    bool readDisk { false };
    // A write failed: the collector's base is a file that does not exist, nothing more of this script this visit.
    bool failed { false };
    // The functions waiting for the encoder, in the order they were compiled; the first `nextDeferred` are done.
    // Handles: created and released with the JS lock held, on the Web thread (the store's encode queue keeps the
    // provider, hence this, alive while any wait).
    Vector<Deferred> deferred;
    size_t nextDeferred { 0 };
    bool queued { false };
};

} // namespace WebCore

namespace Lumia::BytecodeCache {

namespace {

using Script = WebCore::ScriptBytecodeCacheState::Script;
using Origin = WebCore::ScriptBytecodeCacheState::Origin;

// Smallest scripts worth a file (an external script's size is known only when it runs). Inline scripts are filtered
// by ScriptSourceCode before a caching provider even exists.
constexpr unsigned minimumExternalLength = 1024;
constexpr unsigned minimumInlineLength = 4096;
// Larger ones would take seconds to encode on this CPU and tens of megabytes on the flash.
constexpr unsigned maximumLength = 8 * 1024 * 1024;
// The flash the cache may take (one site like Gemini writes 85 MB on its first visit), and where a cleanup brings it
// back to, least recently used first.
constexpr uint64_t capacity = 160 * 1024 * 1024;
constexpr uint64_t capacityAfterCleanup = 128 * 1024 * 1024;
// A script that already has a file is written again only for enough new code, as each write copies the whole file: at
// least this much and a quarter of the file while the page runs, less when the app may not come back.
constexpr size_t minimumUpdateBytes = 64 * 1024;
constexpr size_t minimumUpdateBytesLeaving = 16 * 1024;
// Functions compiled since the last commit are held in memory, encoded: past this, everything is written soon.
constexpr size_t maximumPendingBytes = 8 * 1024 * 1024;
// Functions compiled long after the load (a page that stays open) wait this long for more to join them.
constexpr Seconds pendingCommitDelay = 30_s;
// The encoder's slice on the low lane: then input, normal tasks and due timers go first (RunLoop::performWork).
constexpr Seconds encodeSliceDuration = 6_ms;
// What the app may still spend encoding when it may not come back: suspension (the system gives a few seconds, the
// writes need theirs) and memory pressure (the waiting code is memory too). The functions beyond are dropped: the
// next visit compiles them again and they are cached then.
constexpr Seconds suspensionEncodeBudget = 400_ms;
constexpr Seconds memoryEncodeBudget = 100_ms;
// Inline scripts seen once (stored on the second sighting), remembered across runs.
constexpr size_t maximumSeen = 4096;

struct FileEntry {
    unsigned generation { 0 };
    uint64_t size { 0 };
    WallTime lastUsed;
};

struct PendingScript {
    Ref<JSC::SourceProvider> provider; // keeps the script below alive
    Script* script;
};

// One file to write. Plain data: the writer queue only reads `updates`, which the batch keeps alive on the main thread.
struct Write {
    uint64_t key { 0 };
    unsigned generation { 0 };
    String basePath; // empty when the image starts from nothing
    uint64_t baseSize { 0 };
    String path;
    uint64_t size { 0 };
    const JSC::CachedBytecode* updates { nullptr };
    bool written { false };
};

// What a commit's writes need alive until they are done, released on the main thread.
struct Batch {
    Vector<PendingScript> scripts; // in the order of the writes
    Vector<Ref<JSC::CachedBytecode>> updates;
};

struct Store {
    bool enabled { false };
    // "bytecode" in the address box: scripts get no cache (the comparison of a page with and without it).
    bool bypassed { false };
    String directory;
    // The key of contentKey.
    std::array<uint64_t, 2> secret { };
    HashMap<uint64_t, FileEntry> files;
    uint64_t totalSize { 0 };
    // Generations are unique across keys: two pages running the same script never pick the same file name.
    unsigned nextGeneration { 1 };
    HashSet<uint64_t> seen;
    Vector<uint64_t> seenOrder;
    bool seenChanged { false };
    Vector<PendingScript> pending;
    size_t pendingBytes { 0 };
    // Scripts with functions waiting for the encoder, in the order their first one came; a script is in it once.
    Deque<PendingScript> encodeQueue;
    size_t deferredFunctions { 0 };
    bool encodeScheduled { false };
    bool commitEverything { false };
    RefPtr<WorkQueue> writer;
    std::unique_ptr<RunLoop::Timer> commitTimer;
    // This run, for "memoria".
    unsigned hits { 0 };
    uint64_t hitBytes { 0 };
    unsigned rejected { 0 };
    unsigned misses { 0 };
    unsigned stored { 0 };
    uint64_t storedBytes { 0 };
    unsigned writeFailures { 0 };
    unsigned encoded { 0 };
    unsigned droppedFunctions { 0 };
    Seconds encodeTime;
};

Store& store()
{
    ASSERT(isMainThread());
    static NeverDestroyed<Store> store;
    return store;
}

// SipHash-2-4 (Aumasson and Bernstein), 64 bits out.
uint64_t sipHash24(uint64_t k0, uint64_t k1, std::span<const uint8_t> data)
{
    auto rotate = [](uint64_t value, unsigned bits) { return (value << bits) | (value >> (64 - bits)); };
    uint64_t v0 = 0x736f6d6570736575ull ^ k0;
    uint64_t v1 = 0x646f72616e646f6dull ^ k1;
    uint64_t v2 = 0x6c7967656e657261ull ^ k0;
    uint64_t v3 = 0x7465646279746573ull ^ k1;
    auto round = [&] {
        v0 += v1; v1 = rotate(v1, 13); v1 ^= v0; v0 = rotate(v0, 32);
        v2 += v3; v3 = rotate(v3, 16); v3 ^= v2;
        v0 += v3; v3 = rotate(v3, 21); v3 ^= v0;
        v2 += v1; v1 = rotate(v1, 17); v1 ^= v2; v2 = rotate(v2, 32);
    };
    auto block = [&](uint64_t m) {
        v3 ^= m;
        round();
        round();
        v0 ^= m;
    };
    size_t blocks = data.size() / 8;
    for (size_t i = 0; i < blocks; ++i) {
        uint64_t m;
        memcpy(&m, data.data() + 8 * i, 8);
        block(m);
    }
    uint64_t last = static_cast<uint64_t>(data.size() & 0xff) << 56;
    for (size_t i = 0; i < (data.size() & 7); ++i)
        last |= static_cast<uint64_t>(data[8 * blocks + i]) << (8 * i);
    block(last);
    v2 ^= 0xff;
    round();
    round();
    round();
    round();
    return v0 ^ v1 ^ v2 ^ v3;
}

// Content key: a keyed hash of the text, the secret key being this phone's (key.bin, made at random once). A file is
// decoded for a script whose key, length and JSC's own hash of the text all match the one it was made from (JSC does
// not keep the text in the file, CachedTypes.cpp): an accidental match is out of reach, and without the secret no page
// can make up a script whose key is another site's, to have its own code run there in place of that site's.
// ~5 ms per megabyte here, against the seconds the compilation it saves takes.
uint64_t contentKey(StringView source, JSC::SourceProviderSourceType type)
{
    auto& s = store();
    auto bytes = source.is8Bit() ? asByteSpan(source.span8()) : asByteSpan(source.span16());
    return sipHash24(s.secret[0], s.secret[1] ^ (static_cast<uint64_t>(type) << 1 | (source.is8Bit() ? 1 : 0)), bytes);
}

String fileName(uint64_t key, unsigned generation)
{
    return makeString(hex(key, 16, Lowercase), '-', generation, ".jsc"_s);
}

String filePath(uint64_t key, unsigned generation)
{
    return FileSystem::pathByAppendingComponent(store().directory, fileName(key, generation));
}

// "0123456789abcdef-7.jsc" -> key, generation.
std::optional<std::pair<uint64_t, unsigned>> parseFileName(StringView name)
{
    if (!name.endsWith(".jsc"_s) || name.length() < 16 + 1 + 1 + 4 || name[16] != '-')
        return std::nullopt;
    auto key = parseInteger<uint64_t>(name.left(16), 16);
    auto generation = parseInteger<unsigned>(name.substring(17, name.length() - 17 - 4));
    if (!key || !generation)
        return std::nullopt;
    return { { *key, *generation } };
}

// webkit.dll's link time: a new engine may encode differently (JSC's own check is the compile time of one file).
String buildIdentity()
{
    auto* base = reinterpret_cast<const uint8_t*>(WebCore::instanceHandle());
    if (!base)
        return "0"_s;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return makeString(hex(nt->FileHeader.TimeDateStamp, 8, Lowercase), '-', hex(nt->OptionalHeader.SizeOfImage, Lowercase));
}

String seenPath()
{
    return FileSystem::pathByAppendingComponent(store().directory, "seen.bin"_s);
}

String secretPath()
{
    return FileSystem::pathByAppendingComponent(store().directory, "key.bin"_s);
}

// The secret of contentKey: false when there was none, and the files and the seen list made with another are of no use.
bool loadSecret()
{
    auto& s = store();
    if (auto handle = FileSystem::openFile(secretPath(), FileSystem::FileOpenMode::Read)) {
        if (handle.size() == sizeof(s.secret) && handle.read(asMutableByteSpan(std::span { s.secret })) == sizeof(s.secret))
            return true;
    }
    cryptographicallyRandomValues(asMutableByteSpan(std::span { s.secret }));
    auto temporary = makeString(secretPath(), ".tmp"_s);
    bool written = false;
    if (auto handle = FileSystem::openFile(temporary, FileSystem::FileOpenMode::Truncate))
        written = handle.write(asByteSpan(std::span { s.secret })) == sizeof(s.secret) && FlushFileBuffers(handle.platformHandle());
    if (!written || !FileSystem::moveFile(temporary, secretPath()))
        log("bytecode: chiave non scritta");
    return false;
}

void loadSeen()
{
    auto& s = store();
    auto handle = FileSystem::openFile(seenPath(), FileSystem::FileOpenMode::Read);
    if (!handle)
        return;
    auto size = handle.size();
    if (!size || *size % 8 || *size > maximumSeen * 8)
        return;
    Vector<uint64_t> keys(static_cast<size_t>(*size / 8));
    if (handle.read(asMutableByteSpan(keys.mutableSpan())) != *size)
        return;
    for (auto key : keys) {
        if (s.seen.add(key).isNewEntry)
            s.seenOrder.append(key);
    }
}

// Writes the seen list on the writer queue. Main thread.
void saveSeen()
{
    auto& s = store();
    if (!s.seenChanged || !s.writer)
        return;
    s.seenChanged = false;
    if (s.seenOrder.size() > maximumSeen) {
        auto dropped = s.seenOrder.size() - maximumSeen;
        for (size_t i = 0; i < dropped; ++i)
            s.seen.remove(s.seenOrder[i]);
        s.seenOrder.removeAt(0, dropped);
    }
    s.writer->dispatch([path = seenPath().isolatedCopy(), keys = s.seenOrder] {
        auto temporary = makeString(path, ".tmp"_s);
        auto handle = FileSystem::openFile(temporary, FileSystem::FileOpenMode::Truncate);
        if (!handle)
            return;
        bool written = handle.write(asByteSpan(keys.span())) == keys.size() * 8;
        handle = { };
        if (written)
            FileSystem::moveFile(temporary, path);
    });
}

void deleteDirectoryContents(const String& directory)
{
    for (auto& name : FileSystem::listDirectory(directory)) {
        auto path = FileSystem::pathByAppendingComponent(directory, name);
        if (FileSystem::fileType(path) == FileSystem::FileType::Directory) {
            deleteDirectoryContents(path);
            FileSystem::deleteEmptyDirectory(path);
        } else
            FileSystem::deleteFile(path);
    }
}

void deleteFiles(Vector<String>&& paths)
{
    if (paths.isEmpty())
        return;
    // A file a page still has mapped stays: a later start-up removes it.
    store().writer->dispatch([paths = crossThreadCopy(WTF::move(paths))] {
        for (auto& path : paths)
            FileSystem::deleteFile(path);
    });
}

// Beyond the capacity, the files least recently read or written go.
void evictIfNeeded(Vector<String>& obsolete)
{
    auto& s = store();
    if (s.totalSize <= capacity)
        return;
    Vector<std::pair<WallTime, uint64_t>> byUse;
    for (auto& [key, entry] : s.files)
        byUse.append({ entry.lastUsed, key });
    std::sort(byUse.begin(), byUse.end());
    for (auto& [lastUsed, key] : byUse) {
        if (s.totalSize <= capacityAfterCleanup)
            break;
        auto entry = s.files.take(key);
        s.totalSize -= entry.size;
        obsolete.append(filePath(key, entry.generation));
    }
}

// Writer queue: the file of one Write, from its base and its updates. Nothing that a page may have mapped is changed.
bool writeImage(const Write& write)
{
    auto temporary = makeString(write.path, ".tmp"_s);
    bool written = [&] {
        auto file = FileSystem::openFile(temporary, FileSystem::FileOpenMode::Truncate);
        if (!file)
            return false;
        if (write.baseSize) {
            auto base = FileSystem::openFile(write.basePath, FileSystem::FileOpenMode::Read);
            if (!base || base.size() != write.baseSize)
                return false;
            Vector<uint8_t> buffer(static_cast<size_t>(std::min<uint64_t>(write.baseSize, 256 * 1024)));
            for (uint64_t copied = 0; copied < write.baseSize;) {
                auto chunk = buffer.mutableSpan().first(static_cast<size_t>(std::min<uint64_t>(buffer.size(), write.baseSize - copied)));
                if (base.read(chunk) != chunk.size() || file.write(chunk) != chunk.size())
                    return false;
                copied += chunk.size();
            }
        }
        // The functions' code goes after the copy, the patches of their records into it.
        bool ok = true;
        write.updates->commitUpdates([&](off_t offset, std::span<const uint8_t> data) {
            if (ok)
                ok = file.seek(offset, FileSystem::FileSeekOrigin::Beginning) == static_cast<uint64_t>(offset) && file.write(data) == data.size();
        });
        // On the flash before the name: after a power cut a file is either whole or absent.
        return ok && file.size() == write.size && FlushFileBuffers(file.platformHandle());
    }();
    if (written && FileSystem::moveFile(temporary, write.path))
        return true;
    FileSystem::deleteFile(temporary);
    return false;
}

void notePending(const JSC::SourceProvider&, Script&);

// One function's update: its code encoded and added to the script's collector, after its record (in the collector's
// leaf map, or for a function inside one decoded from the file, in the map JSC fills as it decodes: the same offset in
// every later image) is checked to be really this function's. The bytes it adds.
size_t encodeFunction(Script& script, const Script::Deferred& function)
{
    auto* executable = function.executable.get();
    if (!script.collector) {
        if (!script.decoded)
            return 0;
        script.collector = JSC::CachedBytecode::createOnImage(script.decoded->size());
    }
    auto& collector = *script.collector;
    auto it = collector.leafExecutables().find(executable);
    if (it == collector.leafExecutables().end() || !it->value.describes(*executable)) {
        if (!script.decoded)
            return 0;
        auto decoded = script.decoded->leafExecutables().find(executable);
        if (decoded == script.decoded->leafExecutables().end() || !decoded->value.describes(*executable))
            return 0;
        collector.leafExecutables().set(executable, decoded->value);
    }
    JSC::BytecodeCacheError error;
    RefPtr encoded = JSC::encodeFunctionCodeBlock(executable->vm(), function.codeBlock.get(), error);
    if (!encoded || error.isValid())
        return 0;
    auto size = encoded->size();
    if (!collector.addFunctionUpdate(executable, function.kind, encoded.releaseNonNull()))
        return 0;
    script.pendingBytes += size;
    store().pendingBytes += size;
    return size;
}

// A script whose waiting functions are done or dropped leaves the queue. JS lock held: its handles go here.
void finishQueuedScript(PendingScript&& entry, size_t bytes)
{
    auto& script = *entry.script;
    store().deferredFunctions -= script.deferred.size() - script.nextDeferred;
    script.deferred.clear();
    script.nextDeferred = 0;
    script.queued = false;
    if (bytes)
        notePending(entry.provider, script);
}

// Encodes waiting functions, in the queue's order, until `deadline` (at least one); false when some are left.
bool encodeDeferred(MonotonicTime deadline)
{
    auto& s = store();
    if (s.encodeQueue.isEmpty())
        return true;
    JSC::JSLockHolder lock(WebCore::commonVM());
    auto start = MonotonicTime::now();
    bool first = true;
    bool finished = true;
    while (!s.encodeQueue.isEmpty()) {
        auto& script = *s.encodeQueue.first().script;
        size_t bytes = 0;
        while (script.nextDeferred < script.deferred.size()) {
            if (!first && MonotonicTime::now() >= deadline) {
                finished = false;
                break;
            }
            first = false;
            // Out of the vector before encoding: its handles go at the end of this iteration, with the lock held.
            auto function = WTF::move(script.deferred[script.nextDeferred++]);
            --s.deferredFunctions;
            if (script.failed)
                continue;
            bytes += encodeFunction(script, function);
            ++s.encoded;
        }
        if (!finished) {
            if (bytes)
                notePending(s.encodeQueue.first().provider, script);
            break;
        }
        finishQueuedScript(s.encodeQueue.takeFirst(), bytes);
    }
    s.encodeTime += MonotonicTime::now() - start;
    return finished;
}

// Every waiting function goes, encoded or not. Before a clear, and when a budget runs out.
void dropDeferred()
{
    auto& s = store();
    if (s.encodeQueue.isEmpty())
        return;
    JSC::JSLockHolder lock(WebCore::commonVM());
    s.droppedFunctions += s.deferredFunctions;
    while (!s.encodeQueue.isEmpty())
        finishQueuedScript(s.encodeQueue.takeFirst(), 0);
}

void scheduleEncoding();

void encodeSlice()
{
    auto& s = store();
    s.encodeScheduled = false;
    if (!s.enabled)
        return;
    encodeDeferred(MonotonicTime::now() + encodeSliceDuration);
    scheduleEncoding();
}

void scheduleEncoding()
{
    auto& s = store();
    if (s.encodeScheduled || s.encodeQueue.isEmpty())
        return;
    s.encodeScheduled = true;
    RunLoop::mainSingleton().dispatch(RunLoop::Priority::Low, [] {
        encodeSlice();
    });
}

void commitScripts(const char* reason, bool leaving, Function<void()>&& done);

void commitTimerFired()
{
    commitScripts("a pagina ferma", std::exchange(store().commitEverything, false), nullptr);
}

// Whether a script's new functions are worth a write now.
bool worthWriting(const Script& script, uint64_t baseSize, bool leaving)
{
    if (!baseSize)
        return true;
    if (leaving)
        return script.pendingBytes >= std::max<uint64_t>(minimumUpdateBytesLeaving, baseSize / 16);
    return script.pendingBytes >= std::max<uint64_t>(minimumUpdateBytes, baseSize / 4);
}

// A script has something to write: unless a commit is already coming, one in a while.
void notePending(const JSC::SourceProvider& provider, Script& script)
{
    auto& s = store();
    if (!std::exchange(script.pending, true))
        s.pending.append({ Ref { const_cast<JSC::SourceProvider&>(provider) }, &script });
    if (s.pendingBytes > maximumPendingBytes) {
        s.commitEverything = true;
        if (!s.commitTimer->isActive() || s.commitTimer->secondsUntilFire() > 1_s)
            s.commitTimer->startOneShot(1_s);
    } else if (!s.commitTimer->isActive())
        s.commitTimer->startOneShot(pendingCommitDelay);
}

void dropPending(Script& script)
{
    auto& s = store();
    s.pendingBytes -= std::min(s.pendingBytes, script.pendingBytes);
    script.pendingBytes = 0;
}

// The main thread's half of a commit: each script's new functions leave its collector (takeUpdates, which costs nothing)
// for the writer queue, which copies, writes and renames; the bookkeeping comes back here. `leaving`: the app is being
// suspended, or memory is short.
void commitScripts(const char* reason, bool leaving, Function<void()>&& done)
{
    auto& s = store();
    if (s.commitTimer)
        s.commitTimer->stop();
    if (!s.enabled) {
        if (done)
            done();
        return;
    }
    auto start = MonotonicTime::now();
    auto batch = std::make_unique<Batch>();
    Vector<Write> writes;
    Vector<PendingScript> waiting;
    uint64_t newBytes = 0;
    for (auto& entry : std::exchange(s.pending, { })) {
        auto& script = *entry.script;
        if (script.failed || !script.collector || !script.collector->hasUpdates()) {
            dropPending(script);
            script.pending = false;
            continue;
        }
        auto baseSize = script.collector->baseSize();
        if (!worthWriting(script, baseSize, leaving)) {
            waiting.append(WTF::move(entry));
            continue;
        }
        Ref updates = script.collector->takeUpdates();
        auto generation = s.nextGeneration++;
        Write write;
        write.key = script.key;
        write.generation = generation;
        if (baseSize)
            write.basePath = filePath(script.key, script.baseGeneration).isolatedCopy();
        write.baseSize = baseSize;
        write.path = filePath(script.key, generation).isolatedCopy();
        write.size = updates->sizeForUpdate();
        write.updates = updates.ptr();
        writes.append(WTF::move(write));
        newBytes += updates->sizeForUpdate() - baseSize;
        script.baseGeneration = generation;
        dropPending(script);
        script.pending = false;
        batch->updates.append(WTF::move(updates));
        batch->scripts.append(WTF::move(entry));
    }
    s.pending = WTF::move(waiting);
    saveSeen();
    if (writes.isEmpty()) {
        // A suspension waits for the files already on their way.
        if (done) {
            s.writer->dispatch([done = WTF::move(done)]() mutable {
                callOnMainThread(WTF::move(done));
            });
        }
        return;
    }
    log("bytecode: scrivo %zu script, %llu KB nuovi (%s, preparati in %.1f ms)", writes.size(), static_cast<unsigned long long>(newBytes >> 10),
        reason, (MonotonicTime::now() - start).milliseconds());
    s.writer->dispatch([writes = WTF::move(writes), batch = batch.release(), done = WTF::move(done)]() mutable {
        auto writeStart = MonotonicTime::now();
        uint64_t copied = 0;
        for (auto& write : writes) {
            write.written = writeImage(write);
            copied += write.baseSize;
        }
        auto elapsed = MonotonicTime::now() - writeStart;
        callOnMainThread([writes = WTF::move(writes), batch, done = WTF::move(done), elapsed, copied]() mutable {
            std::unique_ptr<Batch> owned(batch);
            auto& s = store();
            Vector<String> obsolete;
            unsigned failures = 0;
            for (size_t i = 0; i < writes.size(); ++i) {
                auto& write = writes[i];
                if (!write.written) {
                    ++failures;
                    auto& script = *owned->scripts[i].script;
                    script.failed = true;
                    script.collector = nullptr;
                    dropPending(script);
                    continue;
                }
                ++s.stored;
                s.storedBytes += write.size - write.baseSize;
                auto& entry = s.files.add(write.key, FileEntry { }).iterator->value;
                if (write.generation > entry.generation) {
                    if (entry.generation)
                        obsolete.append(filePath(write.key, entry.generation));
                    s.totalSize -= entry.size;
                    entry = { write.generation, write.size, WallTime::now() };
                    s.totalSize += write.size;
                } else // A newer file of the same script, from another page: that one stays.
                    obsolete.append(write.path);
            }
            evictIfNeeded(obsolete);
            deleteFiles(WTF::move(obsolete));
            if (failures) {
                s.writeFailures += failures;
                log("bytecode: %u file non scritti", failures);
            }
            if (Lumia::traceEnabled())
                log("bytecode: %zu file scritti in %.0f ms (%llu KB copiati dai precedenti)", writes.size() - failures, elapsed.milliseconds(),
                    static_cast<unsigned long long>(copied >> 10));
            // The scripts and their updates go here, on the main thread.
            owned = nullptr;
            if (done)
                done();
        });
    });
}

} // namespace

void initialize()
{
    auto& s = store();
    auto* cacheDirectory = config().cache_directory;
    if (!cacheDirectory || s.enabled)
        return;
    auto start = MonotonicTime::now();
    auto root = FileSystem::pathByAppendingComponent(FileSystem::pathByAppendingComponent(String(cacheDirectory), "WebKitCache"_s), "JSBytecode"_s);
    auto build = buildIdentity();
    s.directory = FileSystem::pathByAppendingComponent(root, build);
    if (!FileSystem::makeAllDirectories(s.directory)) {
        log("bytecode: impossibile creare %s", s.directory.utf8().data());
        return;
    }
    s.writer = WorkQueue::create("BytecodeCache"_s, WorkQueue::QOS::Background);
    s.commitTimer = makeUnique<RunLoop::Timer>(RunLoop::mainSingleton(), "BytecodeCache::commitTimer"_s, [] { commitTimerFired(); });

    // Another engine's files are of no use to this one.
    Vector<String> otherBuilds;
    for (auto& name : FileSystem::listDirectory(root)) {
        if (name != build)
            otherBuilds.append(FileSystem::pathByAppendingComponent(root, name));
    }

    // Older generations and leftovers of interrupted writes; then, beyond the capacity, the files least recently used.
    // Everything, with a new secret.
    bool sameSecret = loadSecret();
    Vector<String> obsolete;
    for (auto& name : FileSystem::listDirectory(s.directory)) {
        auto path = FileSystem::pathByAppendingComponent(s.directory, name);
        if (name.endsWith(".tmp"_s) || (!sameSecret && (name.endsWith(".jsc"_s) || name == "seen.bin"_s))) {
            obsolete.append(path);
            continue;
        }
        auto parsed = parseFileName(name);
        if (!parsed)
            continue;
        auto [key, generation] = *parsed;
        s.nextGeneration = std::max(s.nextGeneration, generation + 1);
        FileEntry file { generation, FileSystem::fileSize(path).value_or(0), FileSystem::fileModificationTime(path).value_or(WallTime()) };
        auto result = s.files.add(key, file);
        if (!result.isNewEntry) {
            if (result.iterator->value.generation >= generation) {
                obsolete.append(path);
                continue;
            }
            obsolete.append(filePath(key, result.iterator->value.generation));
            s.totalSize -= result.iterator->value.size;
            result.iterator->value = file;
        }
        s.totalSize += file.size;
    }
    evictIfNeeded(obsolete);
    if (sameSecret)
        loadSeen();
    s.enabled = true;

    s.writer->dispatch([otherBuilds = crossThreadCopy(WTF::move(otherBuilds)), obsolete = crossThreadCopy(WTF::move(obsolete))] {
        for (auto& path : otherBuilds) {
            deleteDirectoryContents(path);
            FileSystem::deleteEmptyDirectory(path);
        }
        for (auto& path : obsolete)
            FileSystem::deleteFile(path);
    });

    log("bytecode: %u file, %llu KB in %s (%.0f ms)", s.files.size(), static_cast<unsigned long long>(s.totalSize >> 10),
        build.utf8().data(), (MonotonicTime::now() - start).milliseconds());
}

void scheduleCommit(Seconds delay)
{
    auto& s = store();
    if (!s.enabled || s.pending.isEmpty())
        return;
    s.commitTimer->startOneShot(delay);
}

void commit(const char* reason, Function<void()>&& done)
{
    // The functions still waiting, as many as the budget allows: the app may not come back.
    auto& s = store();
    if (s.enabled && !s.encodeQueue.isEmpty()) {
        auto budget = !strcmp(reason, "memoria") ? memoryEncodeBudget : suspensionEncodeBudget;
        auto before = s.encoded;
        auto start = MonotonicTime::now();
        auto left = s.deferredFunctions;
        if (!encodeDeferred(start + budget)) {
            left = s.deferredFunctions;
            dropDeferred();
        } else
            left = 0;
        log("bytecode: %u funzioni codificate in %.0f ms (%s)%s", s.encoded - before, (MonotonicTime::now() - start).milliseconds(), reason,
            left ? makeString(", "_s, left, " lasciate"_s).utf8().data() : "");
    }
    commitScripts(reason, true, WTF::move(done));
}

void clear()
{
    auto& s = store();
    if (!s.enabled)
        return;
    dropDeferred();
    s.files.clear();
    s.totalSize = 0;
    s.seen.clear();
    s.seenOrder.clear();
    s.seenChanged = false;
    // The files and the seen list; the secret stays.
    s.writer->dispatch([directory = s.directory.isolatedCopy()] {
        for (auto& name : FileSystem::listDirectory(directory)) {
            if (name.endsWith(".jsc"_s) || name.startsWith("seen.bin"_s))
                FileSystem::deleteFile(FileSystem::pathByAppendingComponent(directory, name));
        }
    });
}

bool toggle()
{
    auto& s = store();
    s.bypassed = !s.bypassed;
    return s.enabled && !s.bypassed;
}

String describe()
{
    auto& s = store();
    if (!s.enabled)
        return "bytecode: spento"_s;
    return makeString("bytecode: "_s, s.bypassed ? "spento a mano, "_s : ""_s, s.files.size(), " file, "_s, s.totalSize >> 10, " KB; in questa esecuzione "_s,
        s.hits, " dal disco ("_s, s.hitBytes >> 10, " KB), "_s, s.rejected, " non validi, "_s, s.misses, " da compilare, "_s,
        s.stored, " scritti ("_s, s.storedBytes >> 10, " KB nuovi), in attesa "_s, s.pending.size(), " ("_s, s.pendingBytes >> 10, " KB)"_s,
        s.writeFailures ? makeString(", "_s, s.writeFailures, " errori di scrittura"_s) : String(),
        "; funzioni codificate a riposo "_s, s.encoded, " in "_s, static_cast<unsigned>(s.encodeTime.milliseconds()), " ms, da codificare "_s,
        s.deferredFunctions, s.droppedFunctions ? makeString(", lasciate "_s, s.droppedFunctions) : String());
}

} // namespace Lumia::BytecodeCache

namespace WebCore {

using namespace Lumia::BytecodeCache;

ScriptBytecodeCacheState::ScriptBytecodeCacheState(Origin origin)
    : m_origin(origin)
{
}

ScriptBytecodeCacheState::~ScriptBytecodeCacheState() = default;

// The script runs in a private tab's page: what a site's code compiles to would say on disk that it was visited, so
// such scripts neither read nor write the cache. The hooks run inside the evaluation (or the lazy compilation) of the
// script, whose global object is the entry scope's.
static bool runsInPrivatePage()
{
    auto* scope = commonVM().entryScope;
    auto* window = scope ? dynamicDowncast<JSDOMWindowBase>(scope->globalObject()) : nullptr;
    if (!window)
        return false;
    RefPtr document = window->wrapped().documentIfLocal();
    auto* page = document ? document->page() : nullptr;
    return page && page->sessionID().isEphemeral();
}

ScriptBytecodeCacheState::Script* ScriptBytecodeCacheState::script(const JSC::SourceProvider& provider) const
{
    if (m_checked)
        return m_script.get();
    m_checked = true;
    if (!isMainThread() || !store().enabled || store().bypassed || runsInPrivatePage())
        return nullptr;
    auto source = provider.source();
    auto minimum = m_origin == Origin::External ? minimumExternalLength : minimumInlineLength;
    if (source.length() < minimum || source.length() > maximumLength)
        return nullptr;
    auto type = provider.sourceType();
    if (type != JSC::SourceProviderSourceType::Program && type != JSC::SourceProviderSourceType::Module)
        return nullptr;
    m_script = std::make_unique<Script>();
    m_script->key = contentKey(source, type);
    m_script->origin = m_origin;
    return m_script.get();
}

RefPtr<JSC::CachedBytecode> ScriptBytecodeCacheState::cachedBytecode(const JSC::SourceProvider& provider) const
{
    auto* script = this->script(provider);
    if (!script)
        return nullptr;
    if (std::exchange(script->readDisk, true))
        return script->decoded;
    auto& s = store();
    auto it = s.files.find(script->key);
    if (it == s.files.end()) {
        ++s.misses;
        return nullptr;
    }
    auto path = filePath(script->key, it->value.generation);
    std::optional<FileSystem::MappedFileData> mapped;
    if (auto handle = FileSystem::openFile(path, FileSystem::FileOpenMode::Read))
        mapped = handle.map(FileSystem::MappedFileMode::Private);
    if (!mapped || !mapped->size()) {
        // Removed behind our back: compile, and write it again.
        s.totalSize -= it->value.size;
        s.files.remove(it);
        ++s.misses;
        return nullptr;
    }
    // Mapped: the pages are the file's, clean and droppable by the system, not the app's memory.
    script->decoded = JSC::CachedBytecode::create(WTF::move(*mapped));
    script->baseGeneration = it->value.generation;
    it->value.lastUsed = WallTime::now();
    s.writer->dispatch([path = path.isolatedCopy()] {
        FileSystem::updateFileModificationTime(path);
    });
    ++s.hits;
    s.hitBytes += script->decoded->size();
    if (Lumia::traceEnabled()) {
        auto url = provider.sourceURL();
        Lumia::log("bytecode: dal disco %zu KB, %s", script->decoded->size() >> 10, url.isEmpty() ? "script nella pagina" : url.left(120).utf8().data());
    }
    return script->decoded;
}

void ScriptBytecodeCacheState::cacheBytecode(const JSC::SourceProvider& provider, const JSC::BytecodeCacheGenerator& generator) const
{
    // JSC compiled the whole script: nothing usable came from the disk (no file, or one that does not match), and what
    // was collected for code from before is of no use either.
    auto* script = this->script(provider);
    if (!script)
        return;
    auto& s = store();
    if (std::exchange(script->decoded, nullptr))
        ++s.rejected;
    dropPending(*script);
    // Functions waiting from before belong to the image being thrown away (the handles go with the JS lock held:
    // this runs inside JSC's compilation of the script).
    if (script->nextDeferred < script->deferred.size()) {
        s.droppedFunctions += script->deferred.size() - script->nextDeferred;
        s.deferredFunctions -= script->deferred.size() - script->nextDeferred;
        script->deferred.shrink(script->nextDeferred);
    }
    script->collector = nullptr;
    script->baseGeneration = 0;
    script->failed = false;
    if (script->origin == Origin::Inline && !s.seen.contains(script->key)) {
        s.seen.add(script->key);
        s.seenOrder.append(script->key);
        s.seenChanged = true;
        return;
    }
    RefPtr update = generator();
    if (!update)
        return;
    auto size = update->size();
    Ref collector = JSC::CachedBytecode::create();
    collector->addGlobalUpdate(update.releaseNonNull());
    script->collector = WTF::move(collector);
    script->pendingBytes = size;
    s.pendingBytes += size;
    notePending(provider, *script);
}

void ScriptBytecodeCacheState::updateCache(const JSC::SourceProvider& provider, const JSC::UnlinkedFunctionExecutable* executable, JSC::CodeSpecializationKind kind, const JSC::UnlinkedFunctionCodeBlock* codeBlock) const
{
    // A function compiled for the first time: its code goes in after what the file has, pointed at by the function's
    // record (encodeFunction), once the encoder gets to it. Inside JSC's compilation of the function: the JS lock is held.
    auto* script = m_script.get();
    if (!script || script->failed || !isMainThread() || !executable || !codeBlock)
        return;
    if (!script->collector && !script->decoded)
        return;
    auto& s = store();
    auto& vm = executable->vm();
    script->deferred.append(Script::Deferred(vm, const_cast<JSC::UnlinkedFunctionExecutable*>(executable),
        const_cast<JSC::UnlinkedFunctionCodeBlock*>(codeBlock), kind));
    ++s.deferredFunctions;
    if (!std::exchange(script->queued, true))
        s.encodeQueue.append({ Ref { const_cast<JSC::SourceProvider&>(provider) }, script });
    scheduleEncoding();
}

bool shouldCacheInlineScriptBytecode(const String& source, JSC::SourceProviderSourceType type)
{
    if (!isMainThread() || source.length() < minimumInlineLength || source.length() > maximumLength)
        return false;
    if (type != JSC::SourceProviderSourceType::Program && type != JSC::SourceProviderSourceType::Module)
        return false;
    return store().enabled && !store().bypassed;
}

} // namespace WebCore
