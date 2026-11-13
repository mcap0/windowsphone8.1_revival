#include "config.h"
#include "Storage.h"

#include "BytecodeCache.h"
#include "CacheStorage.h"
#include "DiskCache.h"
#include "Engine.h"
#include "Permissions.h"
#include "ServiceWorkers.h"
#include "VisitedLinks.h"
#include <wtf/text/StringBuilder.h>
#include "StorageNamespaceImpl.h"
#include "StorageThread.h"
#include "StorageTracker.h"
#include "WebDatabaseProvider.h"
#include "WebPage.h"
#include "WebStorageNamespaceProvider.h"
#include <WebCore/BackForwardCache.h>
#include <WebCore/CookieJar.h>
#include <WebCore/CredentialStorage.h>
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

StorageNamespaceProvider& privateStorageNamespaceProvider()
{
    // An empty path, not a null one: StorageNamespaceImpl makes no StorageSyncManager for it (no file at all).
    static NeverDestroyed<Ref<WebKit::WebStorageNamespaceProvider>> provider = WebKit::WebStorageNamespaceProvider::create(emptyString());
    return provider.get();
}

DatabaseProvider& databaseProvider()
{
    return WebDatabaseProvider::singleton();
}

void endPrivateSession()
{
    auto& session = WebKit::WebPage::ephemeralStorageSession();
    session.deleteAllCookies([] { });
    session.credentialStorage().clearCredentials();
    // The namespace without a path is the private tabs' one (privateStorageNamespaceProvider).
    WebKit::StorageNamespaceImpl::getOrCreateLocalStorageNamespace(emptyString(), 5 * 1024 * 1024, PAL::SessionID::legacyPrivateSessionID())->clearAllOriginsForDeletion();
    WebDatabaseProvider::singleton().deleteAllDatabasesForSession(PAL::SessionID::legacyPrivateSessionID());
    MemoryCache::singleton().evictResources(PAL::SessionID::legacyPrivateSessionID());
    forgetPermissions(true);
    log("sessione privata chiusa: cookie, password, dati dei siti e permessi delle schede private cancellati");
}

void clearData(unsigned what)
{
    Vector<const char*> done;
    auto& session = WebKit::WebPage::storageSession();
    if (what & LW_CLEAR_COOKIES) {
        session.deleteAllCookies([] { });
        done.append("cookie");
    }
    if (what & LW_CLEAR_CACHE) {
        MemoryCache::singleton().evictResources();
        BackForwardCache::singleton().pruneToSizeNow(0, PruningReason::None);
        DiskCache::clear();
        BytecodeCache::clear();
        done.append("cache");
    }
    if (what & LW_CLEAR_STORAGE) {
        WebKit::StorageTracker::tracker().deleteAllOrigins();
        WebKit::WebStorageNamespaceProvider::clearLocalStorageForAllOrigins();
        WebDatabaseProvider::singleton().deleteAllDatabases();
        // Service workers are site data too (a registration keeps a site's code running), and so is what they cached.
        clearAllServiceWorkers([] { });
        clearCacheStorage(nullptr, [] { });
        done.append("dati dei siti");
    }
    if (what & LW_CLEAR_PASSWORDS) {
        session.credentialStorage().clearCredentials();
        done.append("password");
    }
    if (what & LW_CLEAR_PERMISSIONS) {
        forgetPermissions(false);
        done.append("permessi");
    }
    if (what & LW_CLEAR_HISTORY) {
        clearVisitedLinks();
        done.append("link visitati");
    }
    StringBuilder list;
    for (auto* item : done)
        list.append(list.isEmpty() ? ""_s : ", "_s, String::fromLatin1(item));
    log("cancellati: %s", list.toString().utf8().data());
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
    clearServiceWorkers(origin, [] { });
    clearCacheStorage(&origin, [] { });
    MemoryCache::singleton().removeResourcesWithOrigin(origin.securityOrigin());
    DiskCache::removeSite(site);
    log("dati del sito cancellati: %s (%u domini di cookie)", origin.host().utf8().data(), matching.size());
}

} // namespace Lumia
