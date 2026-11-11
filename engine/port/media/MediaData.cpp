#include "config.h"
#include "MediaData.h"

#include <WebCore/HTTPHeaderNames.h>
#include <WebCore/PlatformMediaResourceLoader.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <wtf/MainThread.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace Lumia {

using namespace WebCore;

// The loader's callbacks (Web thread). A request that was replaced keeps its client until it is shut down: the
// generation tells its late calls apart.
class MediaData::Client final : public PlatformMediaResourceClient {
public:
    Client(MediaData& data, unsigned generation)
        : m_data(data)
        , m_generation(generation)
    {
    }

private:
    void responseReceived(PlatformMediaResource&, const ResourceResponse& response, CompletionHandler<void(ShouldContinuePolicyCheck)>&& completionHandler) final
    {
        m_data->responseReceived(m_generation, response);
        completionHandler(ShouldContinuePolicyCheck::Yes);
    }
    bool shouldCacheResponse(PlatformMediaResource&, const ResourceResponse&) final { return false; }
    void dataReceived(PlatformMediaResource&, const SharedBuffer& buffer) final { m_data->dataReceived(m_generation, buffer); }
    void accessControlCheckFailed(PlatformMediaResource&, const ResourceError& error) final { m_data->finished(m_generation, true, error.localizedDescription()); }
    void loadFailed(PlatformMediaResource&, const ResourceError& error) final { m_data->finished(m_generation, true, error.localizedDescription()); }
    void loadFinished(PlatformMediaResource&, const NetworkLoadMetrics&) final { m_data->finished(m_generation, false, { }); }

    const Ref<MediaData> m_data;
    unsigned m_generation;
};

Ref<MediaData> MediaData::create(Ref<PlatformMediaResourceLoader>&& loader, const URL& url)
{
    return adoptRef(*new MediaData(WTF::move(loader), url));
}

MediaData::MediaData(Ref<PlatformMediaResourceLoader>&& loader, const URL& url)
    : m_loader(WTF::move(loader))
    , m_url(url)
    , m_retryTimer(RunLoop::mainSingleton(), "MediaData::RetryTimer"_s, [this] { retryFired(); })
{
}

MediaData::~MediaData()
{
    ASSERT(isMainThread());
    cancelRequest();
}

void MediaData::start()
{
    ASSERT(isMainThread());
    {
        Locker locker { m_lock };
        if (m_started)
            return;
        m_started = true;
    }
    pump();
}

void MediaData::stop()
{
    ASSERT(isMainThread());
    {
        Locker locker { m_lock };
        m_stopped = true;
        m_fetching = false;
        m_blocks.clear();
        m_condition.notifyAll();
    }
    m_retryTimer.stop();
    cancelRequest();
}

void MediaData::setReadAhead(uint64_t bytes)
{
    Locker locker { m_lock };
    if (m_readAhead == bytes)
        return;
    m_readAhead = bytes;
    schedulePump();
}

long long MediaData::size()
{
    Locker locker { m_lock };
    return m_total;
}

bool MediaData::failed() const
{
    Locker locker { m_lock };
    return m_failed;
}

String MediaData::failure() const
{
    Locker locker { m_lock };
    return m_failure.isolatedCopy();
}

int MediaData::httpStatus() const
{
    Locker locker { m_lock };
    return m_httpStatus;
}

bool MediaData::acceptsRanges() const
{
    Locker locker { m_lock };
    return !m_noRanges;
}

bool MediaData::takeProgress()
{
    Locker locker { m_lock };
    return std::exchange(m_progress, false);
}

void MediaData::fail(const String& why)
{
    if (m_failed)
        return;
    m_failed = true;
    m_failure = why.isolatedCopy();
    m_condition.notifyAll();
}

bool MediaData::has(uint64_t offset) const
{
    auto it = m_blocks.find(offset / blockSize);
    return it != m_blocks.end() && it->value.filled > offset % blockSize;
}

// The first block boundary at or after `offset` (a boundary itself) whose block is not whole; `limit` when all are.
uint64_t MediaData::firstMissingFrom(uint64_t offset, uint64_t limit) const
{
    if (m_total >= 0)
        limit = std::min<uint64_t>(limit, m_total);
    for (uint64_t at = offset; at < limit; at += blockSize) {
        auto it = m_blocks.find(at / blockSize);
        if (it == m_blocks.end())
            return at;
        size_t filled = it->value.filled;
        if (filled < blockSize && !(m_total >= 0 && at + filled >= static_cast<uint64_t>(m_total)))
            return at;
    }
    return limit;
}

