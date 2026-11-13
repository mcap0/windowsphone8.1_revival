#include "config.h"
#include "CacheStorage.h"

#include "Engine.h"
#include <WebCore/DOMCacheEngine.h>
#include <wtf/CrossThreadCopier.h>

// An identifier and a flag, resolved on the Web thread: WebKit2 sends it through IPC, this port hands it over as it is.
namespace WTF {
template<> struct CrossThreadCopierBase<false, false, WebCore::DOMCacheEngine::CacheIdentifierOperationResult> {
    using Type = WebCore::DOMCacheEngine::CacheIdentifierOperationResult;
    static constexpr bool IsNeeded = false;
    static Type copy(const Type& value) { return value; }
};
}

#include <WebCore/CacheQueryOptions.h>
#include <WebCore/CacheStorageConnection.h>
#include <WebCore/CacheStorageProvider.h>
#include <WebCore/ClientOrigin.h>
#include <WebCore/CrossOriginAccessControl.h>
#include <WebCore/DOMCacheEngine.h>
#include <WebCore/FetchOptions.h>
#include <WebCore/FormData.h>
#include <WebCore/HTTPHeaderMap.h>
#include <WebCore/OriginAccessPatterns.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/RetrieveRecordsOptions.h>
#include <WebCore/SecurityOrigin.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/WebCorePersistentCoders.h>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/SHA1.h>
#include <wtf/UUID.h>
#include <wtf/WorkQueue.h>
#include <wtf/persistence/PersistentCoders.h>
#include <wtf/persistence/PersistentDecoder.h>
#include <wtf/persistence/PersistentEncoder.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>


namespace Lumia {
using namespace WebCore;
using namespace WebCore::DOMCacheEngine;

namespace {

// What one origin may keep, in body bytes. The phone's data partition is shared with everything else.
constexpr uint64_t originQuota = 200 * 1024 * 1024;
constexpr unsigned indexVersion = 1;

// ---------------------------------------------------------------------------------------------------------------
// Files. Everything here runs on the queue.

struct StoredHeader {
    double insertionTime { 0 };
    FetchHeaders::Guard requestHeadersGuard { FetchHeaders::Guard::None };
    ResourceRequest request;
    FetchOptions options;
    String referrer;
    FetchHeaders::Guard responseHeadersGuard { FetchHeaders::Guard::None };
    ResourceResponse::CrossThreadData response;
    uint64_t responseBodySize { 0 };
};

Vector<uint8_t> encodeHeader(const StoredHeader& header)
{
    WTF::Persistence::Encoder encoder;
    encoder << header.insertionTime;
    encoder << header.requestHeadersGuard;
    encoder << header.request;
    header.options.encodePersistent(encoder);
    encoder << header.referrer;
    encoder << header.responseHeadersGuard;
    encoder << header.response;
    encoder << header.responseBodySize;
    encoder.encodeChecksum();
    return { encoder.span() };
}

std::optional<StoredHeader> decodeHeader(std::span<const uint8_t> data)
{
    WTF::Persistence::Decoder decoder(data);
    StoredHeader header;
    std::optional<double> insertionTime;
    decoder >> insertionTime;
    std::optional<FetchHeaders::Guard> requestHeadersGuard;
    decoder >> requestHeadersGuard;
    std::optional<ResourceRequest> request;
    decoder >> request;
    if (!insertionTime || !requestHeadersGuard || !request)
        return std::nullopt;
    if (!FetchOptions::decodePersistent(decoder, header.options))
        return std::nullopt;
    std::optional<String> referrer;
    decoder >> referrer;
    std::optional<FetchHeaders::Guard> responseHeadersGuard;
    decoder >> responseHeadersGuard;
    std::optional<ResourceResponse::CrossThreadData> response;
    decoder >> response;
    std::optional<uint64_t> responseBodySize;
    decoder >> responseBodySize;
    if (!referrer || !responseHeadersGuard || !response || !responseBodySize || !decoder.verifyChecksum())
        return std::nullopt;
    header.insertionTime = *insertionTime;
    header.requestHeadersGuard = *requestHeadersGuard;
    header.request = WTF::move(*request);
    header.referrer = WTF::move(*referrer);
    header.responseHeadersGuard = *responseHeadersGuard;
    header.response = WTF::move(*response);
    header.responseBodySize = *responseBodySize;
    return header;
}

String headerPath(const String& cacheDirectory, uint64_t file)
{
    return FileSystem::pathByAppendingComponent(cacheDirectory, makeString(file, ".rec"_s));
}

String bodyPath(const String& cacheDirectory, uint64_t file)
{
    return FileSystem::pathByAppendingComponent(cacheDirectory, makeString(file, ".body"_s));
}

std::optional<StoredHeader> readHeader(const String& cacheDirectory, uint64_t file)
{
    auto data = FileSystem::readEntireFile(headerPath(cacheDirectory, file));
    if (!data)
        return std::nullopt;
    return decodeHeader(data->span());
}

// The body first: a record without its header does not exist, one without its body would be served empty.
bool writeRecord(const String& cacheDirectory, uint64_t file, const StoredHeader& header, std::span<const uint8_t> body)
{
    FileSystem::makeAllDirectories(cacheDirectory);
    if (!FileSystem::overwriteEntireFile(bodyPath(cacheDirectory, file), body))
        return false;
    auto encoded = encodeHeader(header);
    if (!FileSystem::overwriteEntireFile(headerPath(cacheDirectory, file), encoded.span())) {
        FileSystem::deleteFile(bodyPath(cacheDirectory, file));
        return false;
    }
    return true;
}

void deleteRecord(const String& cacheDirectory, uint64_t file)
{
    FileSystem::deleteFile(headerPath(cacheDirectory, file));
    FileSystem::deleteFile(bodyPath(cacheDirectory, file));
}

struct IndexEntry {
    String name;
    String directoryName;

