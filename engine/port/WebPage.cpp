#include "config.h"
#include "WebPage.h"

#include "BackForwardList.h"

#include "ChromeClient.h"
#include "EditorClient.h"
#include "Engine.h"
#include "FrameLoaderClient.h"
#include "VisitedLinks.h"
#include "LayerTreeHost.h"
#include "Permissions.h"
#include "ScrollingTreeDispatcher.h"
#include "Storage.h"
#include "ThreadedCompositor.h"
#include "WebSocketChannel.h"
#include "WebPreferencesDefaults.h"
#include <WebCore/BackForwardCache.h>
#include <WebCore/CertificateInfo.h>
#include <WebCore/CertificateSummary.h>
#include <WebCore/CookieJar.h>
#include <WebCore/CookieJarDB.h>
#include <WebCore/DatabaseProvider.h>
#include <WebCore/DisplayRefreshMonitor.h>
#include <WebCore/DocumentLoader.h>
#include <WebCore/DocumentView.h>
#include <WebCore/EmptyClients.h>
#include <WebCore/FrameLoadRequest.h>
#include <WebCore/FrameLoader.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/LocalFrameView.h>
#include <WebCore/LocalFrameViewLayoutContext.h>
#include <WebCore/NetworkStorageSession.h>
#include <WebCore/PageConfiguration.h>
#include <WebCore/PlatformScreen.h>
#include <WebCore/ProgressTracker.h>
#include <WebCore/ProgressTrackerClient.h>
#include <WebCore/RenderView.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceHandle.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>
#include <WebCore/SharedBuffer.h>
#include <WebCore/SubstituteData.h>
#include <WebCore/Settings.h>
#include <WebCore/StorageNamespaceProvider.h>
#include <WebCore/SocketProvider.h>
#include <WebCore/WebTransportSession.h>
#include <WebCore/StorageSessionProvider.h>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace WebKit {
using namespace WebCore;

WTF_MAKE_TZONE_ALLOCATED_IMPL(WebPage);

namespace {

// document.cookie reads and writes the same session the loads use.
class StorageSessionProvider final : public WebCore::StorageSessionProvider {
public:
    static Ref<StorageSessionProvider> create() { return adoptRef(*new StorageSessionProvider); }

private:
    NetworkStorageSession* storageSession() const override { return &WebPage::storageSession(); }
};

// Load progress for the app's bar (LW_EVENT_PROGRESS, 0-100): WebCore's estimate for the whole page, subframes
// included, which ProgressTracker itself reports at most every 200 ms or 2%.
class ProgressTrackerClient final : public WebCore::ProgressTrackerClient {
public:
    explicit ProgressTrackerClient(WebPage& page)
        : m_page(page)
    {
    }

private:
    void progressStarted(LocalFrame&) override { report(); }
    void progressEstimateChanged(LocalFrame&) override { report(); }
    // ProgressTracker has reset its estimate by now.
    void progressFinished(LocalFrame&) override { report(100); }

    void report(std::optional<int> finished = std::nullopt)
    {
        RefPtr page = m_page.get();
        if (!page || !page->corePage())
            return;
        int percent = finished.value_or(static_cast<int>(page->corePage()->progress().estimatedProgress() * 100));
        if (percent == std::exchange(m_lastPercent, percent))
            return;
        Lumia::postEvent(page->view(), LW_EVENT_PROGRESS, percent);
    }

    WeakPtr<WebPage> m_page;
    int m_lastPercent { -1 };
};

// WebSockets: WebKitLegacy's in-process channel (protocol, deflate, closing handshake) over a curl socket stream
// (SocketStreamHandleImplCurl.cpp). One provider for every tab; workers' sockets come through it on the Web thread too.
class SocketProvider final : public WebCore::SocketProvider {
public:
    static SocketProvider& singleton()
    {
        static NeverDestroyed<Ref<SocketProvider>> provider = adoptRef(*new SocketProvider);
        return provider.get();
    }

private:
    RefPtr<ThreadableWebSocketChannel> createWebSocketChannel(Document& document, WebSocketChannelClient& client, IsInitiatedByDedicatedWorker) final
    {
        return WebCore::WebSocketChannel::create(document, client, *this);
    }

