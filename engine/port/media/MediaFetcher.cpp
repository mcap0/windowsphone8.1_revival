#include "config.h"
#include "MediaFetcher.h"

#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/PlatformMediaResourceLoader.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <wtf/MainThread.h>
#include <wtf/URL.h>
#include <wtf/text/MakeString.h>

namespace Lumia {

using namespace WebCore;

namespace {
constexpr size_t maximumResource = 96 * 1024 * 1024;
}

class MediaFetcher::Client final : public PlatformMediaResourceClient {
public:
    Client(MediaFetcher& fetcher, Load& load)
        : m_fetcher(fetcher)
        , m_load(load)
    {
    }

private:
    void responseReceived(PlatformMediaResource&, const ResourceResponse& response, CompletionHandler<void(ShouldContinuePolicyCheck)>&& completionHandler) final
    {
        int status = response.httpStatusCode();
        if (status >= 400) {
            m_fetcher->finished(m_load, true, makeString("HTTP "_s, status));
            completionHandler(ShouldContinuePolicyCheck::No);
            return;
        }
        {
            Locker locker { m_fetcher->m_lock };
            m_load->finalURL = response.url().string().isolatedCopy();
            // A server that answers a range with the whole body: the range is cut out when the bytes are handed over.
            m_whole = m_load->length && status != 206;
            m_load->bytes.clear();
        }
        completionHandler(ShouldContinuePolicyCheck::Yes);
    }
    bool shouldCacheResponse(PlatformMediaResource&, const ResourceResponse&) final { return false; }
    void dataReceived(PlatformMediaResource&, const SharedBuffer& buffer) final
    {
        bool tooLarge = false;
        {
            Locker locker { m_fetcher->m_lock };
            m_load->bytes.append(buffer.span());
            m_fetcher->m_progress = true;
            tooLarge = m_load->bytes.size() > maximumResource;
        }
        if (tooLarge)
            m_fetcher->finished(m_load, true, "risorsa oltre 96 MB"_s);
    }
    void accessControlCheckFailed(PlatformMediaResource&, const ResourceError& error) final { m_fetcher->finished(m_load, true, error.localizedDescription()); }
    void loadFailed(PlatformMediaResource&, const ResourceError& error) final { m_fetcher->finished(m_load, true, error.localizedDescription()); }
    void loadFinished(PlatformMediaResource&, const NetworkLoadMetrics&) final
    {
        if (m_whole) {
            Locker locker { m_fetcher->m_lock };
            auto& bytes = m_load->bytes;
            if (m_load->offset >= bytes.size())
                bytes.clear();
            else {
                bytes.removeAt(0, static_cast<size_t>(m_load->offset));
                if (bytes.size() > m_load->length)
                    bytes.shrink(static_cast<size_t>(m_load->length));
            }
        }
        m_fetcher->finished(m_load, false, { });
    }