    IndexEntry isolatedCopy() const { return { name.isolatedCopy(), directoryName.isolatedCopy() }; }
};

void writeIndex(const String& originDirectory, const ClientOrigin& origin, const Vector<IndexEntry>& entries)
{
    FileSystem::makeAllDirectories(originDirectory);
    {
        WTF::Persistence::Encoder encoder;
        encoder << origin;
        encoder.encodeChecksum();
        FileSystem::overwriteEntireFile(FileSystem::pathByAppendingComponent(originDirectory, "origin"_s), encoder.span());
    }
    WTF::Persistence::Encoder encoder;
    encoder << indexVersion;
    encoder << static_cast<uint64_t>(entries.size());
    for (auto& entry : entries) {
        encoder << entry.name;
        encoder << entry.directoryName;
    }
    encoder.encodeChecksum();
    FileSystem::overwriteEntireFile(FileSystem::pathByAppendingComponent(originDirectory, "index"_s), encoder.span());
}

Vector<IndexEntry> readIndex(const String& originDirectory)
{
    auto data = FileSystem::readEntireFile(FileSystem::pathByAppendingComponent(originDirectory, "index"_s));
    if (!data)
        return { };
    WTF::Persistence::Decoder decoder(data->span());
    std::optional<unsigned> version;
    decoder >> version;
    std::optional<uint64_t> count;
    decoder >> count;
    if (!version || *version != indexVersion || !count)
        return { };
    Vector<IndexEntry> entries;
    for (uint64_t i = 0; i < *count; ++i) {
        std::optional<String> name;
        decoder >> name;
        std::optional<String> directoryName;
        decoder >> directoryName;
        if (!name || !directoryName)
            return { };
        entries.append({ WTF::move(*name), WTF::move(*directoryName) });
    }
    if (!decoder.verifyChecksum())
        return { };
    return entries;
}

std::optional<ClientOrigin> readOrigin(const String& originDirectory)
{
    auto data = FileSystem::readEntireFile(FileSystem::pathByAppendingComponent(originDirectory, "origin"_s));
    if (!data)
        return std::nullopt;
    WTF::Persistence::Decoder decoder(data->span());
    std::optional<ClientOrigin> origin;
    decoder >> origin;
    if (!origin || !decoder.verifyChecksum())
        return std::nullopt;
    return origin;
}

// ---------------------------------------------------------------------------------------------------------------
// What the Web thread knows of a record without opening its files.

struct RecordInfo {
    uint64_t identifier { 0 };
    uint64_t file { 0 };
    double insertionTime { 0 };
    uint64_t size { 0 };
    URL url;
    HTTPHeaderMap requestHeaders;
    bool hasVaryStar { false };
    HashMap<String, String> varyHeaders;