    std::pair<RefPtr<WebTransportSession>, Ref<WebTransportSessionPromise>> initializeWebTransportSession(ScriptExecutionContext&, WebTransportSessionClient&, const URL&, const WebTransportOptions&) final
    {
        return { nullptr, WebTransportSessionPromise::createAndReject() };
    }
};

// Every tab gets its own display ID: its ThreadedDisplayRefreshMonitor is driven by its own compositor (as
// DrawingAreaCoordinatedGraphics does in WebKit2).
PlatformDisplayID nextDisplayID()
{
    static PlatformDisplayID displayID = std::numeric_limits<PlatformDisplayID>::max();
    return displayID--;
}

} // namespace

Ref<WebPage> WebPage::create(lw_view& view, void* swapChainPanel, const IntSize& size, float deviceScaleFactor)
{
    return adoptRef(*new WebPage(view, swapChainPanel, size, deviceScaleFactor));
}

WebPage::WebPage(lw_view& view, void* swapChainPanel, const IntSize& size, float deviceScaleFactor)
    : m_view(view)
    , m_swapChainPanel(swapChainPanel)
    , m_size(size)
    , m_deviceScaleFactor(deviceScaleFactor)
    , m_backForwardList(Lumia::BackForwardList::create(view))
{
    setScreenSizeForWP81(FloatSize(m_size));
    // Before the main frame's first view (frame->init() below): it lays out at the viewport's width from the start.
    // As Safari, the finger may always zoom, between the fit and 5x, whatever the page's user-scalable and
    // maximum-scale say (WKWebViewConfiguration.ignoresViewportScaleLimits).
    m_viewportConfiguration.setForceAlwaysUserScalable(true);
    m_viewportConfiguration.setViewLayoutSize(FloatSize(m_size));

    // An identifier, as WebKitLegacy's views have: without one the page has no media session manager, and an element's
    // play() is refused without a word (Page::mediaSessionManager).
    auto configuration = pageConfigurationWithEmptyClients(PageIdentifier::generate(), PAL::SessionID::defaultSessionID());
    // Session history (4c): the tab's own list, so Back is WebCore's (scroll position, form state, the cached page).
    configuration.backForwardClient = m_backForwardList.copyRef();
    configuration.historyItemClient = Lumia::historyItemClient();
    configuration.chromeClient = makeUniqueRef<ChromeClient>(*this);
    configuration.editorClient = makeUniqueRef<LumiaEditorClient>(*this);
    configuration.progressTrackerClient = makeUniqueRef<ProgressTrackerClient>(*this);
    configuration.cookieJar = CookieJar::create(StorageSessionProvider::create());
    configuration.storageNamespaceProvider = &Lumia::storageNamespaceProvider();
    configuration.databaseProvider = &Lumia::databaseProvider();
    configuration.socketProvider = SocketProvider::singleton();
    // :visited, from the addresses visited in any tab (VisitedLinks.cpp).
    configuration.visitedLinkStore = &Lumia::visitedLinks();
    configuration.mainFrameCreationParameters = PageConfiguration::LocalMainFrameCreationParameters {
        { [this] (auto& localFrame, auto& frameLoader) {
            return makeUniqueRefWithoutRefCountedCheck<FrameLoaderClient>(localFrame, frameLoader, *this);
        } },
        SandboxFlags { },
        ReferrerPolicy::EmptyString
    };
    m_page = Page::create(WTF::move(configuration));
    // As WebKit2 does right after creating the page: the main frame gets its initial empty document, which loads
    // start from (FrameLoadRequest reads the requester's document).
    if (RefPtr frame = localMainFrame())
        frame->init();

    // WebKit's defaults first (WebCore's own are neutral: JavaScript off...), then the port's choices.
    auto& settings = m_page->settings();
    applyWebKitDefaults(settings);
    // No service worker server in the port yet (WebKit2's lives in the network process): with none registered,
    // ServiceWorkerProvider::singleton() RELEASE_ASSERTs on the first document. Sites see no navigator.serviceWorker
    // and fall back, until a target site needs them (plan, phase 7 gaps).
    settings.setServiceWorkersEnabled(false);
    // The page just left stays alive for an instant Back (BackForwardCache, one page: Engine.cpp).
    settings.setUsesBackForwardCache(true);
    // Hidden tabs (4f) keep their timers at most once a second and their CSS animations stopped, as Safari and GTK do;
    // WebKit's defaults for other ports leave them running at full rate.
    settings.setHiddenPageDOMTimerThrottlingEnabled(true);
    settings.setHiddenPageCSSAnimationSuspensionEnabled(true);
    // Popups only from a tap (a link or button): window.open from a timer or on load gets null, as in Safari.
    settings.setJavaScriptCanOpenWindowsAutomatically(false);
    // requestFullscreen and navigator.share have their client (ChromeClient); geolocation and notifications ask the
    // user through the app (Permissions.cpp).
    settings.setFullScreenEnabled(true);
    // The clipboard (the engine's own, shims/wp81shims.c): scripts copy and cut during a tap, and read only after the
    // user said yes (EditorClient::requestDOMPasteAccess).
    settings.setJavaScriptCanAccessClipboard(true);
    settings.setAsyncClipboardAPIEnabled(true);
    settings.setDOMPasteAccessRequestsEnabled(true);
    settings.setDOMPasteAllowed(false);
    settings.setWebShareEnabled(true);
    Lumia::provideDeviceFeatures(m_view, *m_page);

    // Always composited: every frame reaches the screen through the compositor thread, nothing is painted in software
    // into the window.
    settings.setAcceleratedCompositingEnabled(true);
    settings.setForceCompositingMode(true);
    settings.setAcceleratedCompositingForFixedPositionEnabled(true);
    settings.setScrollingCoordinatorEnabled(true);
    settings.setAsyncFrameScrollingEnabled(true);
    settings.setAsyncOverflowScrollingEnabled(true);
    // The tap highlight is drawn by the compositor from these (CoordinatedPlatformLayer's event region).
    settings.setInteractionRegionsEnabled(true);
    // The same event regions also record the page's touch and pointer listeners (passive or not) and its touch-action:
    // the scrolling thread decides about a pan from them, without asking the Web thread (ScrollingTreeDispatcher).
    settings.setAlwaysUseTouchEventRegions(true);

    // CSS generic families on the phone's fonts (C:\Windows\Fonts), as desktop Windows browsers map them.
    // system-ui comes from SystemParametersInfo (Segoe WP, port/shims).
    settings.setStandardFontFamily("Times New Roman"_s);
    settings.setSerifFontFamily("Times New Roman"_s);
    settings.setSansSerifFontFamily("Arial"_s);
    settings.setFixedFontFamily("Courier New"_s);
    settings.setCursiveFontFamily("Comic Sans MS"_s);
    settings.setPictographFontFamily("Segoe UI Symbol"_s);

    m_page->setDeviceScaleFactor(m_deviceScaleFactor);

    auto displayID = nextDisplayID();
    m_layerTreeHost = makeUnique<LayerTreeHost>(*this, displayID);
    m_page->windowScreenDidChange(displayID, std::nullopt);

    if (m_page->scrollingCoordinator())
        ScrollingTreeDispatcher::singleton().addScrollingTreeForView(m_view, *this);
}

