#include "config.h"
#include "PlatformStrategies.h"

#include "loader/WebResourceLoadScheduler.h"
#include <WebCore/AudioDestination.h>
#include <WebCore/BlobRegistry.h>
#include <WebCore/BlobRegistryImpl.h>
#include <WebCore/MediaStrategy.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
using namespace WebCore;

void PlatformStrategies::initialize()
{
    static NeverDestroyed<PlatformStrategies> platformStrategies;
    setPlatformStrategies(&platformStrategies.get());
}

LoaderStrategy* PlatformStrategies::createLoaderStrategy()
{
    return new WebResourceLoadScheduler;
}

namespace {

class MediaStrategy final : public WebCore::MediaStrategy {
private:
#if ENABLE(WEB_AUDIO)
    Ref<AudioDestination> createAudioDestination(const AudioDestinationCreationOptions& options) override
    {
        return AudioDestination::create(options);
    }
#endif
};

// WebKitLegacy's WebBlobRegistry: every call goes straight to the in-process BlobRegistryImpl.
class BlobRegistry final : public WebCore::BlobRegistry {
    WTF_MAKE_TZONE_ALLOCATED(BlobRegistry);
    WTF_OVERRIDE_DELETE_FOR_CHECKED_PTR(BlobRegistry);
private:
    void registerInternalFileBlobURL(const URL& url, Ref<BlobDataFileReference>&& reference, const String&, const String& contentType) final { m_blobRegistry.registerInternalFileBlobURL(url, WTF::move(reference), contentType); }
    void registerInternalBlobURL(const URL& url, Vector<BlobPart>&& parts, const String& contentType) final { m_blobRegistry.registerInternalBlobURL(url, WTF::move(parts), contentType); }
    void registerBlobURL(const URL& url, const URL& srcURL, const PolicyContainer& policyContainer, const std::optional<SecurityOriginData>& topOrigin) final { m_blobRegistry.registerBlobURL(url, srcURL, policyContainer, topOrigin); }
    void registerInternalBlobURLOptionallyFileBacked(const URL& url, const URL& srcURL, RefPtr<BlobDataFileReference>&& reference, const String& contentType) final { m_blobRegistry.registerInternalBlobURLOptionallyFileBacked(url, srcURL, WTF::move(reference), contentType, { }); }
    void registerInternalBlobURLForSlice(const URL& url, const URL& srcURL, long long start, long long end, const String& contentType) final { m_blobRegistry.registerInternalBlobURLForSlice(url, srcURL, start, end, contentType); }
    void unregisterBlobURL(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final { m_blobRegistry.unregisterBlobURL(url, topOrigin); }
    String blobType(const URL& url) final { return m_blobRegistry.blobType(url); }
    unsigned long long blobSize(const URL& url) final { return m_blobRegistry.blobSize(url); }
    void writeBlobsToTemporaryFilesForIndexedDB(const Vector<String>& blobURLs, CompletionHandler<void(Vector<String>&& filePaths)>&& completionHandler) final { m_blobRegistry.writeBlobsToTemporaryFilesForIndexedDB(blobURLs, WTF::move(completionHandler)); }
    void registerBlobURLHandle(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final { m_blobRegistry.registerBlobURLHandle(url, topOrigin); }
    void unregisterBlobURLHandle(const URL& url, const std::optional<SecurityOriginData>& topOrigin) final { m_blobRegistry.unregisterBlobURLHandle(url, topOrigin); }

    BlobRegistryImpl* blobRegistryImpl() final { return &m_blobRegistry; }

    BlobRegistryImpl m_blobRegistry;
};

WTF_MAKE_TZONE_ALLOCATED_IMPL(BlobRegistry);

} // namespace

WebCore::MediaStrategy* PlatformStrategies::createMediaStrategy()
{
    return new MediaStrategy;
}

WebCore::BlobRegistry* PlatformStrategies::createBlobRegistry()
{
    return new BlobRegistry;
}

} // namespace WebKit
