// HTTP disk cache (4d). WebKit2's network cache storage, compiled from WebKit's tree, keeps the records (one file per
// entry, bodies above 16 KB as blob files shared through hard links) in cache_directory\WebKitCache, within a capacity
// it shrinks to by itself. This file is the part WebKit2 keeps in NetworkCache.cpp: what may be kept, and whether an
// entry is fresh, stale (revalidated with a conditional request) or useless, by WebCore's HTTP rules
// (CacheValidation). Curl's loads ask it before going to the network (ResourceHandleDiskCacheWP81.h). Web thread.
#include "config.h"
#include "DiskCache.h"

#include "Engine.h"
#include "NetworkCacheCoders.h"
#include "NetworkCacheKey.h"
#include "NetworkCacheLogWP81.h"
#include "NetworkCacheStorage.h"
#include "ResourceHandleDiskCacheWP81.h"
#include "WebPage.h"
#include <WebCore/CacheValidation.h>
#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/HTTPStatusCodes.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/persistence/PersistentCoders.h>
#include <wtf/persistence/PersistentDecoder.h>
#include <wtf/persistence/PersistentEncoder.h>
#include <memory>
#include <wtf/text/MakeString.h>

// WebKit2's own log channels for the storage (NetworkCacheLogWP81.h), off. Spelled out: LOG_CHANNEL_PREFIX is
// WebCore's "Log" only where WebCore's Logging.h is included.
WTFLogChannel LogNetworkCache = { logChannelStateOff, "NetworkCache", logLevelError };
WTFLogChannel LogNetworkCacheStorage = { logChannelStateOff, "NetworkCacheStorage", logLevelError };
WTFLogChannel LogNetworkCacheSpeculativePreloading = { logChannelStateOff, "NetworkCacheSpeculativePreloading", logLevelError };

// The record key's coder, as NetworkCacheCoders.cpp has it (that file also brings the speculative loader's types).
namespace WTF::Persistence {

void Coder<WebKit::NetworkCache::Key>::encodeForPersistence(Encoder& encoder, const WebKit::NetworkCache::Key& instance)
{
    encoder << instance.partition();
    encoder << instance.type();
    encoder << instance.identifier();
    encoder << instance.range();
    encoder << instance.hash();
    encoder << instance.partitionHash();
}

std::optional<WebKit::NetworkCache::Key> Coder<WebKit::NetworkCache::Key>::decodeForPersistence(Decoder& decoder)
{
    WebKit::NetworkCache::Key key;
    std::optional<String> partition;
    decoder >> partition;
    std::optional<String> type;
    decoder >> type;
    std::optional<String> identifier;
    decoder >> identifier;
    std::optional<String> range;
    decoder >> range;
    std::optional<WebKit::NetworkCache::Key::HashType> hash;
    decoder >> hash;
    std::optional<WebKit::NetworkCache::Key::HashType> partitionHash;
    decoder >> partitionHash;
    if (!partition || !type || !identifier || !range || !hash || !partitionHash)
        return std::nullopt;
    key.m_partition = WTF::move(*partition);
    key.m_type = WTF::move(*type);
    key.m_identifier = WTF::move(*identifier);
    key.m_range = WTF::move(*range);
    key.m_hash = WTF::move(*hash);
    key.m_partitionHash = WTF::move(*partitionHash);
    return { WTF::move(key) };
}

} // namespace WTF::Persistence