WebPage::~WebPage()
{
    close();
}

void WebPage::close()
{
    if (!m_page)
        return;

    ScrollingTreeDispatcher::singleton().removeScrollingTreeForView(m_view);
    if (RefPtr frame = localMainFrame())
        frame->loader().stopAllLoaders();
    // The tab's cached pages go with it (as WebKit2's WebPage::close).
    BackForwardCache::singleton().removeAllItemsForPage(*m_page);
    // The compositor is invalidated before the page goes, so no layer outlives the scene it belongs to.
    m_layerTreeHost = nullptr;
    m_page = nullptr;
}

LocalFrame* WebPage::localMainFrame() const
{
    return m_page ? dynamicDowncast<LocalFrame>(m_page->mainFrame()) : nullptr;
}

LocalFrameView* WebPage::localMainFrameView() const
{
    RefPtr frame = localMainFrame();
    return frame ? frame->view() : nullptr;
}

void WebPage::loadURL(const String& urlString)
{
    RefPtr frame = localMainFrame();
    if (!frame)
        return;
    frame->loader().load(FrameLoadRequest(*frame, ResourceRequest(URL { urlString })));
}

void WebPage::suspendLeavingDocuments()
{
    if (RefPtr frame = localMainFrame())
        static_cast<FrameLoaderClient&>(frame->loader().client()).suspendLeavingDocuments();
}

void WebPage::resumeLeavingDocuments()
{
    if (RefPtr frame = localMainFrame())
        static_cast<FrameLoaderClient&>(frame->loader().client()).resumeLeavingDocuments();
}

