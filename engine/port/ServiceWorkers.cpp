#include "config.h"
#include "ServiceWorkers.h"

#include "CacheStorage.h"
#include "Engine.h"
#include "Storage.h"
#include "WebPage.h"
#include "WebPreferencesDefaults.h"
#include <WebCore/BackgroundFetchInformation.h>
#include <WebCore/BackgroundFetchOptions.h>
#include <WebCore/BackgroundFetchRecordInformation.h>
#include <WebCore/BackgroundFetchRecordLoader.h>
#include <WebCore/BackgroundFetchRequest.h>
#include <WebCore/BackgroundFetchStore.h>
#include <WebCore/ClientOrigin.h>
#include <WebCore/CookieChangeSubscription.h>
#include <WebCore/CrossOriginAccessControl.h>
#include <WebCore/CrossOriginEmbedderPolicy.h>
#include <WebCore/DocumentLoader.h>
#include <WebCore/EmptyClients.h>
#include <WebCore/EmptyFrameLoaderClient.h>
#include <WebCore/ExceptionData.h>
#include <WebCore/ExceptionOr.h>
#include <WebCore/FetchLoader.h>
#include <WebCore/FetchLoaderClient.h>
#include <WebCore/FormData.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/FrameNetworkingContext.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/FrameInlines.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/MessageWithMessagePorts.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/NetworkStorageSession.h>
#include <WebCore/NotificationData.h>
#include <WebCore/NotificationPayload.h>
#include <WebCore/Page.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/ProcessIdentifier.h>
#include <WebCore/PushSubscriptionData.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceHandle.h>
#include <WebCore/ResourceHandleClient.h>
#include <WebCore/ResourceLoader.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/RetrieveRecordsOptions.h>
#include <WebCore/SWClientConnection.h>
#include <WebCore/SWContextManager.h>
#include <WebCore/SWOriginStore.h>
#include <WebCore/SWRegistrationDatabase.h>
#include <WebCore/SWRegistrationStore.h>
#include <WebCore/SWServer.h>
#include <WebCore/SWServerDelegate.h>
#include <WebCore/SWServerRegistration.h>
#include <WebCore/SWServerToContextConnection.h>
#include <WebCore/SWServerWorker.h>
#include <WebCore/ScriptBuffer.h>
#include <WebCore/SecurityOrigin.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/ServiceWorkerClientData.h>
#include <WebCore/ServiceWorkerClientQueryOptions.h>
#include <WebCore/ServiceWorkerContextData.h>
#include <WebCore/ServiceWorkerData.h>
#include <WebCore/ServiceWorkerFetch.h>
#include <WebCore/ServiceWorkerJob.h>
#include <WebCore/ServiceWorkerJobData.h>
#include <WebCore/ServiceWorkerProvider.h>
#include <WebCore/ServiceWorkerRegistrationData.h>
#include <WebCore/ServiceWorkerRegistrationKey.h>
#include <WebCore/ServiceWorkerRoute.h>
#include <WebCore/ServiceWorkerThread.h>
#include <WebCore/ServiceWorkerThreadProxy.h>
#include <WebCore/Settings.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/Site.h>
#include <WebCore/SocketProvider.h>
#include <WebCore/CacheStorageProvider.h>
#include <WebCore/StorageNamespaceProvider.h>
#include <WebCore/DatabaseProvider.h>
#include <WebCore/TextResourceDecoder.h>
#include <WebCore/WorkerFetchResult.h>
#include <WebCore/WorkerScriptLoader.h>
#include <pal/SessionID.h>
#include <wtf/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/MainThread.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/WeakPtr.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace Lumia {
using namespace WebCore;

namespace {

class ClientConnection;
class ServerConnection;
class OriginStore;
class ContextConnection;
class FetchTask;

// All set once by initializeServiceWorkers and never freed. Web thread.
SWServer* s_server;
ServerConnection* s_serverConnection;
ClientConnection* s_clientConnection;
OriginStore* s_originStore;

// Where WebKit2 sends a message: the receiver runs on a later turn of the Web thread's run loop, never inside the
// sender (WebCore's classes on both sides are written for that), and in the order sent.
void later(Function<void()>&& task)
{
    RunLoop::mainSingleton().dispatch(WTF::move(task));
}

// The worker side calls its connection from the Web thread or from the worker's own.
void onWebThread(Function<void()>&& task)
{
    if (isMainThread())
        later(WTF::move(task));
    else
        callOnMainThread(WTF::move(task));
}

String serviceWorkersDirectory()
{
    auto* dataDirectory = config().data_directory;
    return dataDirectory ? FileSystem::pathByAppendingComponent(String(dataDirectory), "ServiceWorkers"_s) : String();
}

ExceptionData notSupported(ASCIILiteral what)
{
    return ExceptionData { ExceptionCode::NotSupportedError, makeString(what, " is not available in this browser"_s) };
}

// ---------------------------------------------------------------------------------------------------------------
// The server's stores.

// The top origins that have a registration: what a navigation asks before it bothers the server at all.
class OriginStore final : public SWOriginStore {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(OriginStore);
public:
    bool mayContain(const SecurityOriginData& origin) const { return !m_imported || m_origins.contains(origin); }

private:
    void importComplete() final { m_imported = true; }
    void addToStore(const SecurityOriginData& origin) final { m_origins.add(origin); }
    void removeFromStore(const SecurityOriginData& origin) final { m_origins.remove(origin); }
    void clearStore() final { m_origins.clear(); }

    HashSet<SecurityOriginData> m_origins;
    bool m_imported { false };
};

// Registrations and their scripts on disk: WebCore's SWRegistrationDatabase (SQLite + script files), on a queue of
// its own as WebKit2's storage manager runs it.
class RegistrationStore final : public SWRegistrationStore {
public:
    static Ref<RegistrationStore> create(SWServer& server) { return adoptRef(*new RegistrationStore(server)); }

private:
    explicit RegistrationStore(SWServer& server)
        : m_server(server)
        , m_directory(serviceWorkersDirectory().isolatedCopy())
        , m_queue(WorkQueue::create("ServiceWorker registrations"_s))
        , m_updateTimer(*this, &RegistrationStore::updateTimerFired)
    {
    }

    // On the queue.
    SWRegistrationDatabase* database()
    {
        if (!m_database && !m_directory.isEmpty())
            m_database = makeUnique<SWRegistrationDatabase>(m_directory);
        return m_database.get();
    }

    void clearAll(CompletionHandler<void()>&& callback) final
    {
        m_updates.clear();
        m_updateTimer.stop();
        m_queue->dispatch([this, callback = WTF::move(callback)]() mutable {
            if (auto* database = this->database())
                database->deleteAllFiles();
            callOnMainThread(WTF::move(callback));
        });
    }

    void flushChanges(CompletionHandler<void()>&& callback) final
    {
        m_updateTimer.stop();
        updateToStorage(WTF::move(callback));
    }

    void closeFiles(CompletionHandler<void()>&& callback) final
    {
        m_queue->dispatch([this, callback = WTF::move(callback)]() mutable {
            m_database = nullptr;
            callOnMainThread(WTF::move(callback));
        });
    }

    void importRegistrationsForOrigin(const SecurityOriginData& topOrigin, CompletionHandler<void(std::optional<Vector<ServiceWorkerContextData>>&&)>&& callback) final
    {
        m_queue->dispatch([this, topOrigin = topOrigin.isolatedCopy(), callback = WTF::move(callback)]() mutable {
            std::optional<Vector<ServiceWorkerContextData>> result;
            if (auto* database = this->database())
                result = database->importRegistrations(topOrigin);
            callOnMainThread([result = crossThreadCopy(WTF::move(result)), callback = WTF::move(callback)]() mutable {
                callback(WTF::move(result));
            });
        });
    }

    void importOriginList(CompletionHandler<void(std::optional<HashSet<ClientOrigin>>&&)>&& callback) final
    {
        m_queue->dispatch([this, callback = WTF::move(callback)]() mutable {
            std::optional<HashSet<ClientOrigin>> result;
            if (auto* database = this->database())
                result = database->importOrigins();
            callOnMainThread([result = crossThreadCopy(WTF::move(result)), callback = WTF::move(callback)]() mutable {
                callback(WTF::move(result));
            });
        });
    }

    void updateRegistration(const ServiceWorkerContextData& registration) final
    {
        m_updates.set(registration.registration.key, registration.copy());
        scheduleUpdate();
    }

    void removeRegistration(const ServiceWorkerRegistrationKey& key) final
    {
        m_updates.set(key, std::nullopt);
        scheduleUpdate();
    }

    void retrieveWorkerScripts(ServiceWorkerIdentifier identifier, const ServiceWorkerRegistrationKey& key, const URL& scriptURL, const Vector<URL>& importedScriptURLs, CompletionHandler<void(std::optional<ServiceWorkerScripts>&&)>&& callback) final
    {
        m_queue->dispatch([this, identifier, key = key.isolatedCopy(), scriptURL = scriptURL.isolatedCopy(), importedScriptURLs = crossThreadCopy(importedScriptURLs), callback = WTF::move(callback)]() mutable {
            std::optional<ServiceWorkerScripts> result;
            if (auto* database = this->database())
                result = database->retrieveWorkerScripts(identifier, key, scriptURL, importedScriptURLs);
            callOnMainThread([result = crossThreadCopy(WTF::move(result)), callback = WTF::move(callback)]() mutable {
                callback(WTF::move(result));
            });
        });
    }

    void scheduleUpdate()
    {
        if (!m_updateTimer.isActive())
            m_updateTimer.startOneShot(0_s);
    }

    void updateTimerFired()
    {
        updateToStorage([] { });
    }

    void updateToStorage(CompletionHandler<void()>&& callback)
    {
        Vector<ServiceWorkerRegistrationKey> toDelete;
        Vector<ServiceWorkerContextData> toUpdate;
        for (auto& [key, registration] : m_updates) {
            if (!registration)
                toDelete.append(key);
            else
                toUpdate.append(WTF::move(*registration));
        }
        m_updates.clear();

        m_queue->dispatch([this, toUpdate = crossThreadCopy(WTF::move(toUpdate)), toDelete = crossThreadCopy(WTF::move(toDelete)), callback = WTF::move(callback)]() mutable {
            std::optional<Vector<ServiceWorkerScripts>> result;
            if (auto* database = this->database())
                result = database->updateRegistrations(toUpdate, toDelete);
            callOnMainThread([this, result = crossThreadCopy(WTF::move(result)), callback = WTF::move(callback)]() mutable {
                RefPtr server = m_server.get();
                if (server && result) {
                    for (auto& scripts : *result)
                        server->didSaveWorkerScriptsToDisk(scripts.identifier, WTF::move(scripts.mainScript), WTF::move(scripts.importedScripts));
                }
                callback();
            });
        });
    }

