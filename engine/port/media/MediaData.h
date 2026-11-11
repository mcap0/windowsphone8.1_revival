// The bytes of a media resource for the pipeline's thread (PLAN §75): a cache of fixed blocks filled by HTTP range
// requests through WebCore's media resource loader (the page's cookies, CORS, CSP and referrer apply as for any load).
//
// The reader asks for any range and waits; the Web thread fetches what the reader waits for and a bounded stretch
// ahead of it, one request at a time, a megabyte or two per request: the read position decides what comes next, so a
// seek, or a file with its index at the end, restarts the fetch elsewhere, and a paused player stops downloading once
// the stretch ahead is full. Blocks behind the reader leave the cache when it is over its limit.

#pragma once

#include "Demux.h"
#include <wtf/CompletionHandler.h>
#include <wtf/Condition.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/RunLoop.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/URL.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class PlatformMediaResource;
class PlatformMediaResourceLoader;
class ResourceResponse;
class SharedBuffer;
}

namespace Lumia {

class MediaData final : public ThreadSafeRefCounted<MediaData, WTF::DestructionThread::Main>, public Media::ByteSource {
public:
    static Ref<MediaData> create(Ref<WebCore::PlatformMediaResourceLoader>&&, const URL&);
    ~MediaData();

    // Web thread.
    void start();
    void stop();
    // How far ahead of the reader the fetch goes on its own. 0: only what a read waits for (preload=metadata).
    void setReadAhead(uint64_t bytes);
    bool didPassAccessControlCheck() const { return m_passedAccessControl; }

    // Any thread.
    long long size() final;
    bool read(unsigned long long offset, void* out, size_t size) final;
    bool failed() const;
    String failure() const;
    int httpStatus() const;
    // True once since the last call when bytes arrived.
    bool takeProgress();
    // The cached stretch around the read position.
    void cachedRange(uint64_t& start, uint64_t& end) const;
    bool acceptsRanges() const;

    // Called on the reading thread when a read starts (true) and stops (false) waiting for the network.
    Function<void(bool)> onWaiting;

private:
    class Client;
    friend class Client;

    MediaData(Ref<WebCore::PlatformMediaResourceLoader>&&, const URL&);

    static constexpr size_t blockSize = 64 * 1024;
    static constexpr uint64_t cacheLimit = 24 * 1024 * 1024;
    static constexpr uint64_t firstRequest = 256 * 1024;
    static constexpr uint64_t requestSize = 2 * 1024 * 1024;

    struct Block {
        Vector<uint8_t> bytes;
        size_t filled { 0 };
        uint64_t lastUse { 0 };
    };

    // Web thread.
    void pump();
    void retryFired();
    void schedulePump() WTF_REQUIRES_LOCK(m_lock);
    void request(uint64_t from);
    void cancelRequest();
    void responseReceived(unsigned generation, const WebCore::ResourceResponse&);
    void dataReceived(unsigned generation, const WebCore::SharedBuffer&);
    void finished(unsigned generation, bool failed, const String& error);

    bool copyOut(uint64_t offset, uint8_t* out, size_t size, uint64_t& missing) WTF_REQUIRES_LOCK(m_lock);
    bool has(uint64_t offset) const WTF_REQUIRES_LOCK(m_lock);
    uint64_t firstMissingFrom(uint64_t offset, uint64_t limit) const WTF_REQUIRES_LOCK(m_lock);
    void evict() WTF_REQUIRES_LOCK(m_lock);
    void fail(const String&) WTF_REQUIRES_LOCK(m_lock);

    Ref<WebCore::PlatformMediaResourceLoader> m_loader;
    URL m_url;
    RefPtr<WebCore::PlatformMediaResource> m_resource; // Web thread
    unsigned m_generation { 0 };                       // Web thread
    unsigned m_retries { 0 };                          // Web thread
    RunLoop::Timer m_retryTimer;
    bool m_passedAccessControl { false };

    mutable Lock m_lock;
    Condition m_condition;
    HashMap<uint64_t, Block, WTF::IntHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>> m_blocks WTF_GUARDED_BY_LOCK(m_lock);
    uint64_t m_useCounter WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    long long m_total WTF_GUARDED_BY_LOCK(m_lock) { -1 };
    bool m_noRanges WTF_GUARDED_BY_LOCK(m_lock) { false };      // the server sent the whole body for a range
    bool m_started WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_stopped WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_failed WTF_GUARDED_BY_LOCK(m_lock) { false };
    String m_failure WTF_GUARDED_BY_LOCK(m_lock);
    int m_httpStatus WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_progress WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_pumpScheduled WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_retryPending WTF_GUARDED_BY_LOCK(m_lock) { false };  // the retry timer runs
    bool m_retryNow WTF_GUARDED_BY_LOCK(m_lock) { false };
    // The request in flight: [m_fetchStart, m_fetchEnd), next byte to arrive at m_fetchPosition.
    bool m_fetching WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_gotResponse WTF_GUARDED_BY_LOCK(m_lock) { false };
    uint64_t m_fetchStart WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    uint64_t m_fetchEnd WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    uint64_t m_fetchPosition WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    // The reader.
    uint64_t m_readPosition WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    uint64_t m_readAhead WTF_GUARDED_BY_LOCK(m_lock) { 0 };
    bool m_waiting WTF_GUARDED_BY_LOCK(m_lock) { false };
    uint64_t m_wantStart WTF_GUARDED_BY_LOCK(m_lock) { 0 };  // first byte the waiting read lacks
    uint64_t m_pinStart WTF_GUARDED_BY_LOCK(m_lock) { 0 };   // the waiting read's whole range: not evicted
    uint64_t m_pinEnd WTF_GUARDED_BY_LOCK(m_lock) { 0 };
};

} // namespace Lumia