    // As WebKit2's CacheStorageRecordInformation::updateVaryHeaders.
    void updateVaryHeaders(const ResourceResponse::CrossThreadData& response)
    {
        hasVaryStar = false;
        varyHeaders = { };
        auto varyValue = response.httpHeaderFields.get(HTTPHeaderName::Vary);
        if (varyValue.isNull() || response.tainting == ResourceResponse::Tainting::Opaque || response.tainting == ResourceResponse::Tainting::Opaqueredirect)
            return;
        varyValue.split(',', [&](StringView view) {
            if (!hasVaryStar && view.trim(isASCIIWhitespaceWithoutFF<char16_t>) == "*"_s)
                hasVaryStar = true;
            varyHeaders.add(view.toString(), requestHeaders.get(view));
        });
        if (hasVaryStar)
            varyHeaders = { };
    }

    RecordInfo isolatedCopy() &&
    {
        return { identifier, file, insertionTime, size, WTF::move(url).isolatedCopy(), WTF::move(requestHeaders).isolatedCopy(), hasVaryStar, crossThreadCopy(WTF::move(varyHeaders)) };
    }
};

RecordInfo infoFromHeader(const StoredHeader& header, uint64_t file, uint64_t bodySize)
{
    RecordInfo info;
    info.file = file;
    info.insertionTime = header.insertionTime;
    info.size = bodySize;
    info.url = header.request.url();
    info.requestHeaders = header.request.httpHeaderFields();
    info.updateVaryHeaders(header.response);
    return info;
}

struct LoadedCache {
    IndexEntry entry;
    Vector<RecordInfo> records;
    uint64_t nextFile { 1 };

    LoadedCache isolatedCopy() && { return { entry.isolatedCopy(), crossThreadCopy(WTF::move(records)), nextFile }; }
};

Vector<LoadedCache> loadOrigin(const String& originDirectory)
{
    Vector<LoadedCache> caches;
    for (auto& entry : readIndex(originDirectory)) {
        LoadedCache cache { entry, { }, 1 };
        auto cacheDirectory = FileSystem::pathByAppendingComponent(originDirectory, entry.directoryName);
        for (auto& name : FileSystem::listDirectory(cacheDirectory)) {
            if (!name.endsWith(".rec"_s))
                continue;
            auto file = parseInteger<uint64_t>(StringView { name }.left(name.length() - 4));
            if (!file)
                continue;
            cache.nextFile = std::max(cache.nextFile, *file + 1);
            auto header = readHeader(cacheDirectory, *file);
            auto bodySize = FileSystem::fileSize(bodyPath(cacheDirectory, *file));
            if (!header || !bodySize) {
                deleteRecord(cacheDirectory, *file);
                continue;
            }
            cache.records.append(infoFromHeader(*header, *file, *bodySize));
        }
        std::ranges::sort(cache.records, [](auto& a, auto& b) {
            return a.insertionTime != b.insertionTime ? a.insertionTime < b.insertionTime : a.file < b.file;
        });
        caches.append(WTF::move(cache));
    }
    return caches;
}

// ---------------------------------------------------------------------------------------------------------------
// The engine. Web thread.

struct Cache : public RefCounted<Cache> {
    static Ref<Cache> create() { return adoptRef(*new Cache); }

    DOMCacheIdentifier identifier { DOMCacheIdentifier::generate() };
    ClientOrigin origin;
    String name;
    String directoryName;
    String directory;
    Vector<RecordInfo> records; // In insertion order.
    uint64_t nextFile { 1 };
    unsigned references { 0 };
    bool removed { false };
};

struct OriginState {
    WTF_DEPRECATED_MAKE_FAST_ALLOCATED(OriginState);
public:
    String directory;
    bool loaded { false };
    Vector<Function<void()>> waiting;
    Vector<Ref<Cache>> caches;
    uint64_t updateCounter { 1 };