    const Ref<MediaFetcher> m_fetcher;
    const Ref<Load> m_load;
    bool m_whole { false };
};

Ref<MediaFetcher> MediaFetcher::create(Ref<PlatformMediaResourceLoader>&& loader)
{
    return adoptRef(*new MediaFetcher(WTF::move(loader)));
}

MediaFetcher::MediaFetcher(Ref<PlatformMediaResourceLoader>&& loader)
    : m_loader(WTF::move(loader))
{
}

MediaFetcher::~MediaFetcher() = default;

String MediaFetcher::keyFor(const String& url, uint64_t offset, uint64_t length)
{
    return makeString(url, ' ', offset, '+', length);
}

String MediaFetcher::resolve(const std::string& base, const std::string& reference)
{
    URL baseURL { String::fromUTF8(base.c_str()) };
    if (reference.empty())
        return baseURL.string().isolatedCopy();
    return URL { baseURL, String::fromUTF8(reference.c_str()) }.string().isolatedCopy();
}

RefPtr<MediaFetcher::Load> MediaFetcher::ensure(const String& url, uint64_t offset, uint64_t length, bool prefetch)
{
    if (m_stopped || url.isEmpty())
        return nullptr;
    auto key = keyFor(url, offset, length);
    if (auto existing = m_loads.get(key))
        return existing;
    Ref load = adoptRef(*new Load);
    load->url = url;
    load->offset = offset;
    load->length = length;
    load->prefetched = prefetch;
    m_loads.add(key, load.copyRef());
    callOnMainThread([protectedThis = Ref { *this }, load] {
        protectedThis->begin(load);
    });
    return load;
}

void MediaFetcher::begin(Load& load)
{
    ASSERT(isMainThread());
    {
        Locker locker { m_lock };
        if (m_stopped || load.done)
            return;
    }
    ResourceRequest request { URL { load.url } };
    if (load.length) {
        request.setHTTPHeaderField(HTTPHeaderName::Range, makeString("bytes="_s, load.offset, '-', load.offset + load.length - 1));
        request.setHTTPHeaderField(HTTPHeaderName::AcceptEncoding, "identity"_s);
    }
    load.resource = m_loader->requestResource(WTF::move(request), PlatformMediaResourceLoader::LoadOption::DisallowCaching);
    if (!load.resource) {
        finished(load, true, "richiesta rifiutata"_s);
        return;
    }
    load.resource->setClient(adoptRef(*new Client(*this, load)));
}

void MediaFetcher::cancel(Load& load)
{
    ASSERT(isMainThread());
    if (auto resource = std::exchange(load.resource, nullptr)) {
        resource->setClient(nullptr);
        resource->shutdown();
    }
}

void MediaFetcher::finished(Load& load, bool failed, const String& why)
{
    ASSERT(isMainThread());
    {
        Locker locker { m_lock };
        if (load.done)
            return;
        load.done = true;
        load.failed = failed;
        if (failed) {
            load.bytes.clear();
            m_failure = makeString(why, " ("_s, load.url, ')').isolatedCopy();
        }
        m_condition.notifyAll();
    }
    cancel(load);
}

void MediaFetcher::stop()
{
    ASSERT(isMainThread());
    Vector<Ref<Load>> loads;
    {
        Locker locker { m_lock };
        m_stopped = true;
        for (auto& load : m_loads.values())
            loads.append(load);
        m_loads.clear();
        m_condition.notifyAll();
    }
    for (auto& load : loads)
        cancel(load);
}

bool MediaFetcher::fetch(const std::string& base, const std::string& reference, uint64_t offset, uint64_t length, std::vector<uint8_t>& out, std::string& resolved)
{
    String url = resolve(base, reference);
    // A connection that drops is tried again; a refusal (404, CORS) is tried once more and then believed.
    for (int attempt = 0; attempt < 3; ++attempt) {
        RefPtr<Load> load;
        bool waited = false;
        {
            Locker locker { m_lock };
            load = ensure(url, offset, length, false);
            if (!load)
                return false;
            load->prefetched = false;
            ++load->waiters;
            if (!load->done && onWaiting) {
                waited = true;
                DropLockForScope drop { locker };
                onWaiting(true);
            }
            while (!load->done && !m_stopped)
                m_condition.wait(m_lock);
            --load->waiters;
            bool stopped = m_stopped;
            bool failed = load->failed;
            if (!stopped) {
                m_loads.remove(keyFor(url, offset, length));
                if (!failed) {
                    out.assign(load->bytes.span().begin(), load->bytes.span().end());
                    load->bytes.clear();
                    resolved = load->finalURL.isEmpty() ? std::string(url.utf8().data()) : std::string(load->finalURL.utf8().data());
                }
            }
            if (waited) {
                DropLockForScope drop { locker };
                onWaiting(false);
            }
            if (stopped)
                return false;
            if (!failed)
                return true;
        }
        if (!wait(400 * (attempt + 1)))
            return false;
    }
    return false;
}

void MediaFetcher::prefetch(const std::string& base, const std::string& reference, uint64_t offset, uint64_t length)
{
    String url = resolve(base, reference);
    Vector<Ref<Load>> dropped;
    {
        Locker locker { m_lock };
        auto key = keyFor(url, offset, length);
        // One resource ahead: an earlier one nobody came for (a seek went elsewhere) is given up.
        m_loads.removeIf([&](auto& entry) {
            if (entry.key == key || !entry.value->prefetched || entry.value->waiters)
                return false;
            dropped.append(entry.value);
            return true;
        });
        ensure(url, offset, length, true);
    }
    if (!dropped.isEmpty()) {
        callOnMainThread([protectedThis = Ref { *this }, dropped = WTF::move(dropped)] {
            for (auto& load : dropped) {
                {
                    Locker locker { protectedThis->m_lock };
                    load->done = true;
                    load->failed = true;
                    load->bytes.clear();
                }
                protectedThis->cancel(load);
            }
        });
    }
}

bool MediaFetcher::wait(int milliseconds)
{
    Locker locker { m_lock };
    auto deadline = MonotonicTime::now() + Seconds::fromMilliseconds(milliseconds);
    while (!m_stopped && MonotonicTime::now() < deadline)
        m_condition.waitUntil(m_lock, deadline);
    return !m_stopped;
}

bool MediaFetcher::takeProgress()
{
    Locker locker { m_lock };
    return std::exchange(m_progress, false);
}

String MediaFetcher::failure() const
{
    Locker locker { m_lock };
    return m_failure.isolatedCopy();
}

} // namespace Lumia
