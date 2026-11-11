// One browser tab on the Web thread: the WebCore::Page, its main frame, and the LayerTreeHost that hands the page's
// layers to the compositor thread. The compositor sources (compositor/, from WebKit2's PlayStation port) know this
// class by the name and methods of WebKit2's WebPage, so it keeps them.
#pragma once

#include <WebCore/Color.h>
#include <WebCore/DisplayRefreshMonitorFactory.h>
#include <WebCore/FloatRect.h>
#include <WebCore/IntRect.h>
#include <WebCore/Page.h>
#include <WebCore/SecurityOriginData.h>
#include <WebCore/ViewportConfiguration.h>
#include <optional>
#include <wtf/CheckedPtr.h>
#include <wtf/RefCounted.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakPtr.h>
#include <wtf/text/WTFString.h>

struct lw_view;

namespace WebCore {
class GraphicsLayer;
class GraphicsLayerFactory;
class LocalFrame;
class LocalFrameView;
class NetworkStorageSession;
class ResourceError;
}

namespace Lumia {
class BackForwardList;
}

namespace WebKit {

class LayerTreeHost;

class WebPage final : public RefCounted<WebPage>, public CanMakeWeakPtr<WebPage>, public WebCore::DisplayRefreshMonitorFactory {
    WTF_MAKE_TZONE_ALLOCATED(WebPage);
public:
    static Ref<WebPage> create(lw_view&, void* swapChainPanel, const WebCore::IntSize&, float deviceScaleFactor);
    ~WebPage();

    void close();

    lw_view& view() const { return m_view; }
    WebCore::Page* corePage() const { return m_page.get(); }
    WebCore::LocalFrame* localMainFrame() const;
    WebCore::LocalFrameView* localMainFrameView() const;
    LayerTreeHost* layerTreeHost() const { return m_layerTreeHost.get(); }
    Lumia::BackForwardList& backForwardList() const { return m_backForwardList.get(); }

    const WebCore::IntSize& size() const { return m_size; }
    WebCore::IntRect bounds() const { return { { }, m_size }; }
    float deviceScaleFactor() const { return m_deviceScaleFactor; }
    const std::optional<WebCore::Color>& backgroundColor() const { return m_backgroundColor; }
    void* swapChainPanel() const { return m_swapChainPanel; }

    void loadURL(const String&);
    void resize(const WebCore::IntSize&);

    // The last lw_text_edit sequence applied to this page: LW_EVENT_FIELD_UPDATE reports it, so the app can tell an
    // echo of its own edit (sequence == the one it sent) from a change the page made by itself (sequence older).
    uint32_t lastTextSequence() const { return m_lastTextSequence; }
    void setLastTextSequence(uint32_t sequence) { m_lastTextSequence = sequence; }

    // True while lw_text_edit applies an edit: the editor client skips the intermediate selection/content updates that
    // the edit itself triggers, and reports one FIELD_UPDATE at the end.
    bool textEditInProgress() const { return m_textEditInProgress; }
    void setTextEditInProgress(bool inProgress) { m_textEditInProgress = inProgress; }

    // Rendering lifecycle, called by LayerTreeHost (same names as in WebKit2's WebPage).
    void updateRendering();
    void finalizeRenderingUpdate(OptionSet<WebCore::FinalizeRenderingUpdateFlags>);
    void didUpdateRendering();
    void flushPendingEditorStateUpdate() { }

    // Called by the ChromeClient.
    void attachRootGraphicsLayer(WebCore::GraphicsLayer*);
    void attachViewOverlayGraphicsLayer(WebCore::GraphicsLayer*);
    void triggerRenderingUpdate();
    WebCore::GraphicsLayerFactory* graphicsLayerFactory() const;

    // Called by the FrameLoaderClient when a load commits in any frame of the page: the frame gets its view here
    // (subframes are then sized by their RenderWidget).
    void createFrameView(WebCore::LocalFrame&);

    // WebCore::DisplayRefreshMonitorFactory: rAF and animations follow the compositor's frames.
    RefPtr<WebCore::DisplayRefreshMonitor> createDisplayRefreshMonitor(WebCore::PlatformDisplayID) override;