    uint64_t size() const
    {
        uint64_t total = 0;
        for (auto& cache : caches) {
            for (auto& record : cache->records)
                total += record.size;
        }
        return total;
    }
};

class Engine {
public:
    static Engine& singleton()
    {
        static NeverDestroyed<Engine> engine;
        return engine;
    }

    Ref<CacheStorageConnection::OpenPromise> open(const ClientOrigin& origin, const String& name)
    {
        CacheStorageConnection::OpenPromise::Producer producer;
        Ref promise = producer.promise();
        withOrigin(origin, [this, origin, name, producer = WTF::move(producer)](OriginState* state) mutable {
            if (!state)
                return producer.reject(Error::WriteDisk);
            for (auto& cache : state->caches) {
                if (cache->name == name)
                    return producer.resolve(CacheIdentifierOperationResult { cache->identifier, false });
            }
            Ref cache = Cache::create();
            cache->origin = origin;
            cache->name = name;
            cache->directoryName = createVersion4UUIDString();
            cache->directory = FileSystem::pathByAppendingComponent(state->directory, cache->directoryName);
            state->caches.append(cache);
            m_caches.add(cache->identifier, cache);
            ++state->updateCounter;
            saveIndex(origin, *state);
            producer.resolve(CacheIdentifierOperationResult { cache->identifier, false });
        });
        return promise;
    }

    Ref<CacheStorageConnection::RemovePromise> remove(DOMCacheIdentifier identifier)
    {
        RefPtr cache = m_caches.get(identifier);
        if (!cache || cache->removed)
            return CacheStorageConnection::RemovePromise::createAndResolve(false);
        auto* state = m_origins.get(cache->origin);
        if (!state)
            return CacheStorageConnection::RemovePromise::createAndResolve(false);
        state->caches.removeFirstMatching([&](auto& item) { return item.ptr() == cache.get(); });
        ++state->updateCounter;
        saveIndex(cache->origin, *state);
        // A page that still holds the cache keeps reading it: its files go with its last reference.
        cache->removed = true;
        if (!cache->references)
            destroy(*cache);
        return CacheStorageConnection::RemovePromise::createAndResolve(true);
    }

    Ref<CacheStorageConnection::RetrieveCachesPromise> retrieveCaches(const ClientOrigin& origin, uint64_t)
    {
        CacheStorageConnection::RetrieveCachesPromise::Producer producer;
        Ref promise = producer.promise();
        withOrigin(origin, [producer = WTF::move(producer)](OriginState* state) mutable {
            if (!state)
                return producer.resolve(CacheInfos { { }, 1 });
            auto infos = WTF::map(state->caches, [](auto& cache) {
                return CacheInfo { cache->identifier, cache->name };
            });
            producer.resolve(CacheInfos { WTF::move(infos), state->updateCounter });
        });
        return promise;
    }

