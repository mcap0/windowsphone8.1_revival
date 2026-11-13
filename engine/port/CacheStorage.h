// Cache storage (window.caches, and a service worker's): WebKit2 keeps it in the network process
// (NetworkProcess/storage/CacheStorage*). Here: the list of records in memory on the Web thread, their files read and
// written on a queue. data_directory\CacheStorage\<hash of the origins>\<cache>\<n>.rec (request + response) and
// <n>.body.
#pragma once

#include <wtf/CompletionHandler.h>
#include <wtf/Forward.h>

namespace WebCore {
class CacheStorageProvider;
class SecurityOriginData;
}

namespace Lumia {

// One for every page and worker.
WebCore::CacheStorageProvider& cacheStorageProvider();
// The caches of the pages of one site (as top or as embedded origin), or of all (nullptr).
void clearCacheStorage(const WebCore::SecurityOriginData*, CompletionHandler<void()>&&);

} // namespace Lumia
