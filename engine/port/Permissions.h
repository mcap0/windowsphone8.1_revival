// What a page may only do after the user said yes, and the phone's services behind it: the permission question
// (LW_EVENT_PERMISSION), geolocation (the app's Geolocator feeds lw_geolocation_update), notifications (the app shows
// them, LW_EVENT_NOTIFICATION) and navigator.share (LW_EVENT_SHARE). Web thread.
#pragma once

#include "lumiaweb.h"
#include <wtf/CompletionHandler.h>

namespace WebCore {
class Page;
struct ShareDataWithParsedURL;
}

namespace Lumia {

// A new tab's page gets its geolocation and notification clients.
void provideDeviceFeatures(lw_view&, WebCore::Page&);
// lw_view_destroy: the view's open questions are refused (not remembered), its position watch and notifications end.
void deviceFeaturesViewWillClose(lw_view&);
// lw_clear_data (privateOnly false: the normal tabs' answers), or the last private tab closed (true: the private ones).
void forgetPermissions(bool privateOnly);
// navigator.share: the app's share screen. True once it is asked to open.
void share(lw_view&, const WebCore::ShareDataWithParsedURL&, CompletionHandler<void(bool)>&&);

} // namespace Lumia