void WebPage::clearSiteDataAndReload()
{
    RefPtr frame = localMainFrame();
    RefPtr document = frame ? frame->document() : nullptr;
    if (!document || m_pendingSiteClear)
        return;
    m_pendingSiteClear = PendingSiteClear { document->securityOrigin().data(), document->url() };
    loadURL(aboutBlankURL().string());
}

void WebPage::didCommitBlankForSiteClear()
{
    if (!m_pendingSiteClear)
        return;
    RefPtr frame = localMainFrame();
    if (!frame || !frame->document() || !frame->document()->url().isAboutBlank())
        return;
    auto pending = *std::exchange(m_pendingSiteClear, std::nullopt);
    // After the commit task: the old document's IndexedDB close messages are already on the server's queue, which
    // runs them before the deletion.
    Lumia::postToWebThread([protectedThis = Ref { *this }, pending = WTF::move(pending)] {
        Lumia::clearSiteData(pending.origin);
        if (RefPtr frame = protectedThis->localMainFrame()) {
            ResourceRequest request { URL { pending.url } };
            request.setCachePolicy(ResourceRequestCachePolicy::ReloadIgnoringCacheData);
            frame->loader().load(FrameLoadRequest(*frame, WTF::move(request)));
        }
    });
}

namespace {

String escapeForHTML(const String& text)
{
    StringBuilder builder;
    for (auto character : StringView(text).codeUnits()) {
        switch (character) {
        case '&': builder.append("&amp;"_s); break;
        case '<': builder.append("&lt;"_s); break;
        case '>': builder.append("&gt;"_s); break;
        case '"': builder.append("&quot;"_s); break;
        default: builder.append(character);
        }
    }
    return builder.toString();
}

// What went wrong, in the words of the phone's UI (HTML, entities for the accented letters).
ASCIILiteral describeError(const ResourceError& error)
{
    if (error.domain() == "CurlErrorDomain"_s) {
        switch (error.errorCode()) {
        case 6: // CURLE_COULDNT_RESOLVE_HOST
            return "Il nome del sito non &#232; stato trovato: controlla l'indirizzo, oppure la connessione non c'&#232;."_s;
        case 7: // CURLE_COULDNT_CONNECT
            return "Il server non accetta connessioni, oppure la rete non lo raggiunge."_s;
        case 28: // CURLE_OPERATION_TIMEDOUT
            return "Il server non ha risposto in tempo."_s;
        case 35: // CURLE_SSL_CONNECT_ERROR
        case 60: // CURLE_PEER_FAILED_VERIFICATION
            return "La connessione sicura non &#232; riuscita: il certificato del sito non &#232; valido o non &#232; riconosciuto."_s;
        case 52: // CURLE_GOT_NOTHING
        case 55: // CURLE_SEND_ERROR
        case 56: // CURLE_RECV_ERROR
            return "La connessione si &#232; interrotta."_s;
        default:
            break;
        }
    }
    return "La pagina non si &#232; caricata."_s;
}

// What OpenSSL found wrong with the certificate (X509_V_ERR_*), in the words of the phone's UI.
ASCIILiteral describeCertificateError(int verificationError)
{
    switch (verificationError) {
    case 9: // CERT_NOT_YET_VALID
        return "Il certificato del sito non &#232; ancora valido. Controlla anche la data del telefono."_s;
    case 10: // CERT_HAS_EXPIRED
        return "Il certificato del sito &#232; scaduto. Controlla anche la data del telefono."_s;
    case 18: // DEPTH_ZERO_SELF_SIGNED_CERT
    case 19: // SELF_SIGNED_CERT_IN_CHAIN
        return "Il certificato del sito &#232; firmato da s&#233; stesso: nessuna autorit&#224; garantisce che il sito sia chi dice di essere."_s;
    case 2: // UNABLE_TO_GET_ISSUER_CERT
    case 20: // UNABLE_TO_GET_ISSUER_CERT_LOCALLY
    case 21: // UNABLE_TO_VERIFY_LEAF_SIGNATURE
        return "Il certificato del sito &#232; emesso da un'autorit&#224; che il telefono non conosce."_s;
    case 23: // CERT_REVOKED
        return "Il certificato del sito &#232; stato revocato."_s;
    case 62: // HOSTNAME_MISMATCH
    case 64: // IP_ADDRESS_MISMATCH
        return "Il certificato appartiene a un altro sito: l'indirizzo non &#232; tra i suoi nomi."_s;
    default:
        return "Il certificato del sito non &#232; valido o non &#232; riconosciuto."_s;
    }
}

String formatCertificateDate(Seconds sinceEpoch)
{
    time_t time = static_cast<time_t>(sinceEpoch.seconds());
    struct tm parts { };
    if (gmtime_s(&parts, &time))
        return "?"_s;
    char text[16];
    snprintf(text, sizeof(text), "%02d/%02d/%04d", parts.tm_mday, parts.tm_mon + 1, parts.tm_year + 1900);
    return String::fromLatin1(text);
}

// The certificate that failed, as rows of the error page: whose it is, its dates, the names it covers, OpenSSL's verdict.
String certificateDetails(const CertificateInfo& info)
{
    StringBuilder rows;
    auto row = [&](ASCIILiteral label, const String& value) {
        if (!value.isEmpty())
            rows.append("<tr><td>"_s, label, "</td><td>"_s, escapeForHTML(value), "</td></tr>"_s);
    };
    if (auto summary = info.summary()) {
        row("Intestato a"_s, summary->subject);
        row("Valido dal"_s, formatCertificateDate(summary->validFrom));
        row("Valido fino al"_s, formatCertificateDate(summary->validUntil));
        StringBuilder names;
        unsigned count = 0;
        auto addName = [&](const String& name) {
            if (count++ < 8)
                names.append(names.isEmpty() ? ""_s : ", "_s, name);
        };
        for (auto& name : summary->dnsNames)
            addName(name);
        for (auto& name : summary->ipAddresses)
            addName(name);
        if (count > 8)
            names.append(" e altri "_s, count - 8);
        row("Nomi"_s, names.toString());
    }
    row("Verifica"_s, makeString(info.verificationErrorDescription(), " ("_s, info.verificationError(), ')'));
    return rows.toString();
}

constexpr auto errorPageStyle = "body{margin:0;padding:28px 20px;font-family:'Segoe WP','Segoe UI',sans-serif;background:#fff;color:#1f1f1f}"
    "h1{font-size:30px;font-weight:300;margin:0 0 18px}p{font-size:17px;line-height:1.4;margin:0 0 12px;color:#444;word-wrap:break-word}"
    ".host{color:#1976D2;font-weight:600}button{margin:18px 12px 0 0;border:0;border-radius:0;background:#1976D2;color:#fff;"
    "font-size:19px;padding:12px 28px;font-family:inherit}.detail{font-size:13px;color:#999;margin-top:28px}"
    "button.risk{background:#fff;color:#B00020;border:2px solid #B00020;padding:10px 26px}"
    "table{border-collapse:collapse;margin:18px 0 0;font-size:14px;color:#444}td{padding:3px 12px 3px 0;vertical-align:top;word-break:break-all}"
    "td:first-child{color:#999;white-space:nowrap;word-break:normal}"_s;

} // namespace

