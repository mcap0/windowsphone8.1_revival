#include "CastLog.h"
#include "WinRtText.h"

#include <ppltasks.h>
#include <windows.system.h>
#include <windows.web.http.h>

using namespace Windows::Foundation;

namespace PerleCast {

void CastLogLine(const std::string& text)
{
    try
    {
        std::string line = text + " | mem " +
            std::to_string(Windows::System::MemoryManager::AppMemoryUsage / (1024 * 1024)) + "/" +
            std::to_string(Windows::System::MemoryManager::AppMemoryUsageLimit / (1024 * 1024)) + " MB";
        Windows::Web::Http::HttpClient^ client = ref new Windows::Web::Http::HttpClient();
        concurrency::create_task(client->PostAsync(ref new Uri("http://192.168.1.148:8765/perletv"),
            ref new Windows::Web::Http::HttpStringContent(FromUtf8(line)))).then([client](concurrency::task<Windows::Web::Http::HttpResponseMessage^> t)
        {
            try { t.get(); } catch (...) { }
        });
    }
    catch (...) { }
}

} // namespace PerleCast
