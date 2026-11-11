// Downloads (4d). A response the engine does not show (an attachment, a type it cannot display) becomes a download where
// it stands: the page's load hands its ResourceHandle over (convertToDownload), so nothing is requested twice and a
// POST, a one-time link or the page's own blob: all work. A link with the download attribute, a data: URL and the
// app's "save" (lw_view_download) start a load of their own, with the page's cookies. Files go to
// lw_config.download_directory; the app follows them through LW_EVENT_DOWNLOAD, LW_EVENT_DOWNLOAD_PROGRESS and
// LW_EVENT_DOWNLOAD_FINISHED, also after the tab that started them is gone. Web thread.
#pragma once

#include <wtf/Forward.h>

struct lw_view;

namespace WebCore {
class NetworkingContext;
class ResourceHandle;
class ResourceRequest;
class ResourceResponse;
}

namespace Lumia {

// The page's load of `response` goes on into a file: from now on the handle's data goes to the download.
void convertToDownload(lw_view&, WebCore::ResourceHandle&, const WebCore::ResourceRequest&, const WebCore::ResourceResponse&);
// A load of its own saved as a file. `suggestedName` (the download attribute) wins over the server's name.
void startDownload(lw_view&, WebCore::NetworkingContext*, const WebCore::ResourceRequest&, const String& suggestedName);
void cancelDownload(int id);

} // namespace Lumia