    Ref<CacheStorageConnection::RetrieveRecordsPromise> retrieveRecords(DOMCacheIdentifier identifier, RetrieveRecordsOptions&& options)
    {
        RefPtr cache = m_caches.get(identifier);
        if (!cache)
            return CacheStorageConnection::RetrieveRecordsPromise::createAndReject(Error::Internal);

        Vector<std::pair<uint64_t, uint64_t>> targets; // identifier, file
        auto& url = options.request.url();
        if (url.isNull()) {
            for (auto& record : cache->records)
                targets.append({ record.identifier, record.file });
        } else if (options.ignoreMethod || options.request.httpMethod() == "GET"_s) {
            CacheQueryOptions queryOptions { options.ignoreSearch, options.ignoreMethod, options.ignoreVary };
            for (auto& record : cache->records) {
                if (queryCacheMatch(options.request, record.url, record.hasVaryStar, record.varyHeaders, queryOptions))
                    targets.append({ record.identifier, record.file });
            }
        }
        if (targets.isEmpty())
            return CacheStorageConnection::RetrieveRecordsPromise::createAndResolve(Vector<CrossThreadRecord> { });

        CacheStorageConnection::RetrieveRecordsPromise::Producer producer;
        Ref promise = producer.promise();
        bool withResponse = options.shouldProvideResponse;
        m_queue->dispatch([directory = cache->directory.isolatedCopy(), targets = WTF::move(targets), withResponse, options = WTF::move(options).isolatedCopy(), producer = WTF::move(producer)]() mutable {
            Vector<CrossThreadRecord> result;
            for (auto& [identifier, file] : targets) {
                auto header = readHeader(directory, file);
                if (!header)
                    continue;
                CrossThreadRecord record { identifier, 0, header->requestHeadersGuard, WTF::move(header->request), header->options, WTF::move(header->referrer), header->responseHeadersGuard, { }, nullptr, 0 };
                if (withResponse) {
                    auto body = FileSystem::readEntireFile(bodyPath(directory, file));
                    if (!body)
                        continue;
                    record.response = WTF::move(header->response);
                    record.responseBodySize = header->responseBodySize;
                    record.responseBody = SharedBuffer::create(WTF::move(*body));
                }
                result.append(WTF::move(record));
            }
            callOnMainThread([result = WTF::move(result), options = WTF::move(options), producer = WTF::move(producer)]() mutable {
                // An opaque response is given only to a page whose embedder policy lets it embed it.
                for (auto& record : result) {
                    if (record.response.type != ResourceResponse::Type::Opaque)
                        continue;
                    if (validateCrossOriginResourcePolicy(options.crossOriginEmbedderPolicy.value, options.sourceOrigin, record.request.url(), false, record.response.url, record.response.httpHeaderFields.get(HTTPHeaderName::CrossOriginResourcePolicy), ForNavigation::No, EmptyOriginAccessPatterns::singleton()))
                        return producer.reject(Error::CORP);
                }
                std::ranges::sort(result, { }, &CrossThreadRecord::identifier);
                producer.resolve(WTF::move(result));
            });
        });
        return promise;
    }

    Ref<CacheStorageConnection::BatchPromise> batchDelete(DOMCacheIdentifier identifier, const ResourceRequest& request, CacheQueryOptions&& options)
    {
        RefPtr cache = m_caches.get(identifier);
        if (!cache)
            return CacheStorageConnection::BatchPromise::createAndReject(Error::Internal);
        Vector<uint64_t> identifiers;
        Vector<uint64_t> files;
        if (options.ignoreMethod || request.httpMethod() == "GET"_s) {
            cache->records.removeAllMatching([&](auto& record) {
                if (!queryCacheMatch(request, record.url, record.hasVaryStar, record.varyHeaders, options))
                    return false;
                identifiers.append(record.identifier);
                files.append(record.file);
                return true;
            });
        }
        if (files.isEmpty())
            return CacheStorageConnection::BatchPromise::createAndResolve(WTF::move(identifiers));

        CacheStorageConnection::BatchPromise::Producer producer;
        Ref promise = producer.promise();
        m_queue->dispatch([directory = cache->directory.isolatedCopy(), files = WTF::move(files), identifiers = WTF::move(identifiers), producer = WTF::move(producer)]() mutable {
            for (auto file : files)
                deleteRecord(directory, file);
            callOnMainThread([identifiers = WTF::move(identifiers), producer = WTF::move(producer)]() mutable {
                producer.resolve(WTF::move(identifiers));
            });
        });
        return promise;
    }

