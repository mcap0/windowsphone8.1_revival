// WebCore's process-wide strategies for a single-process engine: loads go through WebResourceLoadScheduler (curl
// ResourceHandles, from WebKitLegacy) until our network-thread loader (phase 4), blobs through WebCore's
// BlobRegistryImpl, as WebKitLegacy does. No pasteboard strategy: the Win Pasteboard does not use one.
#pragma once

#include <WebCore/PlatformStrategies.h>

namespace WebKit {

class PlatformStrategies final : public WebCore::PlatformStrategies {
public:
    static void initialize();

private:
    WebCore::LoaderStrategy* createLoaderStrategy() override;
    WebCore::PasteboardStrategy* createPasteboardStrategy() override { return nullptr; }
    WebCore::MediaStrategy* createMediaStrategy() override;
    WebCore::BlobRegistry* createBlobRegistry() override;
#if ENABLE(DECLARATIVE_WEB_PUSH)
    WebCore::PushStrategy* createPushStrategy() override { return nullptr; }
#endif
};

} // namespace WebKit