namespace Lumia::DiskCache {
using namespace WebCore;
using WebKit::NetworkCache::Data;
using WebKit::NetworkCache::Key;
using WebKit::NetworkCache::Storage;

namespace {

// The phone's flash is 32 GB, its data plan small: enough for the scripts, styles and images of the sites used every
// day, which is where the second visit saves its time and bytes.
constexpr size_t capacity = 100 * 1024 * 1024;

const String& resourceType()
{
    static NeverDestroyed<String> type { "Resource"_s };
    return type;
}

// As NetworkCache's own entries (NetworkCacheEntry.cpp): the response and the request headers it varies on.
struct Entry {
    ResourceResponse response;
    Vector<std::pair<String, String>> varyingRequestHeaders;
};

Data encodeHeader(const Entry& entry)
{
    WTF::Persistence::Encoder encoder;
    encoder << entry.response;
    encoder << entry.varyingRequestHeaders;
    encoder.encodeChecksum();
    return Data { encoder.span() };
}

std::optional<Entry> decodeHeader(const Data& header)
{
    WTF::Persistence::Decoder decoder(header.span());
    std::optional<ResourceResponse> response;
    decoder >> response;
    std::optional<Vector<std::pair<String, String>>> varyingRequestHeaders;
    decoder >> varyingRequestHeaders;
    if (!response || !varyingRequestHeaders || !decoder.verifyChecksum())
        return std::nullopt;
    return Entry { WTF::move(*response), WTF::move(*varyingRequestHeaders) };
}

bool cachePolicyAllowsExpired(ResourceRequestCachePolicy policy)
{
    // Back and forward use what is there, without asking the server, as browsers do.
    return policy == ResourceRequestCachePolicy::ReturnCacheDataElseLoad || policy == ResourceRequestCachePolicy::ReturnCacheDataDontLoad;
}

// NetworkCache's responseNeedsRevalidation, without stale-while-revalidate (no background revalidation here).
bool needsRevalidation(const ResourceResponse& response, const ResourceRequest& request, WallTime timeStamp)
{
    auto requestDirectives = parseCacheControlDirectives(request.httpHeaderFields());
    if (requestDirectives.noCache || requestDirectives.noStore || response.cacheControlContainsNoCache())
        return true;
    auto age = computeCurrentAge(response, timeStamp);
    auto lifetime = computeFreshnessLifetimeForHTTPFamily(response, timeStamp);
    if (requestDirectives.maxAge && (requestDirectives.maxAge.value() == 0_ms || age > requestDirectives.maxAge.value()))
        return true;
    if (requestDirectives.minFresh && age + requestDirectives.minFresh.value() > lifetime)
        return true;
    auto maxStale = requestDirectives.maxStale ? requestDirectives.maxStale.value() : 0_ms;
    return age - lifetime > maxStale;
}

class Cache final : public ResourceHandleDiskCache {
public:
    explicit Cache(Ref<Storage>&& storage)
        : m_storage(WTF::move(storage))
    {
    }

    Storage& storage() { return m_storage.get(); }

    Key makeKey(const ResourceRequest& request) const
    {
        return { request.cachePartition(), resourceType(), { }, request.url().stringWithoutFragmentIdentifier(), m_storage->salt() };
    }

private:
    bool canRetrieve(const ResourceRequest& request) final
    {
        if (!request.url().protocolIsInHTTPFamily() || request.httpMethod() != "GET"_s || request.isConditional())
            return false;
        // A range of a media file is never kept, so never looked for.
        if (!request.httpHeaderField(HTTPHeaderName::Range).isEmpty())
            return false;
        auto policy = request.cachePolicy();
        return policy != ResourceRequestCachePolicy::ReloadIgnoringCacheData && policy != ResourceRequestCachePolicy::DoNotUseAnyCache;
    }

    void retrieve(const ResourceRequest& request, CompletionHandler<void(std::unique_ptr<ResourceHandleDiskCacheEntry>&&)>&& completionHandler) final
    {
        m_storage->retrieve(makeKey(request), 0, [this, request = ResourceRequest { request }, completionHandler = WTF::move(completionHandler)](Storage::Record&& record, const Storage::Timings&) mutable {
            auto entry = usableEntry(request, record);
            if (!entry) {
                completionHandler(nullptr);
                return false;
            }
            // A hit never completes inside the load's own start (the loader has no handle yet then).
            RunLoop::mainSingleton().dispatch([entry = WTF::move(entry), completionHandler = WTF::move(completionHandler)] mutable {
                completionHandler(WTF::move(entry));
            });
            return true;
        });
    }

    bool canStore(const ResourceRequest& request, const ResourceResponse& response) final
    {
        // NetworkCache's makeStoreDecision.
        if (!request.url().protocolIsInHTTPFamily() || !response.isInHTTPFamily() || request.httpMethod() != "GET"_s)
            return false;
        if (!request.httpHeaderField(HTTPHeaderName::Range).isEmpty() || response.httpStatusCode() == httpStatus304NotModified)
            return false;
        if (parseCacheControlDirectives(request.httpHeaderFields()).noStore || response.cacheControlContainsNoStore())
            return false;
        if (!isStatusCodeCacheableByDefault(response.httpStatusCode()) && !response.expires() && !response.cacheControlMaxAge()
            && !response.cacheControlContainsPublic())
            return false;
        if ((response.isRedirection() || response.isRedirected()) && request.url().hasFragmentIdentifier())
            return false;
        // The main document is kept even when it cannot be reused without asking (Back shows it as it was); the rest
        // only when it can be (a lifetime, or validators for a 304).
        if (request.requester() != ResourceRequestRequester::Main) {
            auto now = WallTime::now();
            bool hasLifetime = !response.cacheControlContainsNoCache() && computeFreshnessLifetimeForHTTPFamily(response, now) > 0_ms;
            if (!hasLifetime && !response.hasCacheValidatorFields())
                return false;
        }
        // Streaming media fills the cache and is not seen again.
        if (request.requester() == ResourceRequestRequester::Media)
            return false;
        auto mimeType = response.mimeType();
        if (request.requester() == ResourceRequestRequester::XHR
            && (startsWithLettersIgnoringASCIICase(mimeType, "video/"_s) || startsWithLettersIgnoringASCIICase(mimeType, "audio/"_s)))
            return false;
        return true;
    }