    Ref<CacheStorageConnection::BatchPromise> batchPut(DOMCacheIdentifier identifier, Vector<CrossThreadRecord>&& records)
    {
        RefPtr cache = m_caches.get(identifier);
        if (!cache)
            return CacheStorageConnection::BatchPromise::createAndReject(Error::Internal);
        auto* state = m_origins.get(cache->origin);

        struct Write {
            uint64_t file;
            bool replaces; // The record exists: its request stays, the response is the new one.
            StoredHeader header;
            RefPtr<SharedBuffer> body;
        };
        Vector<Write> writes;
        Vector<uint64_t> identifiers;

        // First what it takes, then the changes: a put that does not fit changes nothing.
        uint64_t added = 0;
        uint64_t freed = 0;
        Vector<RefPtr<SharedBuffer>> bodies;
        for (auto& record : records) {
            RefPtr<SharedBuffer> body = WTF::switchOn(record.responseBody, [](const Ref<SharedBuffer>& buffer) -> RefPtr<SharedBuffer> {
                return buffer.ptr();
            }, [](const Ref<FormData>& formData) -> RefPtr<SharedBuffer> {
                return formData->asSharedBuffer();
            }, [](const std::nullptr_t&) -> RefPtr<SharedBuffer> {
                return nullptr;
            });
            added += body ? body->size() : 0;
            if (auto* existing = findExisting(*cache, record.request))
                freed += existing->size;
            bodies.append(WTF::move(body));
        }
        if (state && added > freed && state->size() + added - freed > originQuota)
            return CacheStorageConnection::BatchPromise::createAndReject(Error::QuotaExceeded);

        static uint64_t lastInsertion;
        for (size_t i = 0; i < records.size(); ++i) {
            auto& record = records[i];
            auto& body = bodies[i];
            uint64_t bodySize = body ? body->size() : 0;
            Write write;
            write.body = body;
            write.header.requestHeadersGuard = record.requestHeadersGuard;
            write.header.options = record.options;
            write.header.referrer = record.referrer;
            write.header.responseHeadersGuard = record.responseHeadersGuard;
            write.header.responseBodySize = record.responseBodySize;
            if (auto* existing = findExisting(*cache, record.request)) {
                existing->size = bodySize;
                existing->updateVaryHeaders(record.response);
                write.file = existing->file;
                write.replaces = true;
                write.header.insertionTime = existing->insertionTime;
                identifiers.append(existing->identifier);
            } else {
                RecordInfo info;
                info.identifier = nextRecordIdentifier();
                info.file = cache->nextFile++;
                // The order of keys() is the order of insertion, across restarts too.
                info.insertionTime = std::max<double>(WallTime::now().secondsSinceEpoch().milliseconds(), lastInsertion + 1);
                lastInsertion = info.insertionTime;
                info.size = bodySize;
                info.url = record.request.url();
                info.requestHeaders = record.request.httpHeaderFields();
                info.updateVaryHeaders(record.response);
                write.file = info.file;
                write.replaces = false;
                write.header.insertionTime = info.insertionTime;
                write.header.request = WTF::move(record.request);
                identifiers.append(info.identifier);
                cache->records.append(WTF::move(info));
            }
            write.header.response = WTF::move(record.response);
            writes.append(WTF::move(write));
        }

        CacheStorageConnection::BatchPromise::Producer producer;
        Ref promise = producer.promise();
        m_queue->dispatch([directory = cache->directory.isolatedCopy(), writes = WTF::move(writes), identifiers = WTF::move(identifiers), producer = WTF::move(producer)]() mutable {
            bool succeeded = true;
            for (auto& write : writes) {
                if (write.replaces) {
                    auto old = readHeader(directory, write.file);
                    if (!old) {
                        succeeded = false;
                        continue;
                    }
                    write.header.requestHeadersGuard = old->requestHeadersGuard;
                    write.header.request = WTF::move(old->request);
                    write.header.options = old->options;
                    write.header.referrer = WTF::move(old->referrer);
                }
                std::span<const uint8_t> bytes;
                if (write.body)
                    bytes = write.body->span();
                if (!writeRecord(directory, write.file, write.header, bytes))
                    succeeded = false;
            }
            callOnMainThread([succeeded, identifiers = WTF::move(identifiers), producer = WTF::move(producer)]() mutable {
                if (!succeeded)
                    return producer.reject(Error::WriteDisk);
                producer.resolve(WTF::move(identifiers));
            });
        });
        return promise;
    }

    void reference(DOMCacheIdentifier identifier)
    {
        if (RefPtr cache = m_caches.get(identifier))
            ++cache->references;
    }