void WebPage::loadErrorPage(const ResourceError& error)
{
    RefPtr frame = localMainFrame();
    if (!frame || error.failingURL().isEmpty())
        return;
    auto host = error.failingURL().host().toString();
    String html;
    m_certificateErrorURL = { };
    if (error.isCertificationVerificationError()) {
        // The certificate's own page: what is wrong with it, whose it is, and the way through for who knows the site
        // (a router, a home server): "Continua comunque" is lumiaweb:certificato (didTapInternalLink).
        auto info = ResourceHandle::certificateFailureForWP81(host);
        m_certificateErrorURL = error.failingURL();
        html = makeString("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            "<title>Connessione non sicura</title><style>"_s, errorPageStyle,
            "</style></head><body><h1>La connessione non &#232; sicura</h1><p class=\"host\">"_s, escapeForHTML(host), "</p><p>"_s,
            describeCertificateError(info ? info->verificationError() : 0),
            "</p><p>Qualcuno potrebbe leggere o cambiare quello che scambi con questo sito: non scrivere password o dati di pagamento.</p>"
            "<button onclick=\"location.reload()\">Riprova</button>"
            "<button class=\"risk\" onclick=\"location.href='lumiaweb:certificato'\">Continua comunque</button><table>"_s,
            info ? certificateDetails(*info) : String(), "</table><p class=\"detail\">"_s,
            escapeForHTML(error.localizedDescription()), " ("_s, escapeForHTML(error.domain()), ' ', error.errorCode(),
            ")</p></body></html>"_s);
    } else
    html = makeString("<!DOCTYPE html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Pagina non disponibile</title><style>"_s, errorPageStyle,
        "</style></head><body><h1>Impossibile aprire la pagina</h1><p class=\"host\">"_s,
        escapeForHTML(host.isEmpty() ? error.failingURL().string() : host), "</p><p>"_s, describeError(error),
        "</p><button onclick=\"location.reload()\">Riprova</button><p class=\"detail\">"_s,
        escapeForHTML(error.localizedDescription()), " ("_s, escapeForHTML(error.domain()), ' ', error.errorCode(),
        ")</p></body></html>"_s);
    auto utf8 = html.utf8();
    Ref buffer = SharedBuffer::create(utf8.span());
    ResourceResponse response(URL(), "text/html"_s, buffer->size(), "UTF-8"_s);
    URL failingURL = error.failingURL();
    SubstituteData substituteData(WTF::move(buffer), WTF::move(failingURL), WTF::move(response), SubstituteData::SessionHistoryVisibility::Hidden);
    frame->loader().setProvisionalLoadErrorBeingHandledURL(error.failingURL());
    frame->loader().load(FrameLoadRequest(*frame, ResourceRequest { URL { aboutBlankURL() } }, WTF::move(substituteData)));
    frame->loader().setProvisionalLoadErrorBeingHandledURL({ });
}

