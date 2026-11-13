#include "config.h"
#include "FrameLoaderClient.h"

#include "Authentication.h"
#include <WebCore/AuthenticationChallenge.h>
#include <WebCore/AuthenticationClient.h>

#include "BytecodeCache.h"
#include "Downloads.h"
#include "Engine.h"
#include "WebPage.h"
#include "WebResourceLoadScheduler.h"
#include <WebCore/CurlContext.h>
#include <WebCore/CurlRequestScheduler.h>
#include <WebCore/ActiveDOMObject.h>
#include <WebCore/Document.h>
#include <WebCore/DocumentLoader.h>
#include <WebCore/ResourceHandle.h>
#include <WebCore/SubresourceLoader.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/FrameDestructionObserverInlines.h>
#include <WebCore/FrameIdentifier.h>
#include <WebCore/FrameLoadRequest.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/FrameNetworkingContext.h>
#include <WebCore/FrameTree.h>
#include <WebCore/HistoryController.h>
#include <WebCore/HistoryItem.h>
#include <WebCore/FrameTreeSyncData.h>
#include <WebCore/Element.h>
#include <WebCore/HTMLElement.h>
#include <WebCore/HTMLFrameOwnerElement.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/Page.h>
#include <WebCore/MIMETypeRegistry.h>
#include <WebCore/NavigationAction.h>
#include <WebCore/NetworkLoadMetrics.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/StringWithDirection.h>

namespace WebKit {
using namespace WebCore;

namespace {

// Every frame loads through the process-wide curl storage session (cookies, HSTS, credentials), a private tab's frames
// through the private one.
class NetworkingContext final : public FrameNetworkingContext {
public:
    static Ref<NetworkingContext> create(LocalFrame& frame, bool ephemeral) { return adoptRef(*new NetworkingContext(frame, ephemeral)); }

private:
    NetworkingContext(LocalFrame& frame, bool ephemeral)
        : FrameNetworkingContext(&frame)
        , m_ephemeral(ephemeral)
    {
    }