    void dereference(DOMCacheIdentifier identifier)
    {
        RefPtr cache = m_caches.get(identifier);
        if (!cache || !cache->references)
            return;
        if (!--cache->references && cache->removed)
            destroy(*cache);
    }

    void clear(const SecurityOriginData* origin, CompletionHandler<void()>&& completionHandler)
    {
        // What is in memory first, so that nothing is written back.
        Vector<ClientOrigin> gone;
        for (auto& [clientOrigin, state] : m_origins) {
            if (origin && !clientOrigin.isRelated(*origin))
                continue;
            for (auto& cache : state->caches) {
                cache->removed = true;
                cache->records.clear();
                m_caches.remove(cache->identifier);
            }
            gone.append(clientOrigin);
        }
        for (auto& clientOrigin : gone)
            m_origins.remove(clientOrigin);

        std::optional<SecurityOriginData> isolatedOrigin;
        if (origin)
            isolatedOrigin = origin->isolatedCopy();
        m_queue->dispatch([root = m_root.isolatedCopy(), isolatedOrigin = WTF::move(isolatedOrigin), completionHandler = WTF::move(completionHandler)]() mutable {
            if (!root.isEmpty()) {
                for (auto& name : FileSystem::listDirectory(root)) {
                    auto directory = FileSystem::pathByAppendingComponent(root, name);
                    if (isolatedOrigin) {
                        auto stored = readOrigin(directory);
                        if (stored && !stored->isRelated(*isolatedOrigin))
                            continue;
                    }
                    FileSystem::deleteNonEmptyDirectory(directory);
                }
            }
            callOnMainThread(WTF::move(completionHandler));
        });
    }

private:
    friend class NeverDestroyed<Engine>;

    Engine()
        : m_queue(WorkQueue::create("Cache storage"_s))
    {
        if (auto* dataDirectory = config().data_directory)
            m_root = FileSystem::pathByAppendingComponent(String(dataDirectory), "CacheStorage"_s);
    }

    static uint64_t nextRecordIdentifier()
    {
        static uint64_t identifier;
        return ++identifier;
    }

    String directoryFor(const ClientOrigin& origin) const
    {
        if (m_root.isEmpty())
            return { };
        SHA1 sha1;
        sha1.addUTF8Bytes(origin.topOrigin.toString());
        sha1.addUTF8Bytes("|"_s);
        sha1.addUTF8Bytes(origin.clientOrigin.toString());
        return FileSystem::pathByAppendingComponent(m_root, String::fromLatin1(sha1.computeHexDigest().data()));
    }

    // The origin's caches are read from disk the first time it is used; what arrives meanwhile waits in order.
    void withOrigin(const ClientOrigin& origin, Function<void(OriginState*)>&& function)
    {
        auto& state = m_origins.ensure(origin, [&] {
            auto state = makeUnique<OriginState>();
            state->directory = directoryFor(origin);
            return state;
        }).iterator->value;

        if (state->loaded)
            return function(state.get());
        if (state->directory.isEmpty()) {
            // No data directory: nothing can be kept.
            return function(nullptr);
        }

        bool isFirst = state->waiting.isEmpty();
        state->waiting.append([this, origin, function = WTF::move(function)]() mutable {
            function(m_origins.get(origin));
        });
        if (!isFirst)
            return;

        m_queue->dispatch([this, origin = origin.isolatedCopy(), directory = state->directory.isolatedCopy()]() mutable {
            auto caches = loadOrigin(directory);
            callOnMainThread([this, origin = WTF::move(origin), caches = crossThreadCopy(WTF::move(caches))]() mutable {
                auto* state = m_origins.get(origin);
                if (!state)
                    return;
                for (auto& loaded : caches) {
                    Ref cache = Cache::create();
                    cache->origin = origin;
                    cache->name = WTF::move(loaded.entry.name);
                    cache->directoryName = WTF::move(loaded.entry.directoryName);
                    cache->directory = FileSystem::pathByAppendingComponent(state->directory, cache->directoryName);
                    cache->records = WTF::move(loaded.records);
                    for (auto& record : cache->records)
                        record.identifier = nextRecordIdentifier();
                    cache->nextFile = loaded.nextFile;
                    m_caches.add(cache->identifier, cache);
                    state->caches.append(WTF::move(cache));
                }
                state->loaded = true;
                auto waiting = std::exchange(state->waiting, { });
                for (auto& function : waiting)
                    function();
            });
        });
    }

