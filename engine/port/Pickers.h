// The form controls whose choice is made outside the page: <select>, the date and time inputs, <input type=color>.
// WebCore asks the ChromeClient for a popup or chooser object; ours sends LW_EVENT_PICKER to the app, which shows its
// own list or the phone's date and time pickers, and answers with lw_picker_reply. Web thread only.
#pragma once

#include "lumiaweb.h"
#include <wtf/RefPtr.h>

namespace WebCore {
class Color;
class ColorChooser;
class ColorChooserClient;
class DateTimeChooser;
class DateTimeChooserClient;
class PopupMenu;
class PopupMenuClient;
}

namespace Lumia {

RefPtr<WebCore::PopupMenu> createSelectPopup(lw_view&, WebCore::PopupMenuClient&);
RefPtr<WebCore::DateTimeChooser> createDateTimeChooser(lw_view&, WebCore::DateTimeChooserClient&);
RefPtr<WebCore::ColorChooser> createColorChooser(lw_view&, WebCore::ColorChooserClient&, const WebCore::Color&);
// A picker of the page is on the app's screen (Chrome::hasOpenedPopup).
bool hasOpenPicker();

} // namespace Lumia