    NetworkStorageSession* storageSession() const override { return m_ephemeral ? &WebPage::ephemeralStorageSession() : &WebPage::storageSession(); }
    const bool m_ephemeral;
    ResourceError blockedError(const ResourceRequest&) const override { return { }; }
};

// Where the main document's time went, in ms from the navigation's start (the load line in the log): until curl took the
// transfer (our scheduler and curl's own queue), DNS, TCP, TLS, then the server until the first byte of the answer.
// curl reports zero lookup and connect times when it reuses a connection.
String networkTiming(DocumentLoader& loader)
{
    // From the disk cache (DiskCache.cpp): no network at all, or only a 304.
    switch (loader.response().source()) {
    case ResourceResponse::Source::DiskCache:
        return ", dalla cache su disco"_s;
    case ResourceResponse::Source::DiskCacheAfterValidation:
        return ", dalla cache su disco dopo un 304"_s;
    default:
        break;
    }
    auto* metrics = loader.response().deprecatedNetworkLoadMetricsOrNull();
    auto start = loader.timing().startTime();
    if (!metrics || !start || !metrics->fetchStart)
        return { };
    auto ms = [](MonotonicTime from, MonotonicTime to) { return static_cast<int>((to - from).milliseconds()); };
    bool tls = metrics->secureConnectionStart && metrics->secureConnectionStart != reusedTLSConnectionSentinel;
    return makeString(", rete: curl dopo "_s, ms(start, metrics->fetchStart), " ms, DNS "_s,
        ms(metrics->domainLookupStart, metrics->domainLookupEnd), ", TCP "_s,
        ms(metrics->connectStart, tls ? metrics->secureConnectionStart : metrics->connectEnd), ", TLS "_s,
        tls ? ms(metrics->secureConnectionStart, metrics->connectEnd) : 0, ", primo byte dopo "_s,
        ms(metrics->requestStart, metrics->responseStart), " ms ("_s, metrics->protocol, ')');
}

} // namespace

FrameLoaderClient::FrameLoaderClient(LocalFrame& localFrame, FrameLoader& loader, WebPage& page)
    : EmptyFrameLoaderClient(loader)
    , m_localFrame(localFrame)
    , m_page(page)
{
}

bool FrameLoaderClient::isMainFrame() const
{
    return m_localFrame->isMainFrame();
}

// ---- policy ----

// Every navigation proceeds, but a link with the download attribute saves its target (startDownload). Schemes the engine
// cannot load (tel:, mailto:...) fail on their own for now.
void FrameLoaderClient::dispatchDecidePolicyForNavigationAction(const NavigationAction& action, const ResourceRequest& request, const ResourceResponse&,
    FormState*, const String&, std::optional<NavigationIdentifier>, std::optional<HitTestResult>&&, bool,
    NavigationUpgradeToHTTPSBehavior, SandboxFlags, PolicyDecisionMode, FramePolicyFunction&& function)
{
    // The buttons of the engine's own pages (WebPage::loadErrorPage). Only that page's own document, under the user's
    // finger, may press them: another window that navigates this one to the address gets nothing.
    if (request.url().protocolIs("lumiaweb"_s)) {
        function(PolicyAction::Ignore);
        RefPtr document = m_localFrame->document();
        bool fromThisPage = document && action.requester() && action.requester()->documentIdentifier == document->identifier();
        if (m_page && isMainFrame() && fromThisPage && action.processingUserGesture())
            m_page->didTapInternalLink(request.url());
        else
            Lumia::log("lumiaweb: rifiutato (%s)", request.url().string().utf8().data());
        return;
    }
    function(action.downloadAttribute().isNull() ? PolicyAction::Use : PolicyAction::Download);
}

void FrameLoaderClient::dispatchDidReceiveAuthenticationChallenge(DocumentLoader*, ResourceLoaderIdentifier, const AuthenticationChallenge& challenge)
{
    if (m_page) {
        Lumia::askForPassword(m_page->view(), challenge);
        return;
    }
    if (RefPtr client = challenge.authenticationClient())
        client->receivedRequestToContinueWithoutCredential(challenge);
}

// A link with target=_blank: the app opens it in a new tab (LW_EVENT_NEW_WINDOW). A form posted to a new window keeps
// the old behaviour, this tab with the page's own request (method, body, referrer), since a tab opened by the app could
// only repeat the address. window.open() gets null as with a popup blocker (ChromeClient::createWindow).
void FrameLoaderClient::dispatchDecidePolicyForNewWindowAction(const NavigationAction&, const ResourceRequest& request, FormState*, const String&,
    std::optional<HitTestResult>&&, FramePolicyFunction&& function)
{
    function(PolicyAction::Ignore);
    if (!m_page)
        return;
    if (request.httpMethod() == "GET"_s && !request.url().protocolIsJavaScript()) {
        Lumia::postEvent(m_page->view(), LW_EVENT_NEW_WINDOW, 0, request.url().string(), request.httpReferrer());
        return;
    }
    Lumia::postToWebThread([page = Ref { *m_page }, request] {
        if (RefPtr frame = page->localMainFrame())
            frame->loader().load(FrameLoadRequest(*frame, ResourceRequest { request }));
    });
}

// What the engine can show is shown; the rest is saved as a file (convertMainResourceLoadToDownload) and the page stays.
// 204/205 leave the current page alone, as in WebKit2; an error page in a type the engine cannot show is not a file.
void FrameLoaderClient::dispatchDecidePolicyForResponse(const ResourceResponse& response, const ResourceRequest&, const String&, FramePolicyFunction&& function)
{
    int status = response.httpStatusCode();
    if (status == 204 || status == 205) {
        function(PolicyAction::Ignore);
        return;
    }
    if (response.isAttachment() || !canShowMIMEType(response.mimeType())) {
        Lumia::log("risposta non mostrabile: HTTP %d, tipo \"%s\", allegato %d, %s", status, response.mimeType().utf8().data(),
            response.isAttachment(), response.url().string().utf8().data());
        function(status < 400 ? PolicyAction::Download : PolicyAction::Ignore);
        return;
    }
    function(PolicyAction::Use);
}

// PolicyAction::Download on a response: the main resource's load goes on into the file, so nothing is requested twice.
// Without a loader (the response came from the memory cache) the file is fetched again.
void FrameLoaderClient::convertMainResourceLoadToDownload(DocumentLoader* documentLoader, const ResourceRequest& request, const ResourceResponse& response)
{
    if (!m_page)
        return;
    RefPtr loader = documentLoader ? documentLoader->mainResourceLoader() : nullptr;
    if (RefPtr handle = loader ? loader->handle() : nullptr) {
        Lumia::convertToDownload(m_page->view(), *handle, request, response);
        return;
    }
    Lumia::startDownload(m_page->view(), m_localFrame->loader().networkingContext(), request, { });
}

// A link with the download attribute, a data: URL to save, a navigation the policy turned into a download.
void FrameLoaderClient::startDownload(const ResourceRequest& request, const String& suggestedName, FromDownloadAttribute)
{
    if (m_page)
        Lumia::startDownload(m_page->view(), m_localFrame->loader().networkingContext(), request, suggestedName);
}

// As WebKit2: a cancelled load or one a plug-in takes over is not replaced by fallback content.
bool FrameLoaderClient::shouldFallBack(const ResourceError& error) const
{
    if (error.isCancellation())
        return false;
    return !(error.domain() == "WebKitPluginError"_s && error.errorCode() == 204);
}

// The document's bytes go to its parser (DocumentWriter). EmptyFrameLoaderClient drops them: pages stayed empty.
void FrameLoaderClient::committedLoad(DocumentLoader* loader, const SharedBuffer& data)
{
    loader->commitData(data);
}

bool FrameLoaderClient::canShowMIMEType(const String& mimeType) const
{
    return MIMETypeRegistry::canShowMIMEType(mimeType);
}

// WebKit2's rule without plug-ins: no type and no known extension → frame; images → image; anything showable → frame.
ObjectContentType FrameLoaderClient::objectContentType(const URL& url, const String& mimeTypeIn)
{
    String mimeType = mimeTypeIn;
    if (mimeType.isEmpty()) {
        auto path = url.path();
        auto dotPosition = path.reverseFind('.');
        if (dotPosition == notFound)
            return ObjectContentType::Frame;
        mimeType = MIMETypeRegistry::mimeTypeForExtension(path.substring(dotPosition + 1).convertToASCIILowercase());
        if (mimeType.isEmpty())
            return ObjectContentType::Frame;
    }
    if (MIMETypeRegistry::isSupportedImageMIMEType(mimeType))
        return ObjectContentType::Image;
    if (MIMETypeRegistry::isSupportedNonImageMIMEType(mimeType))
        return ObjectContentType::Frame;
    return ObjectContentType::None;
}

// ---- frames ----

void FrameLoaderClient::transitionToCommittedForNewPage(InitializingIframe)
{
    if (m_page)
        m_page->createFrameView(m_localFrame.get());
}

// As WebFrame::createSubframe in WebKit2: the subframe inherits the owner's and parent's sandbox flags and the
// top document's referrer policy, gets its own client, and is initialised (which loads about:blank).
RefPtr<LocalFrame> FrameLoaderClient::createFrame(const AtomString& name, HTMLFrameOwnerElement& ownerElement)
{
    RefPtr page = m_page.get();
    if (!page || !page->corePage())
        return nullptr;

    auto sandboxFlags = ownerElement.sandboxFlags();
    sandboxFlags.add(m_localFrame->effectiveSandboxFlags());
    auto referrerPolicy = ownerElement.referrerPolicy();
    if (RefPtr topDocument = page->localMainFrame() ? page->localMainFrame()->document() : nullptr; referrerPolicy == ReferrerPolicy::EmptyString && topDocument)
        referrerPolicy = topDocument->referrerPolicy();

    Ref subframe = LocalFrame::createSubframe(*page->corePage(), [page] (auto& localFrame, auto& frameLoader) {
        return makeUniqueRefWithoutRefCountedCheck<FrameLoaderClient>(localFrame, frameLoader, *page);
    }, generateFrameIdentifier(), sandboxFlags, referrerPolicy, ownerElement, FrameTreeSyncData::create());

    subframe->tree().setSpecifiedName(name);
    subframe->init();

    // The creation of the frame may have run JavaScript that removed it from the page already.
    if (!subframe->page())
        return nullptr;
    return subframe;
}

Ref<FrameNetworkingContext> FrameLoaderClient::createNetworkingContext()
{
    return NetworkingContext::create(m_localFrame.get(), m_page && m_page->isEphemeral());
}

String FrameLoaderClient::userAgent(const URL& url) const
{
    return m_page ? Lumia::userAgent(m_page->view(), url) : String();
}

// ---- session history ----

// As WebKitLegacy: the main frame's history controller builds the item tree for this frame's navigation.
RefPtr<HistoryItem> FrameLoaderClient::createHistoryItemTree(bool clipAtTarget, BackForwardItemIdentifier itemID) const
{
    Ref rootFrame = m_localFrame->rootFrame();
    return rootFrame->loader().history().createItemTree(m_localFrame.get(), clipAtTarget, itemID);
}

// The page just left went into the back/forward cache, already paused by suspendLeavingDocuments under the embedder's
// reason (PageWillBeSuspended); the cache's own suspension was then ignored and its resume on restore would be too
// (Document::resumeScheduledTasks needs the same reason). Re-suspended here under the cache's reason, in one go on the
// Web thread, so that Back resumes it.
void FrameLoaderClient::didCacheBackForwardItem(BackForwardItemIdentifier, BackForwardFrameItemIdentifier)
{
    // Each document here was suspended by suspendLeavingDocuments first; resumeScheduledTasks does nothing if the
    // reason is not that one, and suspendScheduledTasks nothing if still suspended.
    for (auto& weakDocument : std::exchange(m_suspendedDocuments, { })) {
        RefPtr document = weakDocument.get();
        if (!document)
            continue;
        document->resumeScheduledTasks(ReasonForSuspension::PageWillBeSuspended);
        document->suspendScheduledTasks(ReasonForSuspension::BackForwardCache);
    }
}

// ---- load events: the app hears about the main frame only ----

// A navigation away from a page pauses it while the next one loads: timers, animation frames, its event loop and
// microtasks stop in every frame, so a heavy page (YouTube: seconds of script tasks after the address was typed) no
// longer keeps the new one waiting for the Web thread. It resumes if the navigation ends without a new document (an
// error, Stop, a download); on commit it goes away suspended, or into the back/forward cache (didCacheBackForwardItem).
void FrameLoaderClient::suspendLeavingDocuments()
{
    if (!m_suspendedDocuments.isEmpty())
        return;
    RefPtr document = m_localFrame->document();
    if (!document || document->url().isEmpty() || document->url().isAboutBlank())
        return;
    for (RefPtr<Frame> frame = m_localFrame.ptr(); frame; frame = frame->tree().traverseNext()) {
        RefPtr localFrame = dynamicDowncast<LocalFrame>(*frame);
        RefPtr frameDocument = localFrame ? localFrame->document() : nullptr;
        if (!frameDocument)
            continue;
        frameDocument->suspendScheduledTasks(ReasonForSuspension::PageWillBeSuspended);
        m_suspendedDocuments.append(*frameDocument);
    }
}

void FrameLoaderClient::resumeLeavingDocuments()
{
    for (auto& weakDocument : std::exchange(m_suspendedDocuments, { })) {
        if (RefPtr document = weakDocument.get(); document && document->frame())
            document->resumeScheduledTasks(ReasonForSuspension::PageWillBeSuspended);
    }
}

void FrameLoaderClient::dispatchDidStartProvisionalLoad()
{
    if (!m_page || !isMainFrame())
        return;
    suspendLeavingDocuments();
    if (RefPtr loader = m_localFrame->loader().provisionalDocumentLoader())
        loader->setCustomNavigatorPlatform(Lumia::navigatorPlatform(m_page->view(), loader->url()));
    Lumia::postEvent(m_page->view(), LW_EVENT_LOAD_STATE, LW_LOAD_STARTED);
    // A navigation that never commits is usually a request stuck behind others: say what the queues hold
    // (host requests loading/waiting, then curl's own transfers).
    Lumia::log("rete: code all'inizio della navigazione: %s; curl %u trasferimenti", webResourceLoadScheduler().describeQueues().utf8().data(),
        WebCore::CurlContext::singleton().scheduler().activeJobCount());
}

void FrameLoaderClient::dispatchDidReceiveTitle(const StringWithDirection& title)
{
    static constexpr unsigned maxTitleLength = 1000; // as WebKit2
    if (m_page && isMainFrame())
        Lumia::postEvent(m_page->view(), LW_EVENT_TITLE, 0, truncateFromEnd(title, maxTitleLength).string);
}

void FrameLoaderClient::dispatchDidCommitLoad(std::optional<HasInsecureContent>, std::optional<UsedLegacyTLS>, std::optional<WasPrivateRelayed>)
{
    if (!m_page || !isMainFrame())
        return;
    // The page that was left is gone with its frames' documents, still suspended.
    m_suspendedDocuments.clear();
    RefPtr loader = m_localFrame->loader().documentLoader();
    if (loader)
        loader->setCustomNavigatorPlatform(Lumia::navigatorPlatform(m_page->view(), loader->url()));
    // An error page shows the address that failed, not its about:blank.
    if (loader && !loader->unreachableURL().isEmpty())
        Lumia::postEvent(m_page->view(), LW_EVENT_URL, 0, loader->unreachableURL().string());
    else if (RefPtr document = m_localFrame->document())
        Lumia::postEvent(m_page->view(), LW_EVENT_URL, 0, document->url().string());
    Lumia::postEvent(m_page->view(), LW_EVENT_LOAD_STATE, LW_LOAD_COMMITTED);
    m_page->didCommitBlankForSiteClear();
    // A new page starts from its own viewport and initial scale: the previous page's zoom does not carry over.
    m_page->didCommitLoad();
}

void FrameLoaderClient::dispatchDidLayout()
{
    if (m_page && isMainFrame())
        m_page->mainFrameDidLayout();
}

void FrameLoaderClient::dispatchDidChangeLocationWithinPage()
{
    if (!m_page || !isMainFrame())
        return;
    if (RefPtr document = m_localFrame->document())
        Lumia::postEvent(m_page->view(), LW_EVENT_URL, 0, document->url().string());
}

void FrameLoaderClient::dispatchDidFinishLoad()
{
    if (!m_page || !isMainFrame())
        return;
    // What the main frame got, for the log: cheap fields only (serializing the document here cost the Web thread a
    // copy of the whole page at every load).
    if (RefPtr loader = m_localFrame->loader().documentLoader()) {
        RefPtr data = loader->mainResourceData();
        Lumia::log("FrameLoaderClient: fine %s, HTTP %d, %s, byte %u%s", loader->url().string().utf8().data(),
            loader->response().httpStatusCode(), loader->response().mimeType().utf8().data(),
            data ? static_cast<unsigned>(data->size()) : 0, networkTiming(*loader).utf8().data());
    }
    Lumia::postEvent(m_page->view(), LW_EVENT_LOAD_STATE, LW_LOAD_FINISHED);
    // Most of a page's start-up code has run a few seconds after its load event: then its scripts' compiled code goes
    // to the flash, while the user reads.
    Lumia::BytecodeCache::scheduleCommit(8_s);
}

void FrameLoaderClient::dispatchDidFailLoad(const ResourceError& error)
{
    if (m_page && isMainFrame())
        Lumia::postEvent(m_page->view(), LW_EVENT_LOAD_STATE, LW_LOAD_FAILED, error.localizedDescription(), error.failingURL().string());
}

// A provisional load that gives way to another (a new navigation, a policy Ignore) is not a failure for the app.
void FrameLoaderClient::dispatchDidFailProvisionalLoad(const ResourceError& error, WillContinueLoading willContinueLoading, WillInternallyHandleFailure)
{
    if (!m_page || !isMainFrame() || willContinueLoading == WillContinueLoading::Yes)
        return;
    // The page stays: it runs again.
    resumeLeavingDocuments();
    if (error.isCancellation())
        return;
    // Policy outcomes (a download, a type the engine does not show, a blocked port) are not failures of the page: it is
    // as it was, and the app only hears that the load is over.
    if (error.domain() == "WebKitPolicyError"_s || error.domain() == "WebKitPluginError"_s) {
        Lumia::postEvent(m_page->view(), LW_EVENT_LOAD_STATE, LW_LOAD_FINISHED);
        return;
    }
    Lumia::postEvent(m_page->view(), LW_EVENT_LOAD_STATE, LW_LOAD_FAILED, error.localizedDescription(), error.failingURL().string());
    // Network failures get an error page with a retry button.
    // Not from inside the failing load's own call stack.
    Lumia::postToWebThread([page = Ref { *m_page }, error = error.isolatedCopy()] {
        page->loadErrorPage(error);
    });
}

} // namespace WebKit