    WeakPtr<SWServer> m_server;
    const String m_directory;
    const Ref<WorkQueue> m_queue;
    std::unique_ptr<SWRegistrationDatabase> m_database; // On the queue.
    Timer m_updateTimer;
    HashMap<ServiceWorkerRegistrationKey, std::optional<ServiceWorkerContextData>> m_updates;
};

// Background Fetch is not offered (no downloads with the app closed): the engine asks for a store all the same.
class NoBackgroundFetchStore final : public BackgroundFetchStore {
public:
    static Ref<NoBackgroundFetchStore> create() { return adoptRef(*new NoBackgroundFetchStore); }

private:
    void initializeFetches(const ServiceWorkerRegistrationKey&, CompletionHandler<void()>&& callback) final { callback(); }
    void clearFetch(const ServiceWorkerRegistrationKey&, const String&, CompletionHandler<void()>&& callback) final { callback(); }
    void clearAllFetches(const ServiceWorkerRegistrationKey&, CompletionHandler<void()>&& callback) final { callback(); }
    void storeFetch(const ServiceWorkerRegistrationKey&, const String&, uint64_t, uint64_t, std::optional<size_t>, Vector<uint8_t>&&, CompletionHandler<void(StoreResult)>&& callback) final { callback(StoreResult::InternalError); }
    void storeFetchResponseBodyChunk(const ServiceWorkerRegistrationKey&, const String&, size_t, const SharedBuffer&, CompletionHandler<void(StoreResult)>&& callback) final { callback(StoreResult::InternalError); }
    void retrieveResponseBody(const ServiceWorkerRegistrationKey&, const String&, size_t, RetrieveRecordResponseBodyCallback&& callback) final { callback(makeUnexpected(ResourceError { ResourceError::Type::General })); }
};

// ---------------------------------------------------------------------------------------------------------------
// Loads the server makes for itself (no page behind them): the script of a soft update.

class ServerNetworkingContext final : public NetworkingContext {
public:
    static Ref<ServerNetworkingContext> create() { return adoptRef(*new ServerNetworkingContext); }

private:
    NetworkStorageSession* storageSession() const final { return &WebKit::WebPage::storageSession(); }
    bool shouldClearReferrerOnHTTPSToHTTPRedirect() const final { return true; }
    ResourceError blockedError(const ResourceRequest& request) const final { return ResourceError { errorDomainWebKitInternal, 0, request.url(), "Blocked"_s }; }
};

// https://w3c.github.io/ServiceWorker/#soft-update: the registration's script fetched again, by the server. What
// WebKit2's ServiceWorkerSoftUpdateLoader does, over curl (whose own HTTP cache answers when it may).
class SoftUpdateLoader final : public RefCounted<SoftUpdateLoader>, public ResourceHandleClient {
public:
    using Handler = CompletionHandler<void(WorkerFetchResult&&)>;

    static void start(ServiceWorkerJobData&& jobData, bool shouldRefreshCache, ResourceRequest&& request, Handler&& handler)
    {
        Ref loader = adoptRef(*new SoftUpdateLoader(WTF::move(jobData), WTF::move(handler)));
        if (shouldRefreshCache)
            request.setCachePolicy(ResourceRequestCachePolicy::ReloadIgnoringCacheData);
        loader->m_context = ServerNetworkingContext::create();
        loader->m_handle = ResourceHandle::create(loader->m_context.get(), request, loader.ptr(), false, false, ContentEncodingSniffingPolicy::Default, nullptr, false);
        if (!loader->m_handle) {
            loader->fail(ResourceError { errorDomainWebKitInternal, 0, request.url(), "The request did not start"_s });
            return;
        }
        // Alive until it ends.
        loader->m_self = loader.ptr();
    }

private:
    SoftUpdateLoader(ServiceWorkerJobData&& jobData, Handler&& handler)
        : m_jobData(WTF::move(jobData))
        , m_handler(WTF::move(handler))
    {
    }

    void fail(ResourceError&& error)
    {
        if (m_handler)
            m_handler(workerFetchError(WTF::move(error)));
        finish();
    }

    void finish()
    {
        if (RefPtr handle = std::exchange(m_handle, nullptr)) {
            handle->clearClient();
            handle->cancel();
        }
        m_self = nullptr; // May delete this.
    }

    void willSendRequestAsync(ResourceHandle*, ResourceRequest&&, ResourceResponse&&, CompletionHandler<void(ResourceRequest&&)>&& completionHandler) final
    {
        // The script of a service worker is never the answer of a redirect.
        Ref protectedThis { *this };
        completionHandler({ });
        fail(ResourceError { ResourceError::Type::Cancellation });
    }

    void didReceiveResponseAsync(ResourceHandle*, ResourceResponse&& response, CompletionHandler<void()>&& completionHandler) final
    {
        Ref protectedThis { *this };
        if (auto info = response.certificateInfo())
            m_certificateInfo = *info;
        auto source = m_jobData.workerType == WorkerType::Module ? WorkerScriptLoader::Source::ModuleScript : WorkerScriptLoader::Source::ClassicWorkerScript;
        auto error = WorkerScriptLoader::validateWorkerResponse(response, source, FetchOptions::Destination::Serviceworker);
        if (error.isNull())
            error = ServiceWorkerJob::validateServiceWorkerResponse(m_jobData, response);
        if (!error.isNull()) {
            completionHandler();
            fail(WTF::move(error));
            return;
        }
        m_contentSecurityPolicy = ContentSecurityPolicyResponseHeaders { response };
        m_crossOriginEmbedderPolicy = obtainCrossOriginEmbedderPolicy(response, nullptr);
        m_referrerPolicy = response.httpHeaderField(HTTPHeaderName::ReferrerPolicy);
        auto encoding = response.textEncodingName();
        m_decoder = TextResourceDecoder::create("text/javascript"_s, encoding.isEmpty() ? "UTF-8"_s : encoding);
        completionHandler();
    }

    void didReceiveData(ResourceHandle*, const SharedBuffer& buffer, int) final
    {
        if (m_decoder && buffer.size())
            m_script.append(m_decoder->decode(buffer.span()));
    }

    void didFinishLoading(ResourceHandle*, const NetworkLoadMetrics&) final
    {
        Ref protectedThis { *this };
        if (m_decoder)
            m_script.append(m_decoder->flush());
        if (m_handler)
            m_handler({ ScriptBuffer { m_script.toString() }, m_jobData.scriptURL, m_certificateInfo, m_contentSecurityPolicy, m_crossOriginEmbedderPolicy, m_referrerPolicy, { } });
        finish();
    }

    void didFail(ResourceHandle*, const ResourceError& error) final
    {
        Ref protectedThis { *this };
        fail(ResourceError { error });
    }

#if USE(PROTECTION_SPACE_AUTH_CALLBACK)
    void canAuthenticateAgainstProtectionSpaceAsync(ResourceHandle*, const ProtectionSpace&, CompletionHandler<void(bool)>&& completionHandler) final { completionHandler(false); }
#endif

    ServiceWorkerJobData m_jobData;
    Handler m_handler;
    RefPtr<ServerNetworkingContext> m_context;
    RefPtr<ResourceHandle> m_handle;
    RefPtr<SoftUpdateLoader> m_self;
    RefPtr<TextResourceDecoder> m_decoder;
    StringBuilder m_script;
    CertificateInfo m_certificateInfo;
    ContentSecurityPolicyResponseHeaders m_contentSecurityPolicy;
    CrossOriginEmbedderPolicy m_crossOriginEmbedderPolicy;
    String m_referrerPolicy;
};

// ---------------------------------------------------------------------------------------------------------------
// The worker's own page. WebCore runs a service worker behind a Page with no view (its loads, its settings, its
// storage come from it); this is that page's frame loader client, as WebKit2's RemoteWorkerFrameLoaderClient.

class WorkerNetworkingContext final : public FrameNetworkingContext {
public:
    static Ref<WorkerNetworkingContext> create(LocalFrame& frame) { return adoptRef(*new WorkerNetworkingContext(frame)); }

private:
    explicit WorkerNetworkingContext(LocalFrame& frame)
        : FrameNetworkingContext(&frame)
    {
    }

    NetworkStorageSession* storageSession() const final { return &WebKit::WebPage::storageSession(); }
    ResourceError blockedError(const ResourceRequest& request) const final { return ResourceError { errorDomainWebKitInternal, 0, request.url(), "Blocked"_s }; }
};

class WorkerFrameLoaderClient final : public EmptyFrameLoaderClient {
public:
    WorkerFrameLoaderClient(LocalFrame& frame, FrameLoader& loader, const String& userAgent)
        : EmptyFrameLoaderClient(loader)
        , m_frame(frame)
        , m_userAgent(userAgent)
    {
    }

private:
    Ref<FrameNetworkingContext> createNetworkingContext() final { return WorkerNetworkingContext::create(Ref { m_frame.get() }); }
    String userAgent(const URL&) const final { return m_userAgent; }
    bool shouldUseCredentialStorage(DocumentLoader*, ResourceLoaderIdentifier) final { return true; }

    WeakRef<LocalFrame> m_frame;
    String m_userAgent;
};

// ---------------------------------------------------------------------------------------------------------------
// Server -> workers. One per site whose workers run (the server keeps them apart as it would processes).

class ContextConnection final : public SWServerToContextConnection {
public:
    static Ref<ContextConnection> create(SWServer& server, Site&& site, std::optional<ScriptExecutionContextIdentifier> serviceWorkerPageIdentifier)
    {
        return adoptRef(*new ContextConnection(server, WTF::move(site), serviceWorkerPageIdentifier));
    }

    static HashMap<SWServerToContextConnectionIdentifier, Ref<ContextConnection>>& all()
    {
        static NeverDestroyed<HashMap<SWServerToContextConnectionIdentifier, Ref<ContextConnection>>> map;
        return map;
    }

private:
    ContextConnection(SWServer& server, Site&& site, std::optional<ScriptExecutionContextIdentifier> serviceWorkerPageIdentifier)
        : SWServerToContextConnection(server, WTF::move(site), serviceWorkerPageIdentifier)
    {
    }

    void installServiceWorkerContext(const ServiceWorkerContextData& contextData, const ServiceWorkerData& workerData, const String& userAgent, WorkerThreadMode workerThreadMode, OptionSet<AdvancedPrivacyProtections> advancedPrivacyProtections) final
    {
        later([contextData = contextData.copy(), workerData = ServiceWorkerData { workerData }, userAgent, workerThreadMode, advancedPrivacyProtections]() mutable {
            auto effectiveUserAgent = userAgent.isNull() ? Lumia::userAgent(contextData.scriptURL) : userAgent;

            auto configuration = pageConfigurationWithEmptyClients(PageIdentifier::generate(), PAL::SessionID::defaultSessionID());
            configuration.cookieJar = WebKit::WebPage::createCookieJar(false);
            configuration.storageNamespaceProvider = &storageNamespaceProvider();
            configuration.databaseProvider = &databaseProvider();
            configuration.socketProvider = WebKit::WebPage::socketProvider();
            configuration.cacheStorageProvider = Ref { cacheStorageProvider() };
            configuration.mainFrameCreationParameters = PageConfiguration::LocalMainFrameCreationParameters {
                { [effectiveUserAgent] (auto& frame, auto& frameLoader) {
                    return makeUniqueRefWithoutRefCountedCheck<WorkerFrameLoaderClient>(frame, frameLoader, effectiveUserAgent);
                } },
                SandboxFlags { },
                ReferrerPolicy::EmptyString
            };

            Ref page = Page::create(WTF::move(configuration));
            WebKit::applyWebKitDefaults(page->settings());
            page->settings().setServiceWorkersEnabled(true);
            page->setupForRemoteWorker(contextData.scriptURL, contextData.registration.key.topOrigin(), contextData.referrerPolicy, advancedPrivacyProtections);

            auto identifier = contextData.serviceWorkerIdentifier;
            auto scriptURL = contextData.scriptURL;
            auto proxy = ServiceWorkerThreadProxy::create(WTF::move(page), WTF::move(contextData), WTF::move(workerData), WTF::move(effectiveUserAgent), workerThreadMode, cacheStorageProvider(), std::unique_ptr<NotificationClient> { });
            SWContextManager::singleton().registerServiceWorkerThreadForInstall(WTF::move(proxy));
            log("sw: worker %llu avviato, %s", static_cast<unsigned long long>(identifier.toUInt64()), scriptURL.string().utf8().data());
        });
    }

    void updateAppInitiatedValue(ServiceWorkerIdentifier, LastNavigationWasAppInitiated) final { }

    void fireInstallEvent(ServiceWorkerIdentifier identifier) final
    {
        later([identifier] { SWContextManager::singleton().fireInstallEvent(identifier); });
    }

    void fireActivateEvent(ServiceWorkerIdentifier identifier) final
    {
        later([identifier] { SWContextManager::singleton().fireActivateEvent(identifier); });
    }

    void terminateWorker(ServiceWorkerIdentifier identifier) final
    {
        later([identifier] {
            SWContextManager::singleton().terminateWorker(identifier, SWContextManager::workerTerminationTimeout, nullptr);
        });
    }

    // The scripts are handed over in memory; a worker that is running keeps the copy it started from.
    void didSaveScriptsToDisk(ServiceWorkerIdentifier, const ScriptBuffer&, const MemoryCompactRobinHoodHashMap<URL, ScriptBuffer>&) final { }