void WebPage::didTapInternalLink(const URL& link)
{
    if (link.path() != "certificato"_s || m_certificateErrorURL.isEmpty())
        return;
    auto url = std::exchange(m_certificateErrorURL, { });
    Lumia::log("certificato: accettato dall'utente per %s", url.host().toString().utf8().data());
    ResourceHandle::allowCertificateForWP81(url.host().toString());
    // Not from inside the policy check of the link itself.
    Lumia::postToWebThread([page = Ref { *this }, url = WTF::move(url)] {
        if (RefPtr frame = page->localMainFrame())
            frame->loader().load(FrameLoadRequest(*frame, ResourceRequest { URL { url } }));
    });
}

void WebPage::resize(const IntSize& size)
{
    if (size == m_size)
        return;
    m_size = size;
    setScreenSizeForWP81(FloatSize(m_size));
    if (RefPtr view = localMainFrameView())
        view->resize(m_size);
    if (m_layerTreeHost)
        m_layerTreeHost->sizeDidChange();
    if (m_viewportConfiguration.setViewLayoutSize(FloatSize(m_size)))
        viewportConfigurationChanged();
}

void WebPage::createFrameView(LocalFrame& frame)
{
    if (!frame.isMainFrame()) {
        frame.createView(m_size, m_backgroundColor, { }, false);
        return;
    }
    // A new document's view starts from the default viewport (980 px), not the previous page's: its own <meta
    // name=viewport> arrives while it parses, before its first layout (viewportPropertiesDidChange).
    m_viewportConfiguration.setDefaultConfiguration(ViewportConfiguration::webpageParameters());
    m_viewportConfiguration.setViewportArguments(ViewportArguments());
    frame.createView(m_size, m_backgroundColor, m_viewportConfiguration.layoutSize(), true);
}

// ---- viewport ----

namespace {

// Within 1%: as WebPageIOS's test, relative because a desktop page's fit is around 0.5.
bool scalesAreEssentiallyEqual(double a, double b)
{
    return std::abs(a - b) <= 0.01 * std::max(a, b);
}

} // namespace

void WebPage::didCommitLoad()
{
    RefPtr frame = localMainFrame();
    RefPtr document = frame ? frame->document() : nullptr;
    if (!document || !m_page)
        return;
    m_userHasChangedPageScaleFactor = false;
    // Any page but the certificate's error page itself (it commits as substitute data) ends its offer.
    if (RefPtr loader = frame->loader().documentLoader(); !loader || !loader->substituteData().isValid())
        m_certificateErrorURL = { };
    m_previousMinimumScale = std::nullopt;
    m_viewportScaleScheduled = false;

    // As WebPageIOS::resetViewportDefaultConfiguration. A text document lays out at the device's width (WebCore's
    // 320 is a placeholder outside iOS).
    if (document->isImageDocument())
        m_viewportConfiguration.setDefaultConfiguration(ViewportConfiguration::imageDocumentParameters());
    else if (document->isTextDocument()) {
        auto parameters = ViewportConfiguration::textDocumentParameters();
        parameters.width = m_size.width();
        m_viewportConfiguration.setDefaultConfiguration(parameters);
    } else
        m_viewportConfiguration.setDefaultConfiguration(ViewportConfiguration::webpageParameters());
    // Desktop mode (a user agent that is not a phone's: WhatsApp and Telegram Web, "versione desktop"): content laid out
    // wider than the page asked fits the screen on load, as Chrome's desktop site (overview mode). On phone pages,
    // Safari's rule: an initial-scale is kept, and the overflow is reached by zooming out (an off-screen menu must not
    // shrink the page).
    bool desktopMode = !Lumia::userAgent(m_view, document->url()).contains("Mobile"_s);
    m_viewportConfiguration.setCanIgnoreScalingConstraints(desktopMode);
    m_viewportConfiguration.setContentsSize(contentsSizeForViewport());
    m_viewportConfiguration.setViewportArguments(document->viewportArguments());
    m_viewportConfiguration.setIsKnownToLayOutWiderThanViewport(false);
    viewportConfigurationChanged(ZoomToInitialScale::Yes);
}