    RecordInfo* findExisting(Cache& cache, const ResourceRequest& request)
    {
        CacheQueryOptions options;
        for (auto& record : cache.records) {
            if (queryCacheMatch(request, record.url, record.hasVaryStar, record.varyHeaders, options))
                return &record;
        }
        return nullptr;
    }

    void saveIndex(const ClientOrigin& origin, const OriginState& state)
    {
        auto entries = WTF::map(state.caches, [](auto& cache) {
            return IndexEntry { cache->name.isolatedCopy(), cache->directoryName.isolatedCopy() };
        });
        m_queue->dispatch([directory = state.directory.isolatedCopy(), origin = origin.isolatedCopy(), entries = WTF::move(entries)] {
            writeIndex(directory, origin, entries);
        });
    }

    void destroy(Cache& cache)
    {
        m_caches.remove(cache.identifier);
        m_queue->dispatch([directory = cache.directory.isolatedCopy()] {
            FileSystem::deleteNonEmptyDirectory(directory);
        });
    }

    const Ref<WorkQueue> m_queue;
    String m_root;
    HashMap<ClientOrigin, std::unique_ptr<OriginState>> m_origins;
    HashMap<DOMCacheIdentifier, Ref<Cache>> m_caches;
};

// Pages call it on the Web thread; a worker's calls arrive there through WebCore's WorkerCacheStorageConnection.
class Connection final : public CacheStorageConnection {
public:
    static Ref<Connection> create() { return adoptRef(*new Connection); }

private:
    Ref<OpenPromise> open(const ClientOrigin& origin, const String& name) final { return Engine::singleton().open(origin, name); }
    Ref<RemovePromise> remove(DOMCacheIdentifier identifier) final { return Engine::singleton().remove(identifier); }
    Ref<RetrieveCachesPromise> retrieveCaches(const ClientOrigin& origin, uint64_t updateCounter) final { return Engine::singleton().retrieveCaches(origin, updateCounter); }
    Ref<RetrieveRecordsPromise> retrieveRecords(DOMCacheIdentifier identifier, RetrieveRecordsOptions&& options) final { return Engine::singleton().retrieveRecords(identifier, WTF::move(options)); }
    Ref<BatchPromise> batchDeleteOperation(DOMCacheIdentifier identifier, const ResourceRequest& request, CacheQueryOptions&& options) final { return Engine::singleton().batchDelete(identifier, request, WTF::move(options)); }
    Ref<BatchPromise> batchPutOperation(DOMCacheIdentifier identifier, Vector<CrossThreadRecord>&& records) final { return Engine::singleton().batchPut(identifier, WTF::move(records)); }
    void reference(DOMCacheIdentifier identifier) final { Engine::singleton().reference(identifier); }
    void dereference(DOMCacheIdentifier identifier) final { Engine::singleton().dereference(identifier); }
    void lockStorage(const ClientOrigin&) final { }
    void unlockStorage(const ClientOrigin&) final { }
};

class Provider final : public CacheStorageProvider {
public:
    static Ref<Provider> create() { return adoptRef(*new Provider); }

private:
    Ref<CacheStorageConnection> createCacheStorageConnection() final
    {
        if (!m_connection)
            m_connection = Connection::create();
        return *m_connection;
    }

    RefPtr<Connection> m_connection;
};

} // namespace

CacheStorageProvider& cacheStorageProvider()
{
    static NeverDestroyed<Ref<Provider>> provider = Provider::create();
    return provider.get();
}

void clearCacheStorage(const SecurityOriginData* origin, CompletionHandler<void()>&& completionHandler)
{
    Engine::singleton().clear(origin, WTF::move(completionHandler));
}

} // namespace Lumia