    void firePushEvent(ServiceWorkerIdentifier identifier, const std::optional<Vector<uint8_t>>& data, std::optional<NotificationPayload>&& payload, CompletionHandler<void(bool, std::optional<NotificationPayload>&&)>&& callback) final
    {
        later([identifier, data = data, payload = WTF::move(payload), callback = WTF::move(callback)]() mutable {
            SWContextManager::singleton().firePushEvent(identifier, WTF::move(data), WTF::move(payload), WTF::move(callback));
        });
    }

    void fireNotificationEvent(ServiceWorkerIdentifier identifier, const NotificationData& data, NotificationEventType type, CompletionHandler<void(bool)>&& callback) final
    {
        later([identifier, data = NotificationData { data }, type, callback = WTF::move(callback)]() mutable {
            SWContextManager::singleton().fireNotificationEvent(identifier, WTF::move(data), type, WTF::move(callback));
        });
    }

    void fireBackgroundFetchEvent(ServiceWorkerIdentifier identifier, const BackgroundFetchInformation& information, CompletionHandler<void(bool)>&& callback) final
    {
        later([identifier, information = BackgroundFetchInformation { information }, callback = WTF::move(callback)]() mutable {
            SWContextManager::singleton().fireBackgroundFetchEvent(identifier, WTF::move(information), WTF::move(callback));
        });
    }

    void fireBackgroundFetchClickEvent(ServiceWorkerIdentifier identifier, const BackgroundFetchInformation& information, CompletionHandler<void(bool)>&& callback) final
    {
        later([identifier, information = BackgroundFetchInformation { information }, callback = WTF::move(callback)]() mutable {
            SWContextManager::singleton().fireBackgroundFetchClickEvent(identifier, WTF::move(information), WTF::move(callback));
        });
    }

    ProcessIdentifier webProcessIdentifier() const final { return Process::identifier(); }

    // A worker cannot open a tab here: the app's window.open path belongs to a page the user is looking at.
    void openWindow(ServiceWorkerIdentifier, const URL&, OpenWindowCallback&& callback) final
    {
        callback(makeUnexpected(notSupported("clients.openWindow"_s)));
    }

    void connectionIsNoLongerNeeded() final
    {
        later([identifier = this->identifier()] {
            RefPtr connection = all().take(identifier);
            if (!connection)
                return;
            if (RefPtr server = connection->server())
                server->removeContextConnection(*connection);
        });
    }

    void terminateDueToUnresponsiveness() final
    {
        log("sw: i worker di %s non rispondono", registrableDomain().string().utf8().data());
    }

