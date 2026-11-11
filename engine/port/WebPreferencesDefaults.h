// WebKit's embedder defaults for WebCore::Settings (JavaScript on, modern features...), generated at build time from
// UnifiedWebPreferences.yaml by engine/tools/genpreferences.rb. WebPage applies them, then the port's own settings.
#pragma once

namespace WebCore {
class Settings;
}

namespace WebKit {

void applyWebKitDefaults(WebCore::Settings&);

} // namespace WebKit
