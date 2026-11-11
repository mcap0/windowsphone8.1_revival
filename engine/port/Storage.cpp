#include "config.h"
#include "Storage.h"

#include "DiskCache.h"
#include "Engine.h"
#include "StorageNamespaceImpl.h"
#include "StorageThread.h"
#include "StorageTracker.h"
#include "WebDatabaseProvider.h"
#include "WebPage.h"
#include "WebStorageNamespaceProvider.h"
#include <WebCore/CookieJar.h>
#include <WebCore/MemoryCache.h>
#include <WebCore/NetworkStorageSession.h>
#include <WebCore/RegistrableDomain.h>
#include <WebCore/SecurityOrigin.h>
#include <WebCore/SecurityOriginData.h>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>

using namespace WebCore;

// Declared by WebKitLegacy, defined by each platform (mac/Storage/WebDatabaseProvider.mm).
String WebDatabaseProvider::indexedDatabaseDirectoryPath()
{
    auto* dataDirectory = Lumia::config().data_directory;
    return dataDirectory ? FileSystem::pathByAppendingComponent(String(dataDirectory), "IndexedDB"_s) : String();
}

namespace Lumia {

namespace {

String localStorageDirectory()
{
    auto* dataDirectory = config().data_directory;
    return dataDirectory ? FileSystem::pathByAppendingComponent(String(dataDirectory), "LocalStorage"_s) : String();
}

} // namespace

void initializeStorage()
{
    // The tracker keeps the list of origins with a localStorage file (for clearing them); no client to notify.
    WebKit::StorageTracker::initializeTracker(localStorageDirectory(), nullptr);
}

StorageNamespaceProvider& storageNamespaceProvider()
{
    static NeverDestroyed<Ref<WebKit::WebStorageNamespaceProvider>> provider = WebKit::WebStorageNamespaceProvider::create(localStorageDirectory());
    return provider.get();
}

DatabaseProvider& databaseProvider()
{
    return WebDatabaseProvider::singleton();
}

void flushStorage(Function<void()>&& written)
{
    WebKit::WebStorageNamespaceProvider::syncLocalStorage();
    // Each storage thread runs its queue in order: this comes after the writes just queued.
    StorageThread::dispatchAfterQueuedWorkInAllThreads(WTF::move(written));
}

void clearSiteData(const SecurityOriginData& origin)
{
    // Cookies: every domain of the same site (example.com, .example.com, www.example.com, login.example.com), so a
    // login made on a subdomain goes too.
    auto site = RegistrableDomain::uncheckedCreateFromHost(origin.host()).string();
    auto& session = WebKit::WebPage::storageSession();
    HashSet<String> hostnames;
    session.getHostnamesWithCookies(hostnames);
    Vector<String> matching;
    for (auto& hostname : hostnames) {
        StringView domain = hostname;
        if (domain.startsWith('.'))
            domain = domain.substring(1);
        if (domain == site || domain.endsWith(makeString('.', site)))
            matching.append(hostname);
    }
    session.deleteCookiesForHostnames(matching, IncludeHttpOnlyCookies::Yes, ScriptWrittenCookiesOnly::No, [] { });

    WebKit::WebStorageNamespaceProvider::clearLocalStorageForOrigin(origin);
    WebDatabaseProvider::singleton().deleteDatabasesForOrigin(origin);
    MemoryCache::singleton().removeResourcesWithOrigin(origin.securityOrigin());
    DiskCache::removeSite(site);
    log("dati del sito cancellati: %s (%u domini di cookie)", origin.host().utf8().data(), matching.size());
}

} // namespace Lumia