void WebPage::viewportPropertiesDidChange(const ViewportArguments& arguments)
{
    if (m_viewportConfiguration.setViewportArguments(arguments))
        viewportConfigurationChanged();
}

void WebPage::mainFrameDidLayout()
{
    if (!m_page)
        return;
    // A scale WebCore set by itself: Back and Forward restore the page's zoom with its scroll position
    // (HistoryController). It counts as the user's when it is not the page's initial one.
    float scale = m_page->pageScaleFactor();
    if (scale != m_appliedPageScale) {
        m_appliedPageScale = scale;
        m_userHasChangedPageScaleFactor = !scalesAreEssentiallyEqual(scale, m_viewportConfiguration.initialScale());
        updateCompositorZoom();
    }
    // Content wider (or narrower) than before changes the fit. Not applied from inside layout: the new scale lays the
    // page out again.
    if (m_viewportConfiguration.setContentsSize(contentsSizeForViewport()))
        scheduleViewportScale();
}

void WebPage::didUserScale()
{
    if (!m_page)
        return;
    m_userHasChangedPageScaleFactor = true;
    m_appliedPageScale = m_page->pageScaleFactor();
    updateCompositorZoom();
}

void WebPage::updateCompositorZoom()
{
    if (!m_page || !m_page->isVisible())
        return;
    ThreadedCompositor::setZoomLimits({ m_page->pageScaleFactor(), static_cast<float>(m_viewportConfiguration.minimumScale()),
        static_cast<float>(m_viewportConfiguration.maximumScale()), static_cast<float>(m_viewportConfiguration.initialScale()) });
}

// The document's size in CSS pixels, at least the layout viewport. The frame view's contentsSize is scaled by the page
// scale in this port (WebCore applies it itself), so it comes from the render view.
IntSize WebPage::contentsSizeForViewport() const
{
    RefPtr frame = localMainFrame();
    auto* renderView = frame ? frame->contentRenderer() : nullptr;
    return renderView ? renderView->unscaledDocumentRect().size() : IntSize();
}

// As WebPageIOS::viewportConfigurationChanged: the layout size now (before the next layout), the scale when it is safe
// to lay the page out (applyViewportScale).
void WebPage::viewportConfigurationChanged(ZoomToInitialScale zoomToInitialScale)
{
    RefPtr view = localMainFrameView();
    if (!view || !m_page)
        return;
    if (view->useFixedLayout())
        view->setFixedLayoutSize(m_viewportConfiguration.layoutSize());
    if (zoomToInitialScale == ZoomToInitialScale::Yes)
        m_userHasChangedPageScaleFactor = false;
    // Inside layout or style resolution (or while the fingers own the scale), later.
    RefPtr document = view->frame().document();
    if (view->layoutContext().isInLayout() || (document && document->inStyleRecalc()) || ThreadedCompositor::zoomGestureActive()) {
        scheduleViewportScale();
        return;
    }
    applyViewportScale();
}

void WebPage::scheduleViewportScale()
{
    if (std::exchange(m_viewportScaleScheduled, true))
        return;
    Lumia::postToWebThread([weakThis = WeakPtr { *this }] {
        RefPtr protectedThis = weakThis.get();
        if (!protectedThis || !std::exchange(protectedThis->m_viewportScaleScheduled, false))
            return;
        // Still zooming: the pinch's own end sets the scale (didUserScale).
        if (ThreadedCompositor::zoomGestureActive()) {
            protectedThis->updateCompositorZoom();
            return;
        }
        protectedThis->applyViewportScale();
    });
}