    static WebCore::NetworkStorageSession& storageSession();

    // lw_view_clear_site_data: the page first goes to about:blank, so its document closes its own IndexedDB
    // connections and storage areas (deleting databases a live page holds open aborted in IDBServer); when that blank
    // page commits (FrameLoaderClient), the site's data goes and its address loads again from the network.
    void clearSiteDataAndReload();
    // The leave trap (Engine.cpp): the page being left is paused at once, and runs again if no navigation took it away.
    void suspendLeavingDocuments();
    void resumeLeavingDocuments();
    void didCommitBlankForSiteClear();

    // A main-frame navigation failed before its page arrived: an error page in its place, with the failed address as
    // the page's unreachable URL (reload tries it again), as WebKit2's loadAlternateHTML.
    void loadErrorPage(const WebCore::ResourceError&);
    // A lumiaweb: link of that page, tapped by the user (FrameLoaderClient's navigation policy). "lumiaweb:certificato":
    // the user accepts the certificate the page describes; its host loads without the check until the app ends.
    void didTapInternalLink(const URL&);

    // ---- viewport, as Safari on an iPhone (WebKit2's WebPageIOS on WebCore's ViewportConfiguration) ----
    // The main frame lays out at the width its <meta name=viewport> asks for (device-width = the view's 480 px), or
    // 980 px without one, as a desktop window; the page scale starts at the page's initial-scale or at the one that
    // fits its content's width, and pinch zoom goes from that fit (or the page's minimum-scale) to 5x, whatever
    // user-scalable says. Sites in desktop mode (a desktop user agent) also fit content wider than their layout on
    // load, as Chrome's "desktop site". Before: every page laid out 480 px wide at scale 1 with limits 1-4, so desktop
    // pages (Gmail, WhatsApp Web) were cut on the right and zooming out sprang back (2026-10-28).
    // Web thread. The page scale is applied by WebCore here (not delegated), so changing it lays the page out again.
    void viewportPropertiesDidChange(const WebCore::ViewportArguments&);
    // FrameLoaderClient: a new document in the main frame (back/forward included), and after each of its layouts.
    void didCommitLoad();
    void mainFrameDidLayout();
    // A pinch or double tap just set the page scale (Engine.cpp): from now on the page keeps the user's zoom.
    void didUserScale();
    // The visible tab's scale and limits go to the compositor, where the fingers are handled (lw_view_set_visible).
    void updateCompositorZoom();

private:
    WebPage(lw_view&, void* swapChainPanel, const WebCore::IntSize&, float deviceScaleFactor);

    enum class ZoomToInitialScale : bool { No, Yes };
    void viewportConfigurationChanged(ZoomToInitialScale = ZoomToInitialScale::No);
    void applyViewportScale();
    void scheduleViewportScale();
    WebCore::IntSize contentsSizeForViewport() const;

    lw_view& m_view;
    void* m_swapChainPanel;
    WebCore::IntSize m_size;
    float m_deviceScaleFactor;
    std::optional<WebCore::Color> m_backgroundColor { WebCore::Color::white };
    Ref<Lumia::BackForwardList> m_backForwardList;
    RefPtr<WebCore::Page> m_page;
    std::unique_ptr<LayerTreeHost> m_layerTreeHost;
    uint32_t m_lastTextSequence { 0 };
    bool m_textEditInProgress { false };
    struct PendingSiteClear {
        WebCore::SecurityOriginData origin;
        URL url;
    };
    std::optional<PendingSiteClear> m_pendingSiteClear;

    WebCore::ViewportConfiguration m_viewportConfiguration;
    bool m_userHasChangedPageScaleFactor { false };
    // The address whose certificate error page is on the screen (empty: none).
    URL m_certificateErrorURL;
    // The minimum scale at the last change: a page the user left fully zoomed out stays so as its content grows.
    std::optional<double> m_previousMinimumScale;
    // The scale this class last applied or saw: anything else was set by WebCore itself (Back restores a page's zoom).
    float m_appliedPageScale { 1 };
    bool m_viewportScaleScheduled { false };
};

} // namespace WebKit