    void store(const ResourceRequest& request, const ResourceResponse& response, Ref<FragmentedSharedBuffer>&& body) final
    {
        Entry entry { response, collectVaryingRequestHeaders(&WebKit::WebPage::storageSession(), request, response) };
        // The connection's timings are of that load only.
        entry.response.setDeprecatedNetworkLoadMetrics({ });
        Ref contiguous = body->makeContiguous();
        Storage::Record record { makeKey(request), WallTime::now(), encodeHeader(entry), Data { contiguous->span() }, std::nullopt };
        m_storage->store(record, { });
    }

    // NetworkCache's makeUseDecision: fresh, or stale with validators, or nothing.
    std::unique_ptr<ResourceHandleDiskCacheEntry> usableEntry(const ResourceRequest& request, const Storage::Record& record)
    {
        if (record.isNull())
            return nullptr;
        auto entry = decodeHeader(record.header);
        if (!entry) {
            m_storage->remove(record.key);
            return nullptr;
        }
        if (!verifyVaryingRequestHeaders(&WebKit::WebPage::storageSession(), entry->varyingRequestHeaders, request))
            return nullptr;
        bool stale = !cachePolicyAllowsExpired(request.cachePolicy()) && needsRevalidation(entry->response, request, record.timeStamp);
        if (stale && !entry->response.hasCacheValidatorFields())
            return nullptr;
        entry->response.setSource(ResourceResponse::Source::DiskCache);
        auto body = SharedBuffer::create(record.body.span());
        return std::unique_ptr<ResourceHandleDiskCacheEntry>(new ResourceHandleDiskCacheEntry { WTF::move(entry->response), WTF::move(body), stale });
    }

    Ref<Storage> m_storage;
};

Cache*& sharedCache()
{
    static Cache* cache;
    return cache;
}

} // namespace

void initialize()
{
    auto* cacheDirectory = config().cache_directory;
    if (!cacheDirectory || sharedCache())
        return;
    auto path = FileSystem::pathByAppendingComponent(String(cacheDirectory), "WebKitCache"_s);
    RefPtr storage = Storage::open(path, Storage::Mode::Normal, capacity, 0);
    if (!storage) {
        log("cache su disco: impossibile aprire %s", path.utf8().data());
        return;
    }
    sharedCache() = new Cache(storage.releaseNonNull());
    setResourceHandleDiskCache(sharedCache());
    log("cache su disco: %s, %zu MB al massimo", path.utf8().data(), capacity >> 20);
}

void removeSite(const String& domain)
{
    auto* cache = sharedCache();
    if (!cache || domain.isEmpty())
        return;
    auto keys = std::make_shared<Vector<Key>>();
    cache->storage().traverse(resourceType(), { }, [cache, keys, domain = domain.isolatedCopy()](const Storage::Record* record, const Storage::RecordInfo&) {
        if (!record) {
            log("cache su disco: %zu voci di %s tolte", keys->size(), domain.utf8().data());
            cache->storage().remove(WTF::move(*keys), [] { });
            return;
        }
        auto host = URL { record->key.identifier() }.host();
        if (host == domain || host.endsWith(makeString('.', domain)))
            keys->append(record->key);
    });
}

void clear()
{
    auto* cache = sharedCache();
    if (!cache)
        return;
    cache->storage().clear(String { resourceType() }, -WallTime::infinity(), [] {
        log("cache su disco: vuota");
    });
}

String describe()
{
    auto* cache = sharedCache();
    if (!cache)
        return "cache su disco: spenta"_s;
    return makeString("cache su disco: circa "_s, cache->storage().approximateSize() >> 20, " MB di "_s, cache->storage().capacity() >> 20);
}

} // namespace Lumia::DiskCache
