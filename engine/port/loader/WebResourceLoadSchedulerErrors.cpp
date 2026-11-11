// The loader's error objects (WebKitLegacy keeps them in WebResourceLoadScheduler.mm, Cocoa only): same domains and
// codes as WebKit2's WebErrors.cpp, so pages and WebCore see the usual WebKit errors.
#include "config.h"
#include "WebResourceLoadScheduler.h"

#include <WebCore/ResourceError.h>
#include <WebCore/ResourceRequest.h>
#include <WebCore/ResourceResponse.h>

using namespace WebCore;

namespace {

constexpr ASCIILiteral networkDomain = "WebKitNetworkError"_s;
constexpr ASCIILiteral policyDomain = "WebKitPolicyError"_s;
constexpr ASCIILiteral pluginDomain = "WebKitPluginError"_s;

enum : int {
    CannotShowMIMEType = 100,
    CannotShowURL = 101,
    FrameLoadInterruptedByPolicyChange = 102,
    CannotUseRestrictedPort = 103,
    FrameLoadBlockedByContentBlocker = 104,
    FrameLoadBlockedByContentFilter = 105,
    PlugInWillHandleLoad = 204,
    Cancelled = 302,
    FileDoesNotExist = 303,
    HTTPSUpgradeRedirectLoop = 304,
    HTTPNavigationWithHTTPSOnlyError = 305,
};

} // namespace

ResourceError WebResourceLoadScheduler::cancelledError(const ResourceRequest& request) const
{
    // A cancellation, so FrameLoaderClient and the app don't treat a stopped load as a failure.
    return ResourceError(networkDomain, Cancelled, request.url(), "Load request cancelled"_s, ResourceError::Type::Cancellation);
}

ResourceError WebResourceLoadScheduler::blockedError(const ResourceRequest& request) const
{
    return ResourceError(policyDomain, CannotUseRestrictedPort, request.url(), "Not allowed to use restricted network port"_s);
}

bool WebResourceLoadScheduler::isBlockedError(const ResourceError& error) const
{
    return error.domain() == policyDomain && error.errorCode() == CannotUseRestrictedPort;
}

ResourceError WebResourceLoadScheduler::blockedByContentBlockerError(const ResourceRequest& request) const
{
    return ResourceError(policyDomain, FrameLoadBlockedByContentBlocker, request.url(), "The URL was blocked by a content blocker"_s);
}

ResourceError WebResourceLoadScheduler::cannotShowURLError(const ResourceRequest& request) const
{
    return ResourceError(policyDomain, CannotShowURL, request.url(), "The URL can't be shown"_s);
}

ResourceError WebResourceLoadScheduler::interruptedForPolicyChangeError(const ResourceRequest& request) const
{
    return ResourceError(policyDomain, FrameLoadInterruptedByPolicyChange, request.url(), "Frame load interrupted"_s);
}

#if ENABLE(CONTENT_FILTERING)
ResourceError WebResourceLoadScheduler::blockedByContentFilterError(const ResourceRequest& request) const
{
    return ResourceError(policyDomain, FrameLoadBlockedByContentFilter, request.url(), "The URL was blocked by a content filter"_s);
}
#endif

ResourceError WebResourceLoadScheduler::cannotShowMIMETypeError(const ResourceResponse& response) const
{
    return ResourceError(policyDomain, CannotShowMIMEType, response.url(), "Content with specified MIME type can't be shown"_s);
}

ResourceError WebResourceLoadScheduler::fileDoesNotExistError(const ResourceResponse& response) const
{
    return ResourceError(networkDomain, FileDoesNotExist, response.url(), "File does not exist"_s);
}

ResourceError WebResourceLoadScheduler::httpsUpgradeRedirectLoopError(const ResourceRequest& request) const
{
    return ResourceError(networkDomain, HTTPSUpgradeRedirectLoop, request.url(), "HTTPS Upgrade redirect loop detected"_s);
}

ResourceError WebResourceLoadScheduler::httpNavigationWithHTTPSOnlyError(const ResourceRequest& request) const
{
    return ResourceError(networkDomain, HTTPNavigationWithHTTPSOnlyError, request.url(), "Navigation failed because the request was for an HTTP URL with HTTPS-Only enabled"_s);
}

bool WebResourceLoadScheduler::isHttpNavigationWithHTTPSOnlyError(const ResourceError& error) const
{
    return error.domain() == networkDomain && error.errorCode() == HTTPNavigationWithHTTPSOnlyError;
}

ResourceError WebResourceLoadScheduler::pluginWillHandleLoadError(const ResourceResponse& response) const
{
    return ResourceError(pluginDomain, PlugInWillHandleLoad, response.url(), "Plug-in handled load"_s);
}
