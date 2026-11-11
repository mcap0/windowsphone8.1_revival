#include "config.h"
#include "ChromeClient.h"

#include "Engine.h"
#include "LayerTreeHost.h"
#include "Permissions.h"
#include "Pickers.h"
#include "ScrollingCoordinatorCoordinated.h"
#include "WebPage.h"
#include <WebCore/ColorChooser.h>
#include <WebCore/DateTimeChooser.h>
#include <WebCore/Document.h>
#include <WebCore/DocumentFullscreen.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/FileChooser.h>
#include <WebCore/FrameTree.h>
#include <WebCore/FrameInlines.h>
#include <WebCore/LocalFrame.h>
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/StorageNamespaceProvider.h>
#include <WebCore/WindowFeatures.h>
#include <WebCore/NavigationAction.h>
#include <WebCore/NodeDocument.h>
#include <WebCore/Page.h>
#include <WebCore/PopupMenu.h>
#include <WebCore/ShareData.h>

namespace WebKit {
using namespace WebCore;

ChromeClient::ChromeClient(WebPage& page)
    : m_page(page)
{
}

FloatRect ChromeClient::windowRect() const
{
    return m_page ? FloatRect(m_page->bounds()) : FloatRect();
}

FloatRect ChromeClient::pageRect() const
{
    return windowRect();
}

// The view size is in view pixels (XAML's), which is what CSS pixels are at zoom 1; the device scale factor maps them
// to the panel's physical pixels.
FloatSize ChromeClient::screenSize() const
{
    return m_page ? FloatSize(m_page->size()) : FloatSize();
}

FloatSize ChromeClient::availableScreenSize() const
{
    return screenSize();
}

void ChromeClient::addMessageToConsole(JSC::MessageSource, JSC::MessageLevel level, const String& message, unsigned lineNumber, unsigned, const String& sourceID)
{
    // The page's console and its uncaught errors, in the app log (this replaces the Web Inspector for now). Nothing at
    // all with the tracer off, and at most 30 lines a second: a page logging in a loop must not turn every call into
    // a formatted string, an event and a file write on the Web thread.
    if (!m_page || !Lumia::traceEnabled())
        return;
    static constexpr unsigned maxPerSecond = 30;
    auto now = MonotonicTime::now();
    if (now - m_consoleWindowStart >= 1_s) {
        if (m_consoleDropped)
            Lumia::log("console: %u messaggi saltati", m_consoleDropped);
        m_consoleWindowStart = now;
        m_consoleCount = 0;
        m_consoleDropped = 0;
    }
    if (++m_consoleCount > maxPerSecond) {
        m_consoleDropped++;
        return;
    }
    Lumia::postConsoleMessage(m_page->view(), static_cast<int>(level), message, makeString(sourceID, ':', lineNumber));
}

void ChromeClient::dispatchViewportPropertiesDidChange(const ViewportArguments& arguments) const
{
    if (m_page)
        m_page->viewportPropertiesDidChange(arguments);
}

void ChromeClient::runJavaScriptAlert(LocalFrame&, const String& message)
{
    if (m_page)
        Lumia::runDialog(m_page->view(), LW_DIALOG_ALERT, message, { });
}

bool ChromeClient::runJavaScriptConfirm(LocalFrame&, const String& message)
{
    return m_page && Lumia::runDialog(m_page->view(), LW_DIALOG_CONFIRM, message, { });
}

bool ChromeClient::runJavaScriptPrompt(LocalFrame&, const String& message, const String& defaultValue, String& result)
{
    auto answer = m_page ? Lumia::runDialog(m_page->view(), LW_DIALOG_PROMPT, message, defaultValue) : std::nullopt;
    if (!answer)
        return false;
    result = WTF::move(*answer);
    return true;
}

// "Leave this page?": true leaves. A tab that cannot show it (hidden) lets the navigation go on.
bool ChromeClient::runBeforeUnloadConfirmPanel(String&& message, LocalFrame&)
{
    if (!m_page)
        return true;
    if (!m_page->corePage() || !m_page->corePage()->isVisible())
        return true;
    return !!Lumia::runDialog(m_page->view(), LW_DIALOG_BEFOREUNLOAD, message, { });
}

// WebCore asks only for popups the user caused (JavaScriptCanOpenWindowsAutomatically is off, WebPage.cpp). The new
// window is a tab of the app with a page of its own, and WebCore loads the URL into it. As WebKitLegacy's
// WebChromeClient does, the window gets its opener (and a copy of its sessionStorage) here, before anything loads: its
// first document then belongs to the opener's origin, so the opener may navigate it (window.open('') then location,
// as sign-in libraries do), and a sign-in popup can post its result back and close itself.
RefPtr<Page> ChromeClient::createWindow(LocalFrame& frame, const String& openedMainFrameName, const WindowFeatures& features, const NavigationAction& action)
{
    if (!m_page || action.url().protocolIsJavaScript())
        return nullptr;
    RefPtr page = Lumia::createWindow(m_page->view(), action.url(), action.originalRequest().httpReferrer());
    if (!page)
        return nullptr;
    if (!features.wantsNoOpener()) {
        if (RefPtr openerPage = frame.page())
            openerPage->storageNamespaceProvider().cloneSessionStorageNamespaceForPage(*openerPage, *page);
        page->mainFrame().setOpenerForWebKitLegacy(&frame);
    }
    auto sandboxFlags = frame.effectiveSandboxFlags();
    if (!sandboxFlags.contains(SandboxFlag::PropagatesToAuxiliaryBrowsingContexts))
        sandboxFlags = { };
    page->mainFrame().updateSandboxFlags(sandboxFlags, Frame::NotifyUIProcess::No);
    if (RefPtr document = frame.document())
        page->mainFrame().updateReferrerPolicy(document->referrerPolicy());
    page->mainFrame().tree().setSpecifiedName(AtomString(openedMainFrameName));
    return page;
}

void ChromeClient::closeWindow()
{
    if (m_page)
        Lumia::postEvent(m_page->view(), LW_EVENT_CLOSE_WINDOW);
}

void ChromeClient::runOpenPanel(LocalFrame&, FileChooser& chooser)
{
    if (m_page)
        Lumia::runFileChooser(m_page->view(), chooser);
}

RefPtr<PopupMenu> ChromeClient::createPopupMenu(PopupMenuClient& client) const
{
    return m_page ? Lumia::createSelectPopup(m_page->view(), client) : nullptr;
}

RefPtr<DateTimeChooser> ChromeClient::createDateTimeChooser(DateTimeChooserClient& client)
{
    return m_page ? Lumia::createDateTimeChooser(m_page->view(), client) : nullptr;
}

RefPtr<ColorChooser> ChromeClient::createColorChooser(ColorChooserClient& client, const Color& color)
{
    return m_page ? Lumia::createColorChooser(m_page->view(), client, color) : nullptr;
}

void ChromeClient::showShareSheet(ShareDataWithParsedURL&& data, CompletionHandler<void(bool)>&& done)
{
    if (!m_page)
        return done(false);
    Lumia::share(m_page->view(), data, WTF::move(done));
}

// The two halves of entering are the document's own steps (the element becomes the fullscreen element, then the
// fullscreenchange event); between them the app is told, and the view's new size comes as an ordinary resize. There is
// no animation to wait for.
void ChromeClient::enterFullScreenForElement(Element& element, HTMLMediaElementEnums::VideoFullscreenMode mode, CompletionHandler<void(ExceptionOr<void>)>&& willEnterFullscreen, CompletionHandler<bool(bool)>&& didEnterFullscreen)
{
    if (!m_page) {
        willEnterFullscreen(Exception { ExceptionCode::InvalidStateError });
        didEnterFullscreen(false);
        return;
    }
    auto result = Ref { element.document().fullscreen() }->willEnterFullscreen(element, mode);
    if (result.hasException()) {
        willEnterFullscreen(WTF::move(result));
        didEnterFullscreen(false);
        return;
    }
    willEnterFullscreen({ });
    Lumia::log("schermo intero: si");
    Lumia::postEvent(m_page->view(), LW_EVENT_FULLSCREEN, 1);
    didEnterFullscreen(true);
}

void ChromeClient::exitFullScreenForElement(Element* element, CompletionHandler<void()>&& done)
{
    if (element)
        Ref { element->document().fullscreen() }->willExitFullscreen();
    if (m_page) {
        Lumia::log("schermo intero: no");
        Lumia::postEvent(m_page->view(), LW_EVENT_FULLSCREEN, 0);
    }
    done();
}

GraphicsLayerFactory* ChromeClient::graphicsLayerFactory() const
{
    return m_page ? m_page->graphicsLayerFactory() : nullptr;
}

DisplayRefreshMonitorFactory* ChromeClient::displayRefreshMonitorFactory() const
{
    return m_page.get();
}

void ChromeClient::attachRootGraphicsLayer(LocalFrame&, GraphicsLayer* layer)
{
    if (m_page)
        m_page->attachRootGraphicsLayer(layer);
}

void ChromeClient::attachViewOverlayGraphicsLayer(GraphicsLayer* layer)
{
    if (m_page)
        m_page->attachViewOverlayGraphicsLayer(layer);
}

void ChromeClient::triggerRenderingUpdate()
{
    if (m_page)
        m_page->triggerRenderingUpdate();
}

bool ChromeClient::scheduleRenderingUpdate()
{
    // false: the page's own RenderingUpdateScheduler decides when the next update runs, and it is what aligns
    // rendering updates (and requestAnimationFrame) with the display refresh. Triggering the flush from here made the
    // page update in a tight loop (around a thousand times a second on the phone), saturating the Web thread and
    // starving the raster workers, which showed up as stutter and white patches while scrolling.
    return false;
}

// The page's scrolling tree lives on the scrolling thread (ThreadedScrollingTree): pans and fling move layers there
// while the Web thread is busy.
RefPtr<ScrollingCoordinator> ChromeClient::createScrollingCoordinator(Page&) const
{
    if (!m_page)
        return nullptr;
    return ScrollingCoordinatorCoordinated::create(m_page.get());
}

} // namespace WebKit