    void setInspectable(ServiceWorkerIsInspectable) final { }

#if ENABLE(CONTENT_EXTENSIONS)
    void reportNetworkUsageToWorkerClient(const ScriptExecutionContextIdentifier, uint64_t) final { }
#endif
};

RefPtr<SWServerToContextConnection> contextConnectionOf(ServiceWorkerIdentifier identifier)
{
    RefPtr worker = SWServerWorker::existingWorkerForIdentifier(identifier);
    return worker ? worker->contextConnection() : nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Server -> pages.

class ClientConnection final : public SWClientConnection {
public:
    static Ref<ClientConnection> create() { return adoptRef(*new ClientConnection); }

    // What the server connection sends; SWClientConnection's handlers are protected.
    void rejectJob(ServiceWorkerJobIdentifier identifier, ExceptionData&& data) { jobRejectedInServer(identifier, WTF::move(data)); }
    void resolveRegistrationJob(ServiceWorkerJobIdentifier identifier, ServiceWorkerRegistrationData&& data, ShouldNotifyWhenResolved notify) { registrationJobResolvedInServer(identifier, WTF::move(data), notify); }
    void startScriptFetch(ServiceWorkerJobIdentifier identifier, ServiceWorkerRegistrationKey&& key, FetchOptions::Cache cachePolicy) { startScriptFetchForServer(identifier, WTF::move(key), cachePolicy); }
    void postMessageToClient(ScriptExecutionContextIdentifier destination, MessageWithMessagePorts&& message, ServiceWorkerData&& source, const SecurityOriginData& sourceOrigin) { postMessageToServiceWorkerClient(destination, WTF::move(message), WTF::move(source), sourceOrigin); }
    void registrationStateChanged(ServiceWorkerRegistrationIdentifier identifier, ServiceWorkerRegistrationState state, const std::optional<ServiceWorkerData>& data) { updateRegistrationState(identifier, state, data); }
    void workerStateChanged(ServiceWorkerIdentifier identifier, ServiceWorkerState state) { updateWorkerState(identifier, state); }
    void updateFound(ServiceWorkerRegistrationIdentifier identifier) { fireUpdateFoundEvent(identifier); }
    void lastUpdateTimeChanged(ServiceWorkerRegistrationIdentifier identifier, WallTime time) { setRegistrationLastUpdateTime(identifier, time); }
    void updateViaCacheChanged(ServiceWorkerRegistrationIdentifier identifier, ServiceWorkerUpdateViaCache value) { setRegistrationUpdateViaCache(identifier, value); }
    void controllerChanged(const HashSet<ScriptExecutionContextIdentifier>& contexts, std::optional<ServiceWorkerData>&& controller) { notifyClientsOfControllerChange(contexts, WTF::move(controller)); }

private:
    ClientConnection() = default;

    void matchRegistration(SecurityOriginData&& topOrigin, const URL& clientURL, RegistrationCallback&&) final;
    void getRegistrations(SecurityOriginData&& topOrigin, const URL& clientURL, GetRegistrationsCallback&&) final;
    void whenRegistrationReady(const SecurityOriginData& topOrigin, const URL& clientURL, WhenRegistrationReadyCallback&&) final;
    void addServiceWorkerRegistrationInServer(ServiceWorkerRegistrationIdentifier) final;
    void removeServiceWorkerRegistrationInServer(ServiceWorkerRegistrationIdentifier) final;
    void scheduleUnregisterJobInServer(ServiceWorkerRegistrationIdentifier, ServiceWorkerOrClientIdentifier, CompletionHandler<void(ExceptionOr<bool>&&)>&&) final;
    void registerServiceWorkerInServer(ServiceWorkerIdentifier) final;
    void unregisterServiceWorkerInServer(ServiceWorkerIdentifier) final;
    void didResolveRegistrationPromise(const ServiceWorkerRegistrationKey&) final;
    void postMessageToServiceWorker(ServiceWorkerIdentifier destination, MessageWithMessagePorts&&, const ServiceWorkerOrClientIdentifier& source) final;
    SWServerConnectionIdentifier serverConnectionIdentifier() const final;
    bool mayHaveServiceWorkerRegisteredForOrigin(const SecurityOriginData&) const final;
    void registerServiceWorkerClient(const ClientOrigin&, ServiceWorkerClientData&&, const std::optional<ServiceWorkerRegistrationIdentifier>&, String&& userAgent) final;
    void unregisterServiceWorkerClient(ScriptExecutionContextIdentifier) final;
    void finishFetchingScriptInServer(const ServiceWorkerJobDataIdentifier&, ServiceWorkerRegistrationKey&&, WorkerFetchResult&&) final;
    void scheduleJobInServer(const ServiceWorkerJobData&) final;

    // Push needs a push service behind the browser, which this one does not have.
    void subscribeToPushService(ServiceWorkerRegistrationIdentifier, const Vector<uint8_t>&, SubscribeToPushServiceCallback&& callback) final { callback(notSupported("Push"_s).toException()); }
    void unsubscribeFromPushService(ServiceWorkerRegistrationIdentifier, PushSubscriptionIdentifier, UnsubscribeFromPushServiceCallback&& callback) final { callback(false); }
    void getPushSubscription(ServiceWorkerRegistrationIdentifier, GetPushSubscriptionCallback&& callback) final { callback(std::optional<PushSubscriptionData> { }); }
    void getPushPermissionState(ServiceWorkerRegistrationIdentifier, GetPushPermissionStateCallback&& callback) final { callback(PushPermissionState::Denied); }
#if ENABLE(NOTIFICATION_EVENT)
    void getNotifications(const URL&, const String&, GetNotificationsCallback&& callback) final { callback(Vector<NotificationData> { }); }
#endif

    void enableNavigationPreload(ServiceWorkerRegistrationIdentifier, ExceptionOrVoidCallback&&) final;
    void disableNavigationPreload(ServiceWorkerRegistrationIdentifier, ExceptionOrVoidCallback&&) final;
    void setNavigationPreloadHeaderValue(ServiceWorkerRegistrationIdentifier, String&&, ExceptionOrVoidCallback&&) final;
    void getNavigationPreloadState(ServiceWorkerRegistrationIdentifier, ExceptionOrNavigationPreloadStateCallback&&) final;

    void startBackgroundFetch(ServiceWorkerRegistrationIdentifier, const String&, Vector<BackgroundFetchRequest>&&, BackgroundFetchOptions&&, ExceptionOrBackgroundFetchInformationCallback&& callback) final { callback(notSupported("Background Fetch"_s).toException()); }
    void backgroundFetchInformation(ServiceWorkerRegistrationIdentifier, const String&, ExceptionOrBackgroundFetchInformationCallback&& callback) final { callback(std::optional<BackgroundFetchInformation> { }); }
    void backgroundFetchIdentifiers(ServiceWorkerRegistrationIdentifier, BackgroundFetchIdentifiersCallback&& callback) final { callback({ }); }
    void abortBackgroundFetch(ServiceWorkerRegistrationIdentifier, const String&, AbortBackgroundFetchCallback&& callback) final { callback(false); }
    void matchBackgroundFetch(ServiceWorkerRegistrationIdentifier, const String&, RetrieveRecordsOptions&&, MatchBackgroundFetchCallback&& callback) final { callback({ }); }
    void retrieveRecordResponse(BackgroundFetchRecordIdentifier, RetrieveRecordResponseCallback&& callback) final { callback(notSupported("Background Fetch"_s).toException()); }
    void retrieveRecordResponseBody(BackgroundFetchRecordIdentifier, RetrieveRecordResponseBodyCallback&& callback) final { callback(makeUnexpected(ResourceError { ResourceError::Type::General })); }

    // Cookie Store change events in a worker: no subscriptions are kept.
    void addCookieChangeSubscriptions(ServiceWorkerRegistrationIdentifier, Vector<CookieChangeSubscription>&&, ExceptionOrVoidCallback&& callback) final { callback({ }); }
    void removeCookieChangeSubscriptions(ServiceWorkerRegistrationIdentifier, Vector<CookieChangeSubscription>&&, ExceptionOrVoidCallback&& callback) final { callback({ }); }
    void cookieChangeSubscriptions(ServiceWorkerRegistrationIdentifier, ExceptionOrCookieChangeSubscriptionsCallback&& callback) final { callback(Vector<CookieChangeSubscription> { }); }

    Ref<AddRoutePromise> addRoutes(ServiceWorkerRegistrationIdentifier, Vector<ServiceWorkerRoute>&&) final;
};

// ---------------------------------------------------------------------------------------------------------------
// Pages -> server, and the server's answers to them.

class ServerConnection final : public SWServer::Connection {
public:
    static Ref<ServerConnection> create(SWServer& server) { return adoptRef(*new ServerConnection(server)); }

    // SWServer::Connection keeps these protected: the client connection calls them through here.
    void finishFetchingScript(const ServiceWorkerJobDataIdentifier& identifier, const ServiceWorkerRegistrationKey& key, WorkerFetchResult&& result) { finishFetchingScriptInServer(identifier, key, WTF::move(result)); }
    void addRegistration(ServiceWorkerRegistrationIdentifier identifier) { addServiceWorkerRegistrationInServer(identifier); }
    void removeRegistration(ServiceWorkerRegistrationIdentifier identifier) { removeServiceWorkerRegistrationInServer(identifier); }
    void registerWorker(ServiceWorkerIdentifier identifier) { registerServiceWorkerInServer(identifier); }
    void unregisterWorker(ServiceWorkerIdentifier identifier) { unregisterServiceWorkerInServer(identifier); }
    void whenReady(const SecurityOriginData& topOrigin, const URL& clientURL, CompletionHandler<void(std::optional<ServiceWorkerRegistrationData>&&)>&& callback) { whenRegistrationReady(topOrigin, clientURL, WTF::move(callback)); }
    void flush(CompletionHandler<void()>&& callback) { storeRegistrationsOnDisk(WTF::move(callback)); }

    void scheduleUnregisterJob(ServiceWorkerRegistrationIdentifier registrationIdentifier, ServiceWorkerOrClientIdentifier contextIdentifier, CompletionHandler<void(ExceptionOr<bool>&&)>&& callback)
    {
        RefPtr server = this->server();
        RefPtr registration = server ? server->getRegistration(registrationIdentifier) : nullptr;
        if (!registration)
            return callback(false);

        URL clientURL;
        WTF::switchOn(contextIdentifier, [&](ScriptExecutionContextIdentifier clientIdentifier) {
            auto iterator = m_clientOrigins.find(clientIdentifier);
            if (iterator == m_clientOrigins.end())
                return;
            if (auto data = server->serviceWorkerClientWithOriginByID(iterator->value, clientIdentifier))
                clientURL = data->url;
        }, [&](ServiceWorkerIdentifier workerIdentifier) {
            if (RefPtr worker = server->workerByID(workerIdentifier))
                clientURL = worker->data().scriptURL;
        });
        if (!clientURL.isValid())
            return callback(Exception { ExceptionCode::InvalidStateError, "Client is unknown"_s });

        auto jobIdentifier = ServiceWorkerJobIdentifier::generate();
        m_unregisterJobs.add(jobIdentifier, WTF::move(callback));
        server->scheduleUnregisterJob(ServiceWorkerJobDataIdentifier { identifier(), jobIdentifier }, *registration, contextIdentifier, WTF::move(clientURL));
    }

    void registerClient(ClientOrigin&& clientOrigin, ServiceWorkerClientData&& data, const std::optional<ServiceWorkerRegistrationIdentifier>& controllingRegistration, String&& userAgent, SWServer::IsBeingCreatedClient isBeingCreated)
    {
        RefPtr server = this->server();
        if (!server)
            return;
        // A sandboxed document is not a client.
        if (data.url.protocolIsInHTTPFamily() && clientOrigin.clientOrigin != SecurityOriginData::fromURLWithoutStrictOpaqueness(data.url))
            return;
        m_clientOrigins.set(data.identifier, clientOrigin);
        if (isBeingCreated == SWServer::IsBeingCreatedClient::No) {
            // Messages a worker posted to the client while its document was still loading.
            for (auto&& pending : server->releaseServiceWorkerClientPendingMessage(data.identifier)) {
                later([destination = data.identifier, pending = WTF::move(pending)]() mutable {
                    s_clientConnection->postMessageToClient(destination, WTF::move(pending.message), WTF::move(pending.sourceData), pending.sourceOrigin);
                });
            }
        }
        server->registerServiceWorkerClient(WTF::move(clientOrigin), WTF::move(data), controllingRegistration, WTF::move(userAgent), isBeingCreated);
    }

    void unregisterClient(ScriptExecutionContextIdentifier identifier)
    {
        auto clientOrigin = m_clientOrigins.takeOptional(identifier);
        if (!clientOrigin)
            return;
        if (RefPtr server = this->server())
            server->unregisterServiceWorkerClient(*clientOrigin, identifier);
    }

private:
    explicit ServerConnection(SWServer& server)
        : SWServer::Connection(server, SWServerConnectionIdentifier::generate())
    {
    }

    void rejectJobInClient(ServiceWorkerJobIdentifier jobIdentifier, const ExceptionData& exception) final
    {
        if (auto callback = m_unregisterJobs.take(jobIdentifier)) {
            later([callback = WTF::move(callback), exception]() mutable { callback(exception.toException()); });
            return;
        }
        later([jobIdentifier, exception = ExceptionData { exception }]() mutable { s_clientConnection->rejectJob(jobIdentifier, WTF::move(exception)); });
    }

    void resolveRegistrationJobInClient(ServiceWorkerJobIdentifier jobIdentifier, const ServiceWorkerRegistrationData& data, ShouldNotifyWhenResolved shouldNotify) final
    {
        later([jobIdentifier, data = ServiceWorkerRegistrationData { data }, shouldNotify]() mutable { s_clientConnection->resolveRegistrationJob(jobIdentifier, WTF::move(data), shouldNotify); });
    }

    void resolveUnregistrationJobInClient(ServiceWorkerJobIdentifier jobIdentifier, const ServiceWorkerRegistrationKey&, bool result) final
    {
        if (auto callback = m_unregisterJobs.take(jobIdentifier))
            later([callback = WTF::move(callback), result]() mutable { callback(result); });
    }

    void startScriptFetchInClient(ServiceWorkerJobIdentifier jobIdentifier, const ServiceWorkerRegistrationKey& key, FetchOptions::Cache cachePolicy) final
    {
        later([jobIdentifier, key = ServiceWorkerRegistrationKey { key }, cachePolicy]() mutable { s_clientConnection->startScriptFetch(jobIdentifier, WTF::move(key), cachePolicy); });
    }

    // The five below reach the pages' and dedicated workers' objects through the client connection, and the service
    // workers' own (their registration and its workers, seen from inside) through the context manager.
    void updateRegistrationStateInClient(ServiceWorkerRegistrationIdentifier identifier, ServiceWorkerRegistrationState state, const std::optional<ServiceWorkerData>& data) final
    {
        later([identifier, state, data] {
            s_clientConnection->registrationStateChanged(identifier, state, data);
            SWContextManager::singleton().updateRegistrationState(identifier, state, data);
        });
    }

    void updateWorkerStateInClient(ServiceWorkerIdentifier identifier, ServiceWorkerState state) final
    {
        later([identifier, state] {
            s_clientConnection->workerStateChanged(identifier, state);
            SWContextManager::singleton().updateWorkerState(identifier, state);
        });
    }

    void fireUpdateFoundEvent(ServiceWorkerRegistrationIdentifier identifier) final
    {
        later([identifier] {
            s_clientConnection->updateFound(identifier);
            SWContextManager::singleton().fireUpdateFoundEvent(identifier);
        });
    }

    void setRegistrationLastUpdateTime(ServiceWorkerRegistrationIdentifier identifier, WallTime time) final
    {
        later([identifier, time] {
            s_clientConnection->lastUpdateTimeChanged(identifier, time);
            SWContextManager::singleton().setRegistrationLastUpdateTime(identifier, time);
        });
    }

    void setRegistrationUpdateViaCache(ServiceWorkerRegistrationIdentifier identifier, ServiceWorkerUpdateViaCache value) final
    {
        later([identifier, value] {
            s_clientConnection->updateViaCacheChanged(identifier, value);
            SWContextManager::singleton().setRegistrationUpdateViaCache(identifier, value);
        });
    }

    void notifyClientsOfControllerChange(const HashSet<ScriptExecutionContextIdentifier>& contexts, const std::optional<ServiceWorkerData>& controller) final
    {
        later([contexts, controller = std::optional<ServiceWorkerData> { controller }]() mutable { s_clientConnection->controllerChanged(contexts, WTF::move(controller)); });
    }

    void postMessageToServiceWorkerClient(ScriptExecutionContextIdentifier destination, const MessageWithMessagePorts& message, ServiceWorkerIdentifier source, const SecurityOriginData& sourceOrigin) final
    {
        RefPtr server = this->server();
        if (!server)
            return;
        server->postMessageToServiceWorkerClient(destination, message, source, sourceOrigin, [](auto destination, auto& message, auto& sourceData, auto& sourceOrigin) {
            later([destination, message = MessageWithMessagePorts { message }, sourceData = ServiceWorkerData { sourceData }, sourceOrigin = SecurityOriginData { sourceOrigin }]() mutable {
                s_clientConnection->postMessageToClient(destination, WTF::move(message), WTF::move(sourceData), sourceOrigin);
            });
        });
    }

    // WindowClient.focus(): tabs are the app's, and a page in a hidden one is not shown on a worker's word.
    void focusServiceWorkerClient(ScriptExecutionContextIdentifier, CompletionHandler<void(std::optional<ServiceWorkerClientData>&&)>&& callback) final
    {
        callback(std::nullopt);
    }

    void updateBackgroundFetchRegistration(const BackgroundFetchInformation&) final { }
    void contextConnectionCreated(SWServerToContextConnection&) final { }

    HashMap<ServiceWorkerJobIdentifier, CompletionHandler<void(ExceptionOr<bool>&&)>> m_unregisterJobs;
    HashMap<ScriptExecutionContextIdentifier, ClientOrigin> m_clientOrigins;
};

// ---------------------------------------------------------------------------------------------------------------
// ClientConnection: pages -> server.

void ClientConnection::matchRegistration(SecurityOriginData&& topOrigin, const URL& clientURL, RegistrationCallback&& callback)
{
    if (!mayHaveServiceWorkerRegisteredForOrigin(topOrigin))
        return callback(std::nullopt);
    later([topOrigin = WTF::move(topOrigin), clientURL, callback = WTF::move(callback)]() mutable {
        s_serverConnection->doRegistrationMatching(topOrigin, clientURL, WTF::move(callback));
    });
}

void ClientConnection::getRegistrations(SecurityOriginData&& topOrigin, const URL& clientURL, GetRegistrationsCallback&& callback)
{
    if (!mayHaveServiceWorkerRegisteredForOrigin(topOrigin))
        return callback({ });
    later([topOrigin = WTF::move(topOrigin), clientURL, callback = WTF::move(callback)]() mutable {
        s_server->getRegistrations(topOrigin, clientURL, WTF::move(callback));
    });
}

void ClientConnection::whenRegistrationReady(const SecurityOriginData& topOrigin, const URL& clientURL, WhenRegistrationReadyCallback&& callback)
{
    later([topOrigin, clientURL, callback = WTF::move(callback)]() mutable {
        s_serverConnection->whenReady(topOrigin, clientURL, [callback = WTF::move(callback)](auto&& data) mutable {
            if (data)
                callback(WTF::move(*data));
        });
    });
}

void ClientConnection::addServiceWorkerRegistrationInServer(ServiceWorkerRegistrationIdentifier identifier)
{
    later([identifier] { s_serverConnection->addRegistration(identifier); });
}

void ClientConnection::removeServiceWorkerRegistrationInServer(ServiceWorkerRegistrationIdentifier identifier)
{
    later([identifier] { s_serverConnection->removeRegistration(identifier); });
}

void ClientConnection::scheduleUnregisterJobInServer(ServiceWorkerRegistrationIdentifier registrationIdentifier, ServiceWorkerOrClientIdentifier contextIdentifier, CompletionHandler<void(ExceptionOr<bool>&&)>&& callback)
{
    later([registrationIdentifier, contextIdentifier, callback = WTF::move(callback)]() mutable {
        s_serverConnection->scheduleUnregisterJob(registrationIdentifier, contextIdentifier, WTF::move(callback));
    });
}

void ClientConnection::registerServiceWorkerInServer(ServiceWorkerIdentifier identifier)
{
    later([identifier] { s_serverConnection->registerWorker(identifier); });
}

void ClientConnection::unregisterServiceWorkerInServer(ServiceWorkerIdentifier identifier)
{
    later([identifier] { s_serverConnection->unregisterWorker(identifier); });
}

void ClientConnection::didResolveRegistrationPromise(const ServiceWorkerRegistrationKey& key)
{
    later([key] { s_serverConnection->didResolveRegistrationPromise(key); });
}

void ClientConnection::postMessageToServiceWorker(ServiceWorkerIdentifier destination, MessageWithMessagePorts&& message, const ServiceWorkerOrClientIdentifier& source)
{
    later([destination, message = WTF::move(message), source]() mutable {
        RefPtr destinationWorker = s_server->workerByID(destination);
        if (!destinationWorker)
            return;

        std::optional<ServiceWorkerOrClientData> sourceData;
        WTF::switchOn(source, [&](ServiceWorkerIdentifier identifier) {
            if (RefPtr sourceWorker = s_server->workerByID(identifier))
                sourceData = ServiceWorkerOrClientData { sourceWorker->data() };
        }, [&](ScriptExecutionContextIdentifier identifier) {
            if (auto clientData = destinationWorker->findClientByIdentifier(identifier))
                sourceData = ServiceWorkerOrClientData { *clientData };
        });
        if (!sourceData)
            return;

        // The worker may have been stopped for being idle: it is started again for the message.
        s_server->runServiceWorkerIfNecessary(destination, [destination, message = WTF::move(message), sourceData = WTF::move(*sourceData)](auto* contextConnection) mutable {
            if (!contextConnection)
                return;
            if (RefPtr proxy = SWContextManager::singleton().serviceWorkerThreadProxy(destination))
                proxy->fireMessageEvent(WTF::move(message), WTF::move(sourceData));
        });
    });
}

SWServerConnectionIdentifier ClientConnection::serverConnectionIdentifier() const
{
    return s_serverConnection->identifier();
}

bool ClientConnection::mayHaveServiceWorkerRegisteredForOrigin(const SecurityOriginData& origin) const
{
    return s_originStore->mayContain(origin);
}

void ClientConnection::registerServiceWorkerClient(const ClientOrigin& clientOrigin, ServiceWorkerClientData&& data, const std::optional<ServiceWorkerRegistrationIdentifier>& controllingRegistration, String&& userAgent)
{
    later([clientOrigin = ClientOrigin { clientOrigin }, data = WTF::move(data), controllingRegistration, userAgent = WTF::move(userAgent)]() mutable {
        s_serverConnection->registerClient(WTF::move(clientOrigin), WTF::move(data), controllingRegistration, WTF::move(userAgent), SWServer::IsBeingCreatedClient::No);
    });
}

void ClientConnection::unregisterServiceWorkerClient(ScriptExecutionContextIdentifier identifier)
{
    later([identifier] { s_serverConnection->unregisterClient(identifier); });
}

void ClientConnection::finishFetchingScriptInServer(const ServiceWorkerJobDataIdentifier& identifier, ServiceWorkerRegistrationKey&& key, WorkerFetchResult&& result)
{
    later([identifier, key = WTF::move(key), result = WTF::move(result)]() mutable {
        s_serverConnection->finishFetchingScript(identifier, key, WTF::move(result));
    });
}

void ClientConnection::scheduleJobInServer(const ServiceWorkerJobData& jobData)
{
    later([jobData = ServiceWorkerJobData { jobData }]() mutable {
        if (jobData.scopeURL.isNull()) {
            s_clientConnection->rejectJob(jobData.identifier().jobIdentifier, ExceptionData { ExceptionCode::InvalidStateError, "Scope URL is empty"_s });
            return;
        }
        s_server->scheduleJob(WTF::move(jobData));
    });
}

// Navigation preload: the state is kept as a page sets it; FetchTask::sendToWorker makes the preload request
// (NavigationPreloader).
template<typename Callback, typename Operation>
void withRegistration(ServiceWorkerRegistrationIdentifier identifier, Callback&& callback, Operation&& operation)
{
    later([identifier, callback = WTF::move(callback), operation = WTF::move(operation)]() mutable {
        RefPtr registration = s_server->getRegistration(identifier);
        if (!registration)
            return callback(Exception { ExceptionCode::InvalidStateError, "No registration"_s });
        operation(*registration, WTF::move(callback));
    });
}

void ClientConnection::enableNavigationPreload(ServiceWorkerRegistrationIdentifier identifier, ExceptionOrVoidCallback&& callback)
{
    withRegistration(identifier, WTF::move(callback), [](SWServerRegistration& registration, ExceptionOrVoidCallback&& callback) {
        if (auto error = registration.enableNavigationPreload())
            return callback(error->toException());
        callback({ });
    });
}

void ClientConnection::disableNavigationPreload(ServiceWorkerRegistrationIdentifier identifier, ExceptionOrVoidCallback&& callback)
{
    withRegistration(identifier, WTF::move(callback), [](SWServerRegistration& registration, ExceptionOrVoidCallback&& callback) {
        if (auto error = registration.disableNavigationPreload())
            return callback(error->toException());
        callback({ });
    });
}

void ClientConnection::setNavigationPreloadHeaderValue(ServiceWorkerRegistrationIdentifier identifier, String&& value, ExceptionOrVoidCallback&& callback)
{
    withRegistration(identifier, WTF::move(callback), [value = WTF::move(value)](SWServerRegistration& registration, ExceptionOrVoidCallback&& callback) mutable {
        if (auto error = registration.setNavigationPreloadHeaderValue(WTF::move(value)))
            return callback(error->toException());
        callback({ });
    });
}

void ClientConnection::getNavigationPreloadState(ServiceWorkerRegistrationIdentifier identifier, ExceptionOrNavigationPreloadStateCallback&& callback)
{
    withRegistration(identifier, WTF::move(callback), [](SWServerRegistration& registration, ExceptionOrNavigationPreloadStateCallback&& callback) {
        callback(NavigationPreloadState { registration.navigationPreloadState() });
    });
}

auto ClientConnection::addRoutes(ServiceWorkerRegistrationIdentifier identifier, Vector<ServiceWorkerRoute>&& routes) -> Ref<AddRoutePromise>
{
    AddRoutePromise::Producer producer;
    Ref promise = producer.promise();
    later([identifier, routes = WTF::move(routes), producer = WTF::move(producer)]() mutable {
        s_server->addRoutes(identifier, WTF::move(routes), [producer = WTF::move(producer)](auto&& result) mutable {
            producer.settle(WTF::move(result));
        });
    });
    return promise;
}

class Provider final : public ServiceWorkerProvider {
private:
    SWClientConnection& serviceWorkerConnection() final { return *s_clientConnection; }
    SWClientConnection* existingServiceWorkerConnection() final { return s_clientConnection; }
    void terminateWorkerForTesting(ServiceWorkerIdentifier, CompletionHandler<void()>&& callback) final { callback(); }
};

// ---------------------------------------------------------------------------------------------------------------
// Workers -> server.

class ContextManagerConnection final : public SWContextManager::Connection, public ThreadSafeRefCounted<ContextManagerConnection> {
public:
    static Ref<ContextManagerConnection> create() { return adoptRef(*new ContextManagerConnection); }

    void ref() const final { ThreadSafeRefCounted::ref(); }
    void deref() const final { ThreadSafeRefCounted::deref(); }

private:
    ContextManagerConnection() = default;

    void establishConnection(CompletionHandler<void()>&& completionHandler) final { completionHandler(); }

    void postMessageToServiceWorkerClient(const ScriptExecutionContextIdentifier& destination, const MessageWithMessagePorts& message, ServiceWorkerIdentifier source, const SecurityOriginData& sourceOrigin) final
    {
        onWebThread([destination, message = MessageWithMessagePorts { message }, source, sourceOrigin = sourceOrigin.isolatedCopy()] {
            static_cast<SWServer::Connection&>(*s_serverConnection).postMessageToServiceWorkerClient(destination, message, source, sourceOrigin);
        });
    }

    void serviceWorkerStarted(std::optional<ServiceWorkerJobDataIdentifier> jobDataIdentifier, ServiceWorkerIdentifier identifier, bool doesHandleFetch) final
    {
        onWebThread([jobDataIdentifier, identifier, doesHandleFetch] {
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->scriptContextStarted(jobDataIdentifier, identifier, doesHandleFetch);
        });
    }

    void serviceWorkerFailedToStart(std::optional<ServiceWorkerJobDataIdentifier> jobDataIdentifier, ServiceWorkerIdentifier identifier, const String& message) final
    {
        onWebThread([jobDataIdentifier, identifier, message = message.isolatedCopy()] {
            log("sw: worker %llu non parte: %s", static_cast<unsigned long long>(identifier.toUInt64()), message.utf8().data());
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->scriptContextFailedToStart(jobDataIdentifier, identifier, message);
        });
    }

    void didFinishInstall(std::optional<ServiceWorkerJobDataIdentifier> jobDataIdentifier, ServiceWorkerIdentifier identifier, bool wasSuccessful) final
    {
        onWebThread([jobDataIdentifier, identifier, wasSuccessful] {
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->didFinishInstall(jobDataIdentifier, identifier, wasSuccessful);
        });
    }

    void didFinishActivation(ServiceWorkerIdentifier identifier) final
    {
        onWebThread([identifier] {
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->didFinishActivation(identifier);
        });
    }

    void setServiceWorkerHasPendingEvents(ServiceWorkerIdentifier identifier, bool hasPendingEvents) final
    {
        onWebThread([identifier, hasPendingEvents] {
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->setServiceWorkerHasPendingEvents(identifier, hasPendingEvents);
        });
    }

    void workerTerminated(ServiceWorkerIdentifier identifier) final
    {
        onWebThread([identifier] {
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->workerTerminated(identifier);
        });
    }

    void skipWaiting(ServiceWorkerIdentifier identifier, CompletionHandler<void()>&& callback) final
    {
        onWebThread([identifier, callback = WTF::move(callback)]() mutable {
            if (RefPtr worker = SWServerWorker::existingWorkerForIdentifier(identifier))
                worker->skipWaiting();
            callback();
        });
    }

    void setScriptResource(ServiceWorkerIdentifier identifier, const URL& url, const ServiceWorkerContextData::ImportedScript& script) final
    {
        onWebThread([identifier, url = url.isolatedCopy(), script = script.isolatedCopy()]() mutable {
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->setScriptResource(identifier, WTF::move(url), WTF::move(script));
        });
    }

    void findClientByVisibleIdentifier(ServiceWorkerIdentifier identifier, const String& clientIdentifier, FindClientByIdentifierCallback&& callback) final
    {
        onWebThread([identifier, clientIdentifier = clientIdentifier.isolatedCopy(), callback = WTF::move(callback)]() mutable {
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->findClientByVisibleIdentifier(identifier, clientIdentifier, WTF::move(callback));
            else
                callback(std::nullopt);
        });
    }

    void matchAll(ServiceWorkerIdentifier identifier, const ServiceWorkerClientQueryOptions& options, ServiceWorkerClientsMatchAllCallback&& callback) final
    {
        onWebThread([identifier, options, callback = WTF::move(callback)]() mutable {
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->matchAll(identifier, options, WTF::move(callback));
            else
                callback({ });
        });
    }

    void claim(ServiceWorkerIdentifier identifier, CompletionHandler<void(ExceptionOr<void>&&)>&& callback) final
    {
        onWebThread([identifier, callback = WTF::move(callback)]() mutable {
            RefPtr connection = contextConnectionOf(identifier);
            if (!connection)
                return callback(Exception { ExceptionCode::InvalidStateError, "The worker is gone"_s });
            connection->claim(identifier, [callback = WTF::move(callback)](auto&& error) mutable {
                callback(error ? ExceptionOr<void> { error->toException() } : ExceptionOr<void> { });
            });
        });
    }

    void focus(ScriptExecutionContextIdentifier, CompletionHandler<void(std::optional<ServiceWorkerClientData>&&)>&& callback) final
    {
        onWebThread([callback = WTF::move(callback)]() mutable { callback(std::nullopt); });
    }

    void openWindow(ServiceWorkerIdentifier, const URL&, OpenWindowCallback&& callback) final
    {
        onWebThread([callback = WTF::move(callback)]() mutable { callback(notSupported("clients.openWindow"_s).toException()); });
    }

    void navigate(ScriptExecutionContextIdentifier, ServiceWorkerIdentifier, const URL&, NavigateCallback&& callback) final
    {
        onWebThread([callback = WTF::move(callback)]() mutable { callback(notSupported("WindowClient.navigate"_s).toException()); });
    }

    void didFailHeartBeatCheck(ServiceWorkerIdentifier identifier) final
    {
        onWebThread([identifier] {
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->didFailHeartBeatCheck(identifier);
        });
    }

    void setAsInspected(ServiceWorkerIdentifier identifier, bool isInspected) final
    {
        onWebThread([identifier, isInspected] {
            if (RefPtr connection = contextConnectionOf(identifier))
                connection->setAsInspected(identifier, isInspected);
        });
    }

    bool isThrottleable() const final { return false; }
    PageIdentifier pageIdentifier() const final { return m_pageIdentifier; }
    void stop() final { }

    void reportConsoleMessage(ServiceWorkerIdentifier identifier, MessageSource, MessageLevel level, const String& message, unsigned long) final
    {
        if (level != MessageLevel::Error && level != MessageLevel::Warning)
            return;
        log("sw: worker %llu: %s", static_cast<unsigned long long>(identifier.toUInt64()), message.utf8().data());
    }

    void removeNavigationFetch(SWServerConnectionIdentifier, FetchIdentifier) final { }

    const PageIdentifier m_pageIdentifier { PageIdentifier::generate() };
};

// ---------------------------------------------------------------------------------------------------------------
// The server's delegate.

class Delegate final : public SWServerDelegate, public CanMakeCheckedPtr<Delegate> {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(Delegate);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(Delegate);
public:
    uint32_t checkedPtrCount() const final { return CanMakeCheckedPtr::checkedPtrCount(); }
    uint32_t checkedPtrCountWithoutThreadCheck() const final { return CanMakeCheckedPtr::checkedPtrCountWithoutThreadCheck(); }
    void incrementCheckedPtrCount() const final { CanMakeCheckedPtr::incrementCheckedPtrCount(); }
    void decrementCheckedPtrCount() const final { CanMakeCheckedPtr::decrementCheckedPtrCount(); }
    void setDidBeginCheckedPtrDeletion() final { CanMakeCheckedPtr::setDidBeginCheckedPtrDeletion(); }

private:
    void softUpdate(ServiceWorkerJobData&& jobData, bool shouldRefreshCache, ResourceRequest&& request, CompletionHandler<void(WorkerFetchResult&&)>&& completionHandler) final
    {
        SoftUpdateLoader::start(WTF::move(jobData), shouldRefreshCache, WTF::move(request), WTF::move(completionHandler));
    }

    void createContextConnection(const Site& site, std::optional<ProcessIdentifier>, std::optional<ScriptExecutionContextIdentifier> serviceWorkerPageIdentifier, CrossOriginEmbedderPolicyValue coep, CompletionHandler<void()>&& completionHandler) final
    {
        // The server expects the connection later, as from a process it had to start.
        later([site, serviceWorkerPageIdentifier, coep, completionHandler = WTF::move(completionHandler)]() mutable {
            Ref connection = ContextConnection::create(*s_server, Site { site }, serviceWorkerPageIdentifier);
            connection->setCrossOriginEmbedderPolicyValue(coep);
            ContextConnection::all().add(connection->identifier(), connection.copyRef());
            s_server->addContextConnection(connection);
            completionHandler();
        });
    }

    void appBoundDomains(CompletionHandler<void(HashSet<RegistrableDomain>&&)>&& completionHandler) final { completionHandler({ }); }
    void addAllowedFirstPartyForCookies(ProcessIdentifier, std::optional<ProcessIdentifier>, RegistrableDomain&&) final { }
    void requestBackgroundFetchPermission(const ClientOrigin&, CompletionHandler<void(bool)>&& completionHandler) final { completionHandler(false); }
    RefPtr<BackgroundFetchRecordLoader> createBackgroundFetchRecordLoader(BackgroundFetchRecordLoaderClient&, const BackgroundFetchRequest&, size_t, const ClientOrigin&) final { return nullptr; }
    Ref<BackgroundFetchStore> createBackgroundFetchStore() final { return NoBackgroundFetchStore::create(); }
    RefPtr<SWRegistrationStore> createRegistrationStore(SWServer& server) final { return RegistrationStore::create(server); }
};

// ---------------------------------------------------------------------------------------------------------------
// One load of a page answered by a worker: WebKit2's ServiceWorkerFetchTask (network process) and
// WebServiceWorkerFetchTaskClient (worker process) in one. Web thread, except FetchClient's entry points.

HashMap<FetchIdentifier, Ref<FetchTask>>& fetchTasks()
{
    static NeverDestroyed<HashMap<FetchIdentifier, Ref<FetchTask>>> map;
    return map;
}

HashMap<ResourceLoader*, FetchIdentifier>& fetchTasksByLoader()
{
    static NeverDestroyed<HashMap<ResourceLoader*, FetchIdentifier>> map;
    return map;
}

// https://w3c.github.io/ServiceWorker/#navigation-preload, as WebKit2's ServiceWorkerNavigationPreloader: when the
// registration has it on, a navigation goes to the network as soon as it reaches the worker, its request carrying the
// Service-Worker-Navigation-Preload header, while the worker starts and runs its fetch event. The response is the
// event's preloadResponse. Its body is kept here as it arrives and goes to whoever takes it: the worker's own load of
// it (WebCore loads the body of a preloadResponse through a load that carries the preload's identifier, the
// navigation's FetchIdentifier: see interceptLoad), or the page, when the worker answers with the preload unread
// (respondWith(event.preloadResponse): usePreload). A redirect is the answer itself (the preload does not follow it).
class NavigationPreloader final : public RefCounted<NavigationPreloader>, public CanMakeWeakPtr<NavigationPreloader>, public ResourceHandleClient {
public:
    static HashMap<FetchIdentifier, Ref<NavigationPreloader>>& all()
    {
        static NeverDestroyed<HashMap<FetchIdentifier, Ref<NavigationPreloader>>> map;
        return map;
    }

    // Response and failure go to the worker (FetchEvent.preloadResponse); null when the load could not start.
    static RefPtr<NavigationPreloader> start(FetchIdentifier identifier, ResourceRequest&& request, const String& headerValue, Function<void(ResourceResponse&&)>&& didReceiveResponse, Function<void(ResourceError&&)>&& didFail)
    {
        Ref preloader = adoptRef(*new NavigationPreloader(identifier, WTF::move(didReceiveResponse), WTF::move(didFail)));
        request.setHTTPHeaderField("Service-Worker-Navigation-Preload"_s, headerValue);
        preloader->m_context = ServerNetworkingContext::create();
        preloader->m_handle = ResourceHandle::create(preloader->m_context.get(), request, preloader.ptr(), false, false, ContentEncodingSniffingPolicy::Default, nullptr, false);
        if (!preloader->m_handle)
            return nullptr;
        all().set(identifier, preloader.copyRef());
        log("sw: navigazione %s precaricata per il worker", request.url().string().utf8().data());
        return preloader;
    }

    bool isTaken() const { return m_taken; }

    // The response, the data, then the end go to the task that takes the preload (FetchTask::preload*): what came so
    // far at once, the rest as it comes.
    void take(FetchTask&);

    // The taker went away before the end, or the navigation ended without anyone wanting the body.
    void cancel();

    ~NavigationPreloader();

    // The navigation's fetch is over and nobody has taken the body yet: the worker still may (it loads the body of a
    // preloadResponse a task after resolving it), for a while.
    void navigationEnded()
    {
        if (!m_taken)
            m_untakenTimer.startOneShot(30_s);
    }

private:
    NavigationPreloader(FetchIdentifier identifier, Function<void(ResourceResponse&&)>&& didReceiveResponse, Function<void(ResourceError&&)>&& didFail)
        : m_identifier(identifier)
        , m_didReceiveResponse(WTF::move(didReceiveResponse))
        , m_didFail(WTF::move(didFail))
        , m_untakenTimer(*this, &NavigationPreloader::cancel)
    {
    }

    void remove()
    {
        m_untakenTimer.stop();
        auto iterator = all().find(m_identifier);
        if (iterator != all().end() && iterator->value.ptr() == this)
            all().remove(iterator); // May delete this.
    }

    void gotResponse(ResourceResponse&& response)
    {
        m_response = response;
        if (auto callback = std::exchange(m_didReceiveResponse, nullptr))
            callback(WTF::move(response));
        m_didFail = nullptr;
        if (m_taken)
            tellResponse();
    }

    void tellResponse();
    void tellData(Ref<const SharedBuffer>&&);
    void tellEnd();

    void willSendRequestAsync(ResourceHandle*, ResourceRequest&&, ResourceResponse&& redirectResponse, CompletionHandler<void(ResourceRequest&&)>&& completionHandler) final
    {
        Ref protectedThis { *this };
        completionHandler({ });
        if (RefPtr handle = std::exchange(m_handle, nullptr)) {
            handle->clearClient();
            handle->cancel();
        }
        gotResponse(WTF::move(redirectResponse));
        didFinishLoading(nullptr, { });
    }

    void didReceiveResponseAsync(ResourceHandle*, ResourceResponse&& response, CompletionHandler<void()>&& completionHandler) final
    {
        Ref protectedThis { *this };
        gotResponse(WTF::move(response));
        completionHandler();
    }

    void didReceiveData(ResourceHandle*, const SharedBuffer& buffer, int) final
    {
        // A copy: the bytes of a curl callback are not ours after it returns.
        Ref<const SharedBuffer> copy = SharedBuffer::create(buffer.span());
        if (m_taken)
            tellData(WTF::move(copy));
        else
            m_buffers.append(WTF::move(copy));
    }

    void didFinishLoading(ResourceHandle*, const NetworkLoadMetrics& metrics) final
    {
        Ref protectedThis { *this };
        m_finished = true;
        m_metrics = metrics;
        m_handle = nullptr;
        if (m_taken)
            tellEnd();
    }

    void didFail(ResourceHandle*, const ResourceError& error) final
    {
        Ref protectedThis { *this };
        m_error = error;
        m_handle = nullptr;
        if (auto callback = std::exchange(m_didFail, nullptr))
            callback(ResourceError { error });
        m_didReceiveResponse = nullptr;
        if (m_taken)
            tellEnd();
    }

#if USE(PROTECTION_SPACE_AUTH_CALLBACK)
    void canAuthenticateAgainstProtectionSpaceAsync(ResourceHandle*, const ProtectionSpace&, CompletionHandler<void(bool)>&& completionHandler) final { completionHandler(false); }
#endif

    const FetchIdentifier m_identifier;
    Function<void(ResourceResponse&&)> m_didReceiveResponse;
    Function<void(ResourceError&&)> m_didFail;
    RefPtr<ServerNetworkingContext> m_context;
    RefPtr<ResourceHandle> m_handle;
    ResourceResponse m_response;
    Vector<Ref<const SharedBuffer>> m_buffers;
    NetworkLoadMetrics m_metrics;
    ResourceError m_error;
    bool m_finished { false };
    bool m_taken { false };
    RefPtr<FetchTask> m_taker;
    Timer m_untakenTimer;
};

class FetchTask final : public RefCounted<FetchTask>, public FetchLoaderClient {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(FetchTask);
public:
    static void start(ResourceLoader& loader)
    {
        Ref task = adoptRef(*new FetchTask(loader));
        fetchTasks().add(task->m_identifier, task.copyRef());
        fetchTasksByLoader().set(&loader, task->m_identifier);
        later([task] { task->findRegistration(); });
    }

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    // The loader was cancelled, or ended.
    void loaderRemoved()
    {
        if (m_done)
            return;
        bool wasSent = m_sentToWorker;
        finish();
        if (wasSent) {
            if (RefPtr proxy = SWContextManager::singleton().serviceWorkerThreadProxy(*m_workerIdentifier))
                proxy->cancelFetch(s_serverConnection->identifier(), m_identifier);
        }
    }

    // The worker's answers (posted to the Web thread by FetchClient).
    void workerSentRedirection(ResourceResponse&& response)
    {
        if (m_done)
            return;
        handled();
        response.setSource(ResourceResponse::Source::ServiceWorker);
        RefPtr loader = m_loader;
        auto newRequest = loader->request().redirectedRequest(response, true, ResourceRequest::ShouldSetHash::Yes);
        // The redirected request is a new load for the loader: it asks the interceptor again.
        finish();
        loader->willSendRequest(WTF::move(newRequest), response, [loader](ResourceRequest&& request) {
            if (request.isNull() || loader->reachedTerminalState())
                return;
            loader->start();
        });
    }

    void workerSentResponse(ResourceResponse&& response)
    {
        if (m_done)
            return;
        handled();
        response.setSource(ResourceResponse::Source::ServiceWorker);
        // The loader may ask a navigation policy before it takes the body: what the worker sends meanwhile waits.
        m_waitingForResponsePolicy = true;
        RefPtr loader = m_loader;
        loader->didReceiveResponse(WTF::move(response), [task = Ref { *this }] {
            task->m_waitingForResponsePolicy = false;
            auto pending = std::exchange(task->m_pending, { });
            for (auto& event : pending) {
                if (task->m_done)
                    break;
                event();
            }
        });
    }

    void workerSentData(Ref<const SharedBuffer>&& buffer)
    {
        whenResponseIsTaken([this, buffer = WTF::move(buffer)] {
            RefPtr loader = m_loader;
            loader->didReceiveData(buffer, buffer->size(), DataPayloadType::DataPayloadBytes);
        });
    }

    void workerSentFormData(Ref<FormData>&& formData)
    {
        whenResponseIsTaken([this, formData = WTF::move(formData)]() mutable {
            if (RefPtr buffer = formData->asSharedBuffer()) {
                RefPtr loader = m_loader;
                if (buffer->size())
                    loader->didReceiveData(*buffer, buffer->size(), DataPayloadType::DataPayloadBytes);
                if (!m_done)
                    workerFinished({ });
                return;
            }
            // A body that is a Blob (a response taken from Cache storage, mostly): read through the worker's loader.
            auto blobURL = formData->asBlobURL();
            RefPtr proxy = SWContextManager::singleton().serviceWorkerThreadProxy(*m_workerIdentifier);
            if (blobURL.isNull() || !proxy)
                return workerFailed(ResourceError { errorDomainWebKitInternal, 0, m_loader->url(), "The worker's response body cannot be read"_s });
            m_blobLoader = proxy->createBlobLoader(*this, blobURL);
            if (!m_blobLoader)
                workerFailed(ResourceError { errorDomainWebKitInternal, 0, m_loader->url(), "The worker's response body cannot be read"_s });
        });
    }

    void workerFailed(ResourceError&& error)
    {
        whenResponseIsTaken([this, error = WTF::move(error)] {
            RefPtr loader = m_loader;
            finish();
            loader->didFail(error);
        });
    }

    void workerFinished(NetworkLoadMetrics&& metrics)
    {
        whenResponseIsTaken([this, metrics = WTF::move(metrics)] {
            RefPtr loader = m_loader;
            finish();
            loader->didFinishLoading(metrics);
        });
    }

    // No fetch listener, or it did not call respondWith: the load goes to the network as if no worker existed.
    void workerDidNotHandle()
    {
        if (m_done)
            return;
        RefPtr loader = m_loader;
        finish();
        loader->startWithoutInterceptionForWP81();
    }

    void workerIsStopping()
    {
        if (m_done)
            return;
        if (!m_handled)
            return workerDidNotHandle();
        workerFailed(ResourceError { errorDomainWebKitInternal, 0, m_loader->url(), "The service worker was stopped"_s });
    }

    // The worker answered with the navigation preload unread (respondWith(event.preloadResponse)): it goes to the
    // page as it comes from the network.
    void workerUsedPreload()
    {
        if (m_done)
            return;
        RefPtr preloader = NavigationPreloader::all().get(m_identifier);
        if (!preloader || preloader->isTaken())
            return workerDidNotHandle();
        handled();
        m_takenPreloader = preloader;
        preloader->take(*this);
    }

    // The worker's own load of the body of a preloadResponse (interceptLoad).
    static void serveFromPreload(ResourceLoader& loader, NavigationPreloader& preloader)
    {
        Ref task = adoptRef(*new FetchTask(loader));
        fetchTasks().add(task->m_identifier, task.copyRef());
        fetchTasksByLoader().set(&loader, task->m_identifier);
        task->m_handled = true;
        task->m_takenPreloader = &preloader;
        preloader.take(task);
    }

    // What NavigationPreloader tells its taker.
    void preloadSentResponse(const ResourceResponse& response)
    {
        if (response.isRedirection() && response.httpHeaderFields().contains(HTTPHeaderName::Location))
            workerSentRedirection(ResourceResponse { response });
        else
            workerSentResponse(ResourceResponse { response });
    }
    void preloadSentData(Ref<const SharedBuffer>&& buffer) { workerSentData(WTF::move(buffer)); }
    void preloadFinished(const NetworkLoadMetrics& metrics) { workerFinished(NetworkLoadMetrics { metrics }); }
    void preloadFailed(const ResourceError& error) { workerFailed(ResourceError { error }); }

private:
    explicit FetchTask(ResourceLoader& loader)
        : m_loader(&loader)
        , m_identifier(FetchIdentifier::generate())
        , m_timeoutTimer(*this, &FetchTask::timedOut)
    {
    }

    // FetchLoaderClient: the Blob body.
    void didReceiveData(const SharedBuffer& buffer) final
    {
        if (m_done)
            return;
        RefPtr loader = m_loader;
        loader->didReceiveData(buffer, buffer.size(), DataPayloadType::DataPayloadBytes);
    }
    void didSucceed(const NetworkLoadMetrics& metrics) final { workerFinished(NetworkLoadMetrics { metrics }); }
    void didFail(const ResourceError& error) final { workerFailed(ResourceError { error }); }

    void whenResponseIsTaken(Function<void()>&& event)
    {
        if (m_done)
            return;
        if (m_waitingForResponsePolicy)
            m_pending.append(WTF::move(event));
        else
            event();
    }

    void handled()
    {
        m_handled = true;
        m_timeoutTimer.stop();
        if (RefPtr registration = m_registrationIdentifier ? s_server->getRegistration(*m_registrationIdentifier) : nullptr) {
            if (registration->shouldSoftUpdate(m_loader->options()))
                registration->scheduleSoftUpdate(IsAppInitiated::No);
        }
    }

    void finish()
    {
        if (m_done)
            return;
        m_done = true;
        m_timeoutTimer.stop();
        m_pending.clear();
        m_blobLoader = nullptr;
        // A preload this task took and that is not over: nobody else wants it.
        if (RefPtr preloader = std::exchange(m_takenPreloader, nullptr))
            preloader->cancel();
        // The navigation's own preload, untaken: the worker may still load its body for a while.
        if (RefPtr preloader = NavigationPreloader::all().get(m_identifier))
            preloader->navigationEnded();
        if (m_sentToWorker)
            SWContextManager::singleton().removeFetch(*m_workerIdentifier, s_serverConnection->identifier(), m_identifier, m_loader->options().mode == FetchOptions::Mode::Navigate);
        auto iterator = fetchTasksByLoader().find(m_loader.get());
        if (iterator != fetchTasksByLoader().end() && iterator->value == m_identifier)
            fetchTasksByLoader().remove(iterator);
        m_loader = nullptr;
        fetchTasks().remove(m_identifier); // May delete this.
    }

    void timedOut()
    {
        log("sw: il worker %llu non risponde a una richiesta, va alla rete", static_cast<unsigned long long>(m_workerIdentifier->toUInt64()));
        Ref protectedThis { *this };
        bool wasSent = m_sentToWorker;
        auto workerIdentifier = *m_workerIdentifier;
        auto identifier = m_identifier;
        workerDidNotHandle();
        if (wasSent) {
            if (RefPtr proxy = SWContextManager::singleton().serviceWorkerThreadProxy(workerIdentifier))
                proxy->cancelFetch(s_serverConnection->identifier(), identifier);
        }
    }

    SecurityOriginData topOrigin() const
    {
        RefPtr frame = m_loader->frame();
        if (frame && frame->isMainFrame() && m_loader->options().mode == FetchOptions::Mode::Navigate)
            return SecurityOriginData::fromURLWithoutStrictOpaqueness(m_loader->request().url());
        if (RefPtr document = frame ? frame->document() : nullptr)
            return document->topOrigin().data();
        return SecurityOriginData::fromURLWithoutStrictOpaqueness(m_loader->request().url());
    }

    // Step 1: which registration. A navigation or a worker's script picks it by scope (and becomes its client); a
    // subresource has the one that controls its document.
    void findRegistration()
    {
        if (m_done)
            return;
        auto& options = m_loader->options();
        if (!options.resultingClientIdentifier) {
            continueWithRegistration(s_server->getRegistration(*options.serviceWorkerRegistrationIdentifier));
            return;
        }
        auto topOrigin = this->topOrigin();
        auto match = [task = Ref { *this }, topOrigin] {
            if (task->m_done)
                return;
            RefPtr registration = s_server->doRegistrationMatchingSync(topOrigin, task->m_loader->request().url());
            if (registration)
                task->controlClient(*registration);
            task->continueWithRegistration(registration.get());
        };
        // Registrations are read from disk one top origin at a time, the first time one is asked for.
        if (s_server->isImportCompletedForOrigin(topOrigin))
            match();
        else
            s_server->importRegistrationsForOrigin(topOrigin, WTF::move(match));
    }

    // https://w3c.github.io/ServiceWorker/#on-fetch-request-algorithm, step 12: the document (or worker) this load
    // creates is controlled by the registration from now, if its loader agrees.
    void controlClient(SWServerRegistration& registration)
    {
        auto& options = m_loader->options();
        auto& request = m_loader->request();
        ScriptExecutionContextIdentifier clientIdentifier { *options.resultingClientIdentifier, Process::identifier() };

        bool controlled = false;
        if (RefPtr documentLoader = DocumentLoader::fromScriptExecutionContextIdentifier(clientIdentifier))
            controlled = documentLoader->setControllingServiceWorkerRegistration(registration.data());
        else if (RefPtr manager = WorkerScriptLoader::serviceWorkerDataManagerFromIdentifier(clientIdentifier)) {
            auto data = registration.data();
            if (data.activeWorker) {
                manager->setData(WTF::move(*data.activeWorker));
                controlled = true;
            }
        }
        if (!controlled)
            return;

        ServiceWorkerClientType clientType = ServiceWorkerClientType::Window;
        if (options.destination == FetchOptions::Destination::Worker)
            clientType = ServiceWorkerClientType::Worker;
        else if (options.destination == FetchOptions::Destination::Sharedworker)
            clientType = ServiceWorkerClientType::Sharedworker;

        RefPtr frame = m_loader->frame();
        std::optional<PageIdentifier> pageIdentifier;
        if (RefPtr page = frame ? frame->page() : nullptr)
            pageIdentifier = page->identifier();
        ServiceWorkerClientData data { clientIdentifier, clientType, ServiceWorkerClientFrameType::None, request.url(), URL(), pageIdentifier, frame ? std::optional { frame->frameID() } : std::nullopt, LastNavigationWasAppInitiated::No, { }, false, false, 0, { } };
        s_serverConnection->registerClient(ClientOrigin { registration.key().topOrigin(), SecurityOriginData::fromURLWithoutStrictOpaqueness(request.url()) }, WTF::move(data), registration.identifier(), request.httpUserAgent(), SWServer::IsBeingCreatedClient::Yes);
    }

    // Step 2: its active worker, running.
    void continueWithRegistration(SWServerRegistration* registration)
    {
        RefPtr worker = registration ? registration->activeWorker() : nullptr;
        if (!worker || worker->hasTimedOutAnyFetchTasks())
            return workerDidNotHandle();
        m_registrationIdentifier = registration->identifier();

        // Static routes: "network" skips the worker. A "cache" route and the race are not implemented apart: the
        // worker's fetch event answers them (a route is an optimisation, the handler must give the same result).
        auto& options = m_loader->options();
        auto routerSource = worker->getRouterSource(options, m_loader->request());
        if (routerSource) {
            if (auto* source = std::get_if<RouterSourceEnum>(&*routerSource); source && *source == RouterSourceEnum::Network) {
                if (registration->shouldSoftUpdate(options))
                    registration->scheduleSoftUpdate(IsAppInitiated::No);
                return workerDidNotHandle();
            }
        }

        m_workerIdentifier = worker->identifier();
        m_timeoutTimer.startOneShot(60_s);
        worker->whenActivated([task = Ref { *this }](bool success) {
            if (task->m_done)
                return;
            RefPtr worker = success ? s_server->workerByID(*task->m_workerIdentifier) : nullptr;
            if (!worker)
                return task->workerDidNotHandle();
            if (!worker->contextConnection())
                s_server->createContextConnection(worker->topSite(), worker->serviceWorkerPageIdentifier(), worker->crossOriginEmbedderPolicy().value);
            s_server->runServiceWorkerIfNecessary(*task->m_workerIdentifier, [task](auto* contextConnection) {
                if (task->m_done)
                    return;
                if (!contextConnection)
                    return task->workerDidNotHandle();
                task->sendToWorker();
            });
        });
    }

    // Step 3: the fetch event.
    void sendToWorker();

    RefPtr<ResourceLoader> m_loader;
    const FetchIdentifier m_identifier;
    Markable<ServiceWorkerIdentifier> m_workerIdentifier;
    std::optional<ServiceWorkerRegistrationIdentifier> m_registrationIdentifier;
    Timer m_timeoutTimer;
    Vector<Function<void()>> m_pending;
    RefPtr<FetchLoader> m_blobLoader;
    RefPtr<NavigationPreloader> m_takenPreloader;
    bool m_done { false };
    bool m_handled { false };
    bool m_sentToWorker { false };
    bool m_waitingForResponsePolicy { false };
};

// What the worker's thread holds of a FetchTask: every answer is posted to the Web thread.
class FetchClient final : public ServiceWorkerFetch::Client {
public:
    static Ref<FetchClient> create(FetchIdentifier identifier) { return adoptRef(*new FetchClient(identifier)); }

private:
    explicit FetchClient(FetchIdentifier identifier)
        : m_identifier(identifier)
    {
    }

    template<typename Function> void post(Function&& function)
    {
        callOnMainThread([identifier = m_identifier, function = WTF::move(function)]() mutable {
            if (RefPtr task = fetchTasks().get(identifier))
                function(*task);
        });
    }

    void didReceiveRedirection(const ResourceResponse& response) final
    {
        post([data = response.crossThreadData()](FetchTask& task) mutable { task.workerSentRedirection(ResourceResponse::fromCrossThreadData(WTF::move(data))); });
    }
    void didReceiveResponse(ResourceResponse&& response) final
    {
        post([data = response.crossThreadData()](FetchTask& task) mutable { task.workerSentResponse(ResourceResponse::fromCrossThreadData(WTF::move(data))); });
    }
    void didReceiveData(const SharedBuffer& buffer) final
    {
        post([buffer = Ref { buffer }](FetchTask& task) mutable { task.workerSentData(WTF::move(buffer)); });
    }
    void didReceiveFormDataAndFinish(Ref<FormData>&& formData) final
    {
        post([formData = formData->isolatedCopy()](FetchTask& task) mutable { task.workerSentFormData(WTF::move(formData)); });
    }
    void didFail(const ResourceError& error) final
    {
        post([error = error.isolatedCopy()](FetchTask& task) mutable { task.workerFailed(WTF::move(error)); });
    }
    void didFinish(const NetworkLoadMetrics& metrics) final
    {
        post([metrics = metrics.isolatedCopy()](FetchTask& task) mutable { task.workerFinished(WTF::move(metrics)); });
    }
    void didNotHandle() final
    {
        post([](FetchTask& task) { task.workerDidNotHandle(); });
    }
    // respondWith(event.preloadResponse), its body unread: the preload goes to the page (NavigationPreloader).
    void usePreload() final
    {
        post([](FetchTask& task) { task.workerUsedPreload(); });
    }
    void contextIsStopping() final
    {
        post([](FetchTask& task) { task.workerIsStopping(); });
    }
    void setCancelledCallback(Function<void()>&& callback) final { m_cancelledCallback = WTF::move(callback); }
    void doCancel() final
    {
        if (m_cancelledCallback)
            m_cancelledCallback();
    }

    const FetchIdentifier m_identifier;
    Function<void()> m_cancelledCallback; // Worker thread.
};

void FetchTask::sendToWorker()
{
    RefPtr proxy = SWContextManager::singleton().serviceWorkerThreadProxy(*m_workerIdentifier);
    if (!proxy)
        return workerDidNotHandle();

    auto& options = m_loader->options();
    // The request as the page made it: the headers the loader added on the way to the network are not the worker's
    // to see.
    ResourceRequest request { m_loader->request() };
    auto referrer = request.httpReferrer();
    cleanHTTPRequestHeadersForAccessControl(request, options.httpHeadersToKeep);

    String clientIdentifier;
    if (options.mode != FetchOptions::Mode::Navigate && options.clientIdentifier)
        clientIdentifier = options.clientIdentifier->toString();
    String resultingClientIdentifier;
    if (options.resultingClientIdentifier)
        resultingClientIdentifier = options.resultingClientIdentifier->toString();

    // Navigation preload, when the registration has it on (a GET navigation only, as WebKit2 makes it).
    bool preloading = false;
    if (options.mode == FetchOptions::Mode::Navigate && m_loader->request().httpMethod() == "GET"_s) {
        RefPtr registration = m_registrationIdentifier ? s_server->getRegistration(*m_registrationIdentifier) : nullptr;
        if (registration && registration->navigationPreloadState().enabled) {
            auto connection = s_serverConnection->identifier();
            auto identifier = m_identifier;
            auto workerIdentifier = *m_workerIdentifier;
            preloading = !!NavigationPreloader::start(identifier, ResourceRequest { m_loader->request() }, registration->navigationPreloadState().headerValue,
                [connection, identifier, workerIdentifier](ResourceResponse&& response) {
                    if (RefPtr proxy = SWContextManager::singleton().serviceWorkerThreadProxy(workerIdentifier))
                        proxy->navigationPreloadIsReady(connection, identifier, WTF::move(response));
                },
                [connection, identifier, workerIdentifier](ResourceError&& error) {
                    if (RefPtr proxy = SWContextManager::singleton().serviceWorkerThreadProxy(workerIdentifier))
                        proxy->navigationPreloadFailed(connection, identifier, WTF::move(error));
                });
        }
    }

    m_sentToWorker = true;
    proxy->startFetch(s_serverConnection->identifier(), m_identifier, FetchClient::create(m_identifier), WTF::move(request), WTF::move(referrer), FetchOptions { options }, preloading, WTF::move(clientIdentifier), WTF::move(resultingClientIdentifier));
}

NavigationPreloader::~NavigationPreloader() = default;

void NavigationPreloader::cancel()
{
    Ref protectedThis { *this };
    m_taker = nullptr;
    if (RefPtr handle = std::exchange(m_handle, nullptr)) {
        handle->clearClient();
        handle->cancel();
    }
    remove();
}

void NavigationPreloader::take(FetchTask& task)
{
    ASSERT(!m_taken);
    Ref protectedThis { *this };
    m_taken = true;
    m_untakenTimer.stop();
    m_taker = &task;
    if (!m_response.isNull())
        tellResponse();
    for (auto& buffer : std::exchange(m_buffers, { }))
        tellData(WTF::move(buffer));
    if (m_finished || !m_error.isNull())
        tellEnd();
}

// The taker may end itself (and cancel this) from any of these: it is held for the call.
void NavigationPreloader::tellResponse()
{
    if (RefPtr taker = m_taker)
        taker->preloadSentResponse(m_response);
}

void NavigationPreloader::tellData(Ref<const SharedBuffer>&& buffer)
{
    if (RefPtr taker = m_taker)
        taker->preloadSentData(WTF::move(buffer));
}

void NavigationPreloader::tellEnd()
{
    Ref protectedThis { *this };
    if (RefPtr taker = std::exchange(m_taker, nullptr)) {
        if (m_finished)
            taker->preloadFinished(m_metrics);
        else
            taker->preloadFailed(m_error);
    }
    remove();
}

// ResourceLoader::start asks here before a load goes to curl.
bool interceptLoad(ResourceLoader& loader)
{
    auto& options = loader.options();
    // The body of a preloadResponse (or a fetch of event.request while the navigation is preloaded): from the preload.
    if (options.navigationPreloadIdentifier) {
        if (RefPtr preloader = NavigationPreloader::all().get(*options.navigationPreloadIdentifier); preloader && !preloader->isTaken()) {
            FetchTask::serveFromPreload(loader, *preloader);
            return true;
        }
    }
    if (options.serviceWorkersMode == ServiceWorkersMode::None || !s_server)
        return false;
    auto& url = loader.request().url();
    if (!url.protocolIsInHTTPFamily())
        return false;
    RefPtr frame = loader.frame();
    RefPtr page = frame ? frame->page() : nullptr;
    // Private tabs have no service workers (their settings say so too: this is for the loads).
    if (!page || page->sessionID().isEphemeral())
        return false;

    if (options.resultingClientIdentifier) {
        // A navigation, or a worker's script: is there any registration for the top origin?
        SecurityOriginData topOrigin = SecurityOriginData::fromURLWithoutStrictOpaqueness(url);
        if (!(frame->isMainFrame() && options.mode == FetchOptions::Mode::Navigate)) {
            if (RefPtr document = frame->document())
                topOrigin = document->topOrigin().data();
        }
        if (!s_originStore->mayContain(topOrigin))
            return false;
    } else {
        if (!options.serviceWorkerRegistrationIdentifier)
            return false;
        // <object> and <embed>: their request may turn out a navigation, which a subresource's worker must not see.
        if (isPotentialNavigationOrSubresourceRequest(options.destination))
            return false;
    }

    FetchTask::start(loader);
    return true;
}

} // namespace

void initializeServiceWorkers()
{
    static bool initialized;
    if (std::exchange(initialized, true))
        return;

    static NeverDestroyed<Delegate> delegate;
    auto originStore = makeUniqueRef<OriginStore>();
    s_originStore = originStore.ptr();
    UniqueRef<SWOriginStore> baseOriginStore = WTF::move(originStore);
    s_server = &SWServer::create(delegate.get(), WTF::move(baseOriginStore), true, serviceWorkersDirectory(), PAL::SessionID::defaultSessionID(), false, false, std::nullopt, ServiceWorkerIsInspectable::No).leakRef();

    Ref serverConnection = ServerConnection::create(*s_server);
    s_serverConnection = serverConnection.ptr();
    s_server->addConnection(WTF::move(serverConnection));

    s_clientConnection = &ClientConnection::create().leakRef();

    static NeverDestroyed<Provider> provider;
    ServiceWorkerProvider::setSharedProvider(provider.get());
    SWContextManager::singleton().setConnection(ContextManagerConnection::create());
    ResourceLoader::setLoadInterceptorForWP81(interceptLoad);
}

void serviceWorkerLoadRemoved(ResourceLoader& loader)
{
    auto& byLoader = fetchTasksByLoader();
    if (byLoader.isEmpty())
        return;
    auto iterator = byLoader.find(&loader);
    if (iterator == byLoader.end())
        return;
    if (RefPtr task = fetchTasks().get(iterator->value))
        task->loaderRemoved();
    else
        byLoader.remove(iterator);
}

void clearServiceWorkers(const SecurityOriginData& origin, CompletionHandler<void()>&& completionHandler)
{
    if (!s_server)
        return completionHandler();
    s_server->clear(origin, WTF::move(completionHandler));
}

void clearAllServiceWorkers(CompletionHandler<void()>&& completionHandler)
{
    if (!s_server)
        return completionHandler();
    s_server->clearAll(WTF::move(completionHandler));
}

void flushServiceWorkers(CompletionHandler<void()>&& completionHandler)
{
    if (!s_serverConnection)
        return completionHandler();
    s_serverConnection->flush(WTF::move(completionHandler));
}

String describeServiceWorkers()
{
    if (!s_server)
        return "sw: spenti"_s;
    StringBuilder builder;
    unsigned running = 0;
    s_server->forEachServiceWorker([&](const SWServerWorker& worker) {
        if (worker.isRunning())
            ++running;
        builder.append("\nsw: worker "_s, worker.identifier().toUInt64(), ' ', worker.scriptURL().string(),
            worker.isRunning() ? " in esecuzione"_s : " fermo"_s, ", stato "_s, static_cast<unsigned>(worker.state()));
        return true;
    });
    return makeString("sw: "_s, running, " worker in esecuzione, "_s, fetchTasks().size(), " richieste in corso"_s, builder.toString());
}

} // namespace Lumia