bool MediaData::copyOut(uint64_t offset, uint8_t* out, size_t size, uint64_t& missing)
{
    // Checked before anything is copied: a read is whole or not at all.
    for (uint64_t at = offset; at < offset + size;) {
        size_t inBlock = static_cast<size_t>(at % blockSize);
        size_t want = std::min<uint64_t>(blockSize - inBlock, offset + size - at);
        auto it = m_blocks.find(at / blockSize);
        if (it == m_blocks.end()) {
            missing = at;
            return false;
        }
        if (it->value.filled < inBlock + want) {
            missing = std::max<uint64_t>(at, at - inBlock + it->value.filled);
            return false;
        }
        at += want;
    }
    ++m_useCounter;
    for (uint64_t at = offset; at < offset + size;) {
        size_t inBlock = static_cast<size_t>(at % blockSize);
        size_t want = std::min<uint64_t>(blockSize - inBlock, offset + size - at);
        auto it = m_blocks.find(at / blockSize);
        memcpy(out + (at - offset), it->value.bytes.span().data() + inBlock, want);
        it->value.lastUse = m_useCounter;
        at += want;
    }
    return true;
}

void MediaData::evict()
{
    uint64_t held = static_cast<uint64_t>(m_blocks.size()) * blockSize;
    if (m_noRanges) {
        // What left the cache could not be asked for again.
        if (held > 4 * cacheLimit)
            fail("il server non accetta richieste a intervalli e la risorsa supera 96 MB"_s);
        return;
    }
    if (held <= cacheLimit)
        return;
    // Down to a megabyte under the limit, least recently read first; never what the reader is about to read or
    // waits for, nor what just arrived.
    uint64_t keepFrom = m_readPosition > blockSize ? m_readPosition - blockSize : 0;
    uint64_t keepTo = m_readPosition + std::max<uint64_t>(m_readAhead, requestSize) + requestSize;
    Vector<std::pair<uint64_t, uint64_t>> candidates;
    for (auto& entry : m_blocks) {
        uint64_t start = entry.key * blockSize;
        if (start + blockSize > keepFrom && start < keepTo)
            continue;
        if (m_pinEnd && start + blockSize > m_pinStart && start < m_pinEnd)
            continue;
        if (m_fetching && start + blockSize > m_fetchStart && start <= m_fetchPosition)
            continue;
        candidates.append({ entry.value.lastUse, entry.key });
    }
    std::sort(candidates.begin(), candidates.end());
    for (auto& candidate : candidates) {
        if (held + 1024 * 1024 <= cacheLimit)
            break;
        m_blocks.remove(candidate.second);
        held -= blockSize;
    }
}

bool MediaData::read(unsigned long long offset, void* out, size_t size)
{
    if (!size)
        return true;
    bool result = false;
    bool reported = false;
    bool waited = false;
    {
        Locker locker { m_lock };
        m_readPosition = offset;
        for (;;) {
            if (m_stopped || m_failed)
                break;
            if (m_total >= 0 && offset + size > static_cast<uint64_t>(m_total))
                break;
            if (size > cacheLimit * 2 / 3)
                break;
            uint64_t missing = 0;
            if (copyOut(offset, static_cast<uint8_t*>(out), size, missing)) {
                result = true;
                break;
            }
            if (m_noRanges && !m_fetching && !m_retryPending && m_gotResponse)
                break; // the body ended before these bytes
            m_waiting = true;
            m_wantStart = missing;
            m_pinStart = offset;
            m_pinEnd = offset + size;
            schedulePump();
            // A wait the request in flight ends in a moment is not worth telling.
            if (waited && !reported && onWaiting) {
                reported = true;
                DropLockForScope unlocked { locker };
                onWaiting(true);
                continue;
            }
            m_condition.waitFor(m_lock, waited ? 2_s : 300_ms);
            waited = true;
        }
        m_waiting = false;
        m_pinEnd = 0;
        if (result) {
            m_readPosition = offset + size;
            if (!m_fetching && m_readAhead && m_readPosition + m_readAhead / 2 > m_fetchPosition
                && (m_total < 0 || m_fetchPosition < static_cast<uint64_t>(m_total)))
                schedulePump();
        }
    }
    if (reported)
        onWaiting(false);
    return result;
}