void WebPage::applyViewportScale()
{
    RefPtr view = localMainFrameView();
    if (!view || !m_page)
        return;
    double minimumScale = m_viewportConfiguration.minimumScale();
    double previousMinimumScale = std::exchange(m_previousMinimumScale, minimumScale).value_or(minimumScale);
    double currentScale = m_page->pageScaleFactor();
    double scale = m_viewportConfiguration.initialScale();
    if (m_userHasChangedPageScaleFactor) {
        // The user's zoom stays, within the new limits; fully zoomed out stays fully zoomed out.
        scale = scalesAreEssentiallyEqual(previousMinimumScale, currentScale) ? minimumScale
            : std::clamp(currentScale, minimumScale, m_viewportConfiguration.maximumScale());
    }
    // Every change lays the page out again: one under 0.3% (a pixel and a half across the screen) is not worth it.
    if (std::abs(scale - currentScale) > 0.003 * currentScale) {
        // The same document point stays at the top left: the scroll position is in scaled pixels here.
        FloatPoint origin = view->scrollPosition();
        origin.scale(scale / currentScale);
        m_page->setPageScaleFactor(scale, roundedIntPoint(origin));
        auto layoutSize = m_viewportConfiguration.layoutSize();
        auto contentsSize = m_viewportConfiguration.contentsSize();
        Lumia::log("viewport: scala %.3f (iniziale %.3f, min %.3f, max %.2f), layout %dx%d, contenuto %dx%d%s", scale,
            m_viewportConfiguration.initialScale(), minimumScale, m_viewportConfiguration.maximumScale(), layoutSize.width(),
            layoutSize.height(), contentsSize.width(), contentsSize.height(), m_userHasChangedPageScaleFactor ? ", dell'utente" : "");
    }
    m_appliedPageScale = m_page->pageScaleFactor();
    updateCompositorZoom();
}

// ---- rendering lifecycle ----

void WebPage::updateRendering()
{
    if (m_page)
        m_page->updateRendering();
}

void WebPage::finalizeRenderingUpdate(OptionSet<FinalizeRenderingUpdateFlags> flags)
{
    if (m_page)
        m_page->finalizeRenderingUpdate(flags);
}

void WebPage::didUpdateRendering()
{
    if (m_page)
        m_page->didUpdateRendering();
}

// ---- ChromeClient ----

void WebPage::attachRootGraphicsLayer(GraphicsLayer* layer)
{
    Lumia::log("WebPage: root layer %p", layer);
    if (m_layerTreeHost)
        m_layerTreeHost->setRootCompositingLayer(layer);
}

void WebPage::attachViewOverlayGraphicsLayer(GraphicsLayer* layer)
{
    if (m_layerTreeHost)
        m_layerTreeHost->setViewOverlayRootLayer(layer);
}

void WebPage::triggerRenderingUpdate()
{
    if (m_layerTreeHost)
        m_layerTreeHost->scheduleRenderingUpdate();
}

GraphicsLayerFactory* WebPage::graphicsLayerFactory() const
{
    return m_layerTreeHost ? m_layerTreeHost->graphicsLayerFactory() : nullptr;
}

RefPtr<DisplayRefreshMonitor> WebPage::createDisplayRefreshMonitor(PlatformDisplayID displayID)
{
    return m_layerTreeHost ? m_layerTreeHost->createDisplayRefreshMonitor(displayID) : nullptr;
}

// ---- network ----

// One session for every tab; cookies persist in data_directory\cookies.db (session cookies too with keep_session_cookies).
NetworkStorageSession& WebPage::storageSession()
{
    static NeverDestroyed<std::unique_ptr<NetworkStorageSession>> session = [] {
        auto session = makeUnique<NetworkStorageSession>(PAL::SessionID::defaultSessionID(), String());
        if (auto* dataDirectory = Lumia::config().data_directory) {
            auto database = makeUniqueRef<CookieJarDB>(FileSystem::pathByAppendingComponent(String(dataDirectory), "cookies.db"_s));
            // Before the database opens (on first use): opening deletes the session cookies unless they are kept.
            database->setKeepSessionCookies(Lumia::config().keep_session_cookies);
            session->setCookieDatabase(WTF::move(database));
        }
        return session;
    }();
    return *session.get();
}

} // namespace WebKit
