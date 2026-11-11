// WebCore::ChromeClient of a tab: compositing layers go to the page's LayerTreeHost, rendering updates are scheduled
// there, scrolling runs on the scrolling thread, geometry is the view size. Everything not listed keeps
// EmptyChromeClient's behaviour for now.
#pragma once

#include <WebCore/EmptyClients.h>
#include <wtf/MonotonicTime.h>
#include <wtf/WeakPtr.h>

namespace WebKit {

class WebPage;

class ChromeClient final : public WebCore::EmptyChromeClient {
public:
    explicit ChromeClient(WebPage&);

private:
    void chromeDestroyed() override { }

    WebCore::FloatRect windowRect() const override;
    WebCore::FloatRect pageRect() const override;
    WebCore::FloatSize screenSize() const override;
    WebCore::FloatSize availableScreenSize() const override;
    WebCore::IntPoint screenToRootView(const WebCore::IntPoint& point) const override { return point; }
    WebCore::IntPoint rootViewToScreen(const WebCore::IntPoint& point) const override { return point; }
    WebCore::IntRect rootViewToScreen(const WebCore::IntRect& rect) const override { return rect; }

    void addMessageToConsole(JSC::MessageSource, JSC::MessageLevel, const String& message, unsigned lineNumber, unsigned columnNumber, const String& sourceID) override;

    // The main document's <meta name=viewport> (WebPage::viewportPropertiesDidChange).
    void dispatchViewportPropertiesDidChange(const WebCore::ViewportArguments&) const override;

    // The page's dialogs (4e): the app shows them, the Web thread waits for the answer (Lumia::runDialog).
    void runJavaScriptAlert(WebCore::LocalFrame&, const String&) override;
    bool runJavaScriptConfirm(WebCore::LocalFrame&, const String&) override;
    bool runJavaScriptPrompt(WebCore::LocalFrame&, const String& message, const String& defaultValue, String& result) override;
    bool canRunBeforeUnloadConfirmPanel() override { return true; }
    bool runBeforeUnloadConfirmPanel(String&& message, WebCore::LocalFrame&) override;
    // window.open from a tap: a new tab of the app, with its opener (Lumia::createWindow).
    RefPtr<WebCore::Page> createWindow(WebCore::LocalFrame&, const String& openedMainFrameName, const WebCore::WindowFeatures&, const WebCore::NavigationAction&) override;
    // window.close(): the app closes the tab (LW_EVENT_CLOSE_WINDOW).
    void closeWindow() override;
    // <input type=file>: the app's file picker (LW_EVENT_FILE_CHOOSER), answered by lw_file_chooser_reply.
    void runOpenPanel(WebCore::LocalFrame&, WebCore::FileChooser&) override;
    // <select>, date and time inputs, <input type=color>: the app's pickers (LW_EVENT_PICKER, Pickers.cpp).
    RefPtr<WebCore::PopupMenu> createPopupMenu(WebCore::PopupMenuClient&) const override;
    RefPtr<WebCore::DateTimeChooser> createDateTimeChooser(WebCore::DateTimeChooserClient&) override;
    RefPtr<WebCore::ColorChooser> createColorChooser(WebCore::ColorChooserClient&, const WebCore::Color&) override;
    // navigator.share: the phone's share screen (LW_EVENT_SHARE).
    void showShareSheet(WebCore::ShareDataWithParsedURL&&, CompletionHandler<void(bool)>&&) override;
    // element.requestFullscreen(): the app hides its bars (LW_EVENT_FULLSCREEN) and the view grows to the screen.
    bool supportsFullScreenForElement(const WebCore::Element&, bool) override { return true; }
    void enterFullScreenForElement(WebCore::Element&, WebCore::HTMLMediaElementEnums::VideoFullscreenMode, CompletionHandler<void(WebCore::ExceptionOr<void>)>&& willEnterFullscreen, CompletionHandler<bool(bool)>&& didEnterFullscreen) override;
    void exitFullScreenForElement(WebCore::Element*, CompletionHandler<void()>&&) override;

    WebCore::GraphicsLayerFactory* graphicsLayerFactory() const override;
    WebCore::DisplayRefreshMonitorFactory* displayRefreshMonitorFactory() const override;
    void attachRootGraphicsLayer(WebCore::LocalFrame&, WebCore::GraphicsLayer*) override;
    void attachViewOverlayGraphicsLayer(WebCore::GraphicsLayer*) override;
    void setNeedsOneShotDrawingSynchronization() override { }
    void triggerRenderingUpdate() override;
    bool scheduleRenderingUpdate() override;

    RefPtr<WebCore::ScrollingCoordinator> createScrollingCoordinator(WebCore::Page&) const override;

    WeakPtr<WebPage> m_page;
    // Console rate limit (addMessageToConsole), Web thread.
    MonotonicTime m_consoleWindowStart;
    unsigned m_consoleCount { 0 };
    unsigned m_consoleDropped { 0 };
};

} // namespace WebKit