void MediaData::cachedRange(uint64_t& start, uint64_t& end) const
{
    Locker locker { m_lock };
    uint64_t at = m_readPosition - m_readPosition % blockSize;
    start = end = m_readPosition;
    if (!has(m_readPosition))
        return;
    start = at;
    while (start >= blockSize) {
        auto it = m_blocks.find(start / blockSize - 1);
        if (it == m_blocks.end() || it->value.filled < blockSize)
            break;
        start -= blockSize;
    }
    for (;;) {
        auto it = m_blocks.find(at / blockSize);
        if (it == m_blocks.end())
            break;
        end = at + it->value.filled;
        if (it->value.filled < blockSize)
            break;
        at += blockSize;
    }
}

void MediaData::retryFired()
{
    {
        Locker locker { m_lock };
        m_retryPending = false;
        m_retryNow = true;
    }
    pump();
}

void MediaData::schedulePump()
{
    if (m_pumpScheduled || m_stopped)
        return;
    m_pumpScheduled = true;
    callOnMainThread([protectedThis = Ref { *this }] {
        protectedThis->pump();
    });
}

void MediaData::pump()
{
    ASSERT(isMainThread());
    uint64_t from = 0;
    bool cancel = false;
    {
        Locker locker { m_lock };
        m_pumpScheduled = false;
        if (m_stopped || m_failed || !m_started || m_retryPending)
            return;
        bool retry = std::exchange(m_retryNow, false);
        if (m_noRanges) {
            // One body from its first byte: asked again only after a failure.
            if (m_fetching || !retry)
                return;
            from = 0;
        } else if (m_waiting) {
            // What a read waits for decides. It is left to the request in flight when that one is about to bring it.
            if (m_fetching) {
                if (!m_gotResponse)
                    return;
                if (m_wantStart >= m_fetchPosition && m_wantStart < m_fetchEnd + 4 * blockSize)
                    return;
                cancel = true;
                m_fetching = false;
            }
            from = m_wantStart - m_wantStart % blockSize;
        } else {
            if (m_fetching)
                return;
            // The stretch ahead of the reader (the first request, when nothing was read yet).
            uint64_t ahead = m_gotResponse ? m_readAhead : std::max<uint64_t>(m_readAhead, 1);
            uint64_t limit = m_readPosition + ahead;
            from = firstMissingFrom(m_readPosition - m_readPosition % blockSize, limit);
            if (from >= limit || (m_total >= 0 && from >= static_cast<uint64_t>(m_total))) {
                m_fetchPosition = std::max(m_fetchPosition, limit);
                return;
            }
        }
    }
    if (cancel)
        cancelRequest();
    request(from);
}

void MediaData::cancelRequest()
{
    ASSERT(isMainThread());
    ++m_generation;
    if (auto resource = std::exchange(m_resource, nullptr)) {
        resource->setClient(nullptr);
        resource->shutdown();
    }
}

void MediaData::request(uint64_t from)
{
    ASSERT(isMainThread());
    cancelRequest();
    uint64_t end = 0;
    {
        Locker locker { m_lock };
        end = from + (m_gotResponse ? requestSize : firstRequest);
        // Not over what the cache already holds.
        for (uint64_t at = from + blockSize; at < end; at += blockSize) {
            auto it = m_blocks.find(at / blockSize);
            if (it != m_blocks.end() && it->value.filled == blockSize) {
                end = at;
                break;
            }
        }
        if (m_total >= 0)
            end = std::min<uint64_t>(end, m_total);
        if (end <= from && !m_noRanges) {
            m_condition.notifyAll();
            return;
        }
        m_fetching = true;
        m_fetchStart = m_fetchPosition = from;
        m_fetchEnd = end;
    }
    ResourceRequest request { URL { m_url } };
    request.setHTTPHeaderField(HTTPHeaderName::Range, makeString("bytes="_s, from, '-', end - 1));
    // A range of a compressed body would be a range of the compressed bytes.
    request.setHTTPHeaderField(HTTPHeaderName::AcceptEncoding, "identity"_s);
    unsigned generation = m_generation;
    m_resource = m_loader->requestResource(WTF::move(request), PlatformMediaResourceLoader::LoadOption::DisallowCaching);
    if (!m_resource) {
        Locker locker { m_lock };
        m_fetching = false;
        fail("richiesta rifiutata"_s);
        return;
    }
    m_resource->setClient(adoptRef(*new Client(*this, generation)));
}

