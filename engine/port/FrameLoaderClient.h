// WebCore::LocalFrameLoaderClient of every frame of a tab. Starts from EmptyFrameLoaderClient (MetroSurf's overlay
// makes its methods overridable) and implements what loading pages needs: policy answers, the frame view on commit,
// subframes, the networking context, the User-Agent and the load events for the app.
#pragma once

#include <WebCore/Document.h>
#include <WebCore/EmptyFrameLoaderClient.h>
#include <wtf/WeakPtr.h>

namespace WebKit {

class WebPage;

class FrameLoaderClient final : public WebCore::EmptyFrameLoaderClient {
public:
    FrameLoaderClient(WebCore::LocalFrame&, WebCore::FrameLoader&, WebPage&);

    // The page a navigation leaves is paused until the new one commits, and runs again if it stays. Also from the
    // leave trap (Engine.cpp), before the navigation has started.
    void suspendLeavingDocuments();
    void resumeLeavingDocuments();

private:
    bool isEmptyFrameLoaderClient() const override { return false; }

    void dispatchDecidePolicyForNavigationAction(const WebCore::NavigationAction&, const WebCore::ResourceRequest&, const WebCore::ResourceResponse& redirectResponse,
        WebCore::FormState*, const String&, std::optional<WebCore::NavigationIdentifier>, std::optional<WebCore::HitTestResult>&&, bool,
        WebCore::NavigationUpgradeToHTTPSBehavior, WebCore::SandboxFlags, WebCore::PolicyDecisionMode, WebCore::FramePolicyFunction&&) override;
    void dispatchDecidePolicyForNewWindowAction(const WebCore::NavigationAction&, const WebCore::ResourceRequest&, WebCore::FormState*, const String&,
        std::optional<WebCore::HitTestResult>&&, WebCore::FramePolicyFunction&&) override;
    void dispatchDecidePolicyForResponse(const WebCore::ResourceResponse&, const WebCore::ResourceRequest&, const String&, WebCore::FramePolicyFunction&&) override;
    // Downloads (Downloads.cpp).
    void convertMainResourceLoadToDownload(WebCore::DocumentLoader*, const WebCore::ResourceRequest&, const WebCore::ResourceResponse&) override;
    void startDownload(const WebCore::ResourceRequest&, const String& suggestedName, WebCore::FromDownloadAttribute) override;
    bool canHandleRequest(const WebCore::ResourceRequest&) const override { return true; }
    bool canShowMIMEType(const String&) const override;
    bool canShowMIMETypeAsHTML(const String&) const override { return true; }
    bool shouldUseCredentialStorage(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier) override { return true; }
    // A 401 nobody has a password for: the app asks the user (Authentication.cpp).
    void dispatchDidReceiveAuthenticationChallenge(WebCore::DocumentLoader*, WebCore::ResourceLoaderIdentifier, const WebCore::AuthenticationChallenge&) override;
    bool shouldFallBack(const WebCore::ResourceError&) const override;
    // Single process: history navigations need no one's permission.
    WebCore::ShouldGoToHistoryItem shouldGoToHistoryItem(WebCore::HistoryItem&, WebCore::IsSameDocumentNavigation) const override { return WebCore::ShouldGoToHistoryItem::Yes; }
    // Session history and the back/forward cache (4c).
    RefPtr<WebCore::HistoryItem> createHistoryItemTree(bool clipAtTarget, WebCore::BackForwardItemIdentifier) const override;
    bool canCachePage() const override { return true; }
    void didCacheBackForwardItem(WebCore::BackForwardItemIdentifier, WebCore::BackForwardFrameItemIdentifier) override;

    void committedLoad(WebCore::DocumentLoader*, const WebCore::SharedBuffer&) override;
    WebCore::ObjectContentType objectContentType(const URL&, const String& mimeType) override;

    void transitionToCommittedForNewPage(InitializingIframe) override;
    RefPtr<WebCore::LocalFrame> createFrame(const AtomString& name, WebCore::HTMLFrameOwnerElement&) override;
    Ref<WebCore::FrameNetworkingContext> createNetworkingContext() override;
    String userAgent(const URL&) const override;

    void dispatchDidStartProvisionalLoad() override;
    void dispatchDidReceiveTitle(const WebCore::StringWithDirection&) override;
    void dispatchDidCommitLoad(std::optional<WebCore::HasInsecureContent>, std::optional<WebCore::UsedLegacyTLS>, std::optional<WebCore::WasPrivateRelayed>) override;
    void dispatchDidChangeLocationWithinPage() override;
    // After every layout of the main frame: its content size feeds the viewport (WebPage::mainFrameDidLayout).
    void dispatchDidLayout() override;
    void dispatchDidFinishLoad() override;
    void dispatchDidFailLoad(const WebCore::ResourceError&) override;
    void dispatchDidFailProvisionalLoad(const WebCore::ResourceError&, WebCore::WillContinueLoading, WebCore::WillInternallyHandleFailure) override;

    bool isMainFrame() const;

    WeakRef<WebCore::LocalFrame> m_localFrame;
    WeakPtr<WebPage> m_page;
    // The page a main-frame navigation is leaving, paused until the new one commits (suspendLeavingDocuments).
    Vector<WeakPtr<WebCore::Document, WebCore::WeakPtrImplWithEventTargetData>> m_suspendedDocuments;
};

} // namespace WebKit
