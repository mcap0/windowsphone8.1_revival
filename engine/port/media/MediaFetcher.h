// Whole resources for the pipeline's thread (PLAN §75.3 item 6, HLS): play lists, segments and keys, each fetched
// through WebCore's media resource loader on the Web thread (the page's cookies, CORS, CSP and referrer apply) while
// the reader waits. A resource asked for ahead of time (prefetch) is kept until it is asked for, or until another
// one is prefetched.

#pragma once

#include "Demux.h"
#include <wtf/Condition.h>
#include <wtf/Function.h>
#include <wtf/HashMap.h>
#include <wtf/Lock.h>
#include <wtf/ThreadSafeRefCounted.h>
#include <wtf/Vector.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace WebCore {
class PlatformMediaResource;
class PlatformMediaResourceLoader;
}

namespace Lumia {

class MediaFetcher final : public ThreadSafeRefCounted<MediaFetcher, WTF::DestructionThread::Main>, public Media::Fetcher {
public:
    static Ref<MediaFetcher> create(Ref<WebCore::PlatformMediaResourceLoader>&&);
    ~MediaFetcher();

    // Web thread.
    void stop();

    // The pipeline's thread.
    bool fetch(const std::string& base, const std::string& reference, uint64_t offset, uint64_t length, std::vector<uint8_t>& out, std::string& resolved) final;
    void prefetch(const std::string& base, const std::string& reference, uint64_t offset, uint64_t length) final;
    bool wait(int milliseconds) final;

    // Any thread.
    bool takeProgress();
    String failure() const;

    // Called on the reading thread when a fetch starts (true) and stops (false) waiting for the network.
    Function<void(bool)> onWaiting;

private:
    class Client;
    friend class Client;

    struct Load : public ThreadSafeRefCounted<Load, WTF::DestructionThread::Main> {
        String url;
        uint64_t offset { 0 }, length { 0 };
        // Under the fetcher's lock.
        Vector<uint8_t> bytes;
        String finalURL;
        bool done { false }, failed { false }, prefetched { false };
        unsigned waiters { 0 };
        // Web thread.
        RefPtr<WebCore::PlatformMediaResource> resource;
    };

    explicit MediaFetcher(Ref<WebCore::PlatformMediaResourceLoader>&&);
    static String keyFor(const String& url, uint64_t offset, uint64_t length);
    static String resolve(const std::string& base, const std::string& reference);
    RefPtr<Load> ensure(const String& url, uint64_t offset, uint64_t length, bool prefetch) WTF_REQUIRES_LOCK(m_lock);
    void begin(Load&);                 // Web thread
    void cancel(Load&);                // Web thread
    void finished(Load&, bool failed, const String& why);

    Ref<WebCore::PlatformMediaResourceLoader> m_loader;
    mutable Lock m_lock;
    Condition m_condition;
    HashMap<String, Ref<Load>> m_loads WTF_GUARDED_BY_LOCK(m_lock);
    bool m_stopped WTF_GUARDED_BY_LOCK(m_lock) { false };
    bool m_progress WTF_GUARDED_BY_LOCK(m_lock) { false };
    String m_failure WTF_GUARDED_BY_LOCK(m_lock);
};

} // namespace Lumia