void MediaData::responseReceived(unsigned generation, const ResourceResponse& response)
{
    ASSERT(isMainThread());
    if (generation != m_generation)
        return;
    if (m_resource)
        m_passedAccessControl = m_resource->didPassAccessControlCheck();
    int status = response.httpStatusCode();
    Locker locker { m_lock };
    m_httpStatus = status;
    m_gotResponse = true;
    if (status == 206) {
        // "bytes first-last/total"
        auto range = response.httpHeaderField(HTTPHeaderName::ContentRange);
        size_t space = range.find(' ');
        size_t dash = range.find('-');
        size_t slash = range.find('/');
        if (space == notFound || dash == notFound || slash == notFound || dash < space || slash < dash) {
            fail("Content-Range illeggibile"_s);
            return;
        }
        auto first = parseInteger<uint64_t>(range.substring(space + 1, dash - space - 1).trim(isASCIIWhitespace));
        auto last = parseInteger<uint64_t>(range.substring(dash + 1, slash - dash - 1).trim(isASCIIWhitespace));
        auto total = parseInteger<uint64_t>(range.substring(slash + 1).trim(isASCIIWhitespace));
        if (!first || !last || *first != m_fetchStart || *last < *first) {
            fail("Content-Range diverso dalla richiesta"_s);
            return;
        }
        m_fetchEnd = *last + 1;
        if (total)
            m_total = static_cast<long long>(*total);
    } else if (status == 416) {
        // Past the end.
        if (m_total < 0 || static_cast<uint64_t>(m_total) > m_fetchStart)
            m_total = static_cast<long long>(m_fetchStart);
    } else if (status == 200 || !status) {
        // The whole body, from its first byte (a server without ranges, or a file: or data: address).
        m_noRanges = true;
        m_fetchStart = m_fetchPosition = 0;
        long long length = response.expectedContentLength();
        m_total = length > 0 ? length : -1;
        m_fetchEnd = m_total >= 0 ? static_cast<uint64_t>(m_total) : std::numeric_limits<uint64_t>::max();
    } else
        fail(makeString("HTTP "_s, status));
    m_condition.notifyAll();
    // A read that moved elsewhere while this answer was on its way.
    if (m_waiting)
        schedulePump();
}

void MediaData::dataReceived(unsigned generation, const SharedBuffer& buffer)
{
    ASSERT(isMainThread());
    if (generation != m_generation)
        return;
    m_retries = 0;
    auto bytes = buffer.span();
    Locker locker { m_lock };
    if (!m_fetching || m_failed || m_stopped || m_httpStatus == 416)
        return;
    size_t done = 0;
    while (done < bytes.size()) {
        uint64_t at = m_fetchPosition;
        size_t inBlock = static_cast<size_t>(at % blockSize);
        size_t count = std::min(blockSize - inBlock, bytes.size() - done);
        auto& block = m_blocks.ensure(at / blockSize, [] { return Block { }; }).iterator->value;
        if (block.bytes.isEmpty())
            block.bytes.grow(blockSize);
        // Blocks fill from their start: bytes that would leave a hole (a request cut short earlier) wait for the
        // request that brings the block's beginning.
        if (inBlock <= block.filled) {
            memcpy(block.bytes.mutableSpan().data() + inBlock, bytes.data() + done, count);
            block.filled = std::max(block.filled, inBlock + count);
            block.lastUse = m_useCounter;
        }
        m_fetchPosition += count;
        done += count;
    }
    m_progress = true;
    evict();
    m_condition.notifyAll();
}

void MediaData::finished(unsigned generation, bool failed, const String& error)
{
    ASSERT(isMainThread());
    if (generation != m_generation)
        return;
    {
        Locker locker { m_lock };
        m_fetching = false;
        if (failed) {
            if (m_stopped)
                return;
            // A dropped connection is asked again from where the reader is, a few times.
            if (m_retries >= 4 || (m_gotResponse && m_httpStatus >= 400)) {
                fail(error.isEmpty() ? "caricamento interrotto"_s : error);
                return;
            }
            ++m_retries;
            m_retryPending = true;
            m_retryTimer.startOneShot(Seconds { 0.5 * m_retries });
            return;
        }
        if (m_noRanges && m_total < 0)
            m_total = static_cast<long long>(m_fetchPosition);
        m_condition.notifyAll();
    }
    pump();
}

} // namespace Lumia
