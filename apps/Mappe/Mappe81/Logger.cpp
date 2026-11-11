#include "pch.h"
#include "Logger.h"

using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
using namespace Windows::Web::Http;

namespace {
SRWLOCK g_lock = SRWLOCK_INIT;
std::wstring g_buffer;
String^ g_url;
DispatcherTimer^ g_timer;
bool g_sending;

std::wstring Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[32];
    swprintf_s(b, L"%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return b;
}

void Send(String^ url, String^ body, bool releaseFlag) {
    auto client = ref new HttpClient();
    auto content = ref new HttpStringContent(body);
    concurrency::create_task(client->PostAsync(ref new Uri(url), content)).then([client, releaseFlag](concurrency::task<HttpResponseMessage^> t) {
        try { t.get(); } catch (Exception^) {}
        if (releaseFlag) g_sending = false;
    });
}

void Flush(Object^, Object^) {
    if (g_sending || g_url == nullptr) return;
    std::wstring text;
    AcquireSRWLockExclusive(&g_lock);
    text.swap(g_buffer);
    ReleaseSRWLockExclusive(&g_lock);
    if (text.empty()) return;
    g_sending = true;
    Send(g_url, ref new String(text.c_str()), true);
}
}  // namespace

void Logger::Write(const std::wstring& line) {
    std::wstring l = Stamp() + line;
    if (l.empty() || l.back() != L'\n') l += L"\n";
    OutputDebugStringW(l.c_str());
    AcquireSRWLockExclusive(&g_lock);
    if (g_buffer.size() < 512 * 1024) g_buffer += l;
    ReleaseSRWLockExclusive(&g_lock);
}

void Logger::WriteUtf8(const char* line) {
    if (!line) return;
    int n = MultiByteToWideChar(CP_UTF8, 0, line, -1, nullptr, 0);
    if (n <= 1) return;
    std::wstring w(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, line, -1, &w[0], n);
    while (!w.empty() && (w.back() == L'\n' || w.back() == L'\r')) w.pop_back();
    Write(w);
}

void Logger::Start(String^ inboxUrl) {
    g_url = inboxUrl;
    if (g_timer != nullptr) return;
    g_timer = ref new DispatcherTimer();
    TimeSpan every;
    every.Duration = 3LL * 10000000LL;
    g_timer->Interval = every;
    g_timer->Tick += ref new EventHandler<Object^>(&Flush);
    g_timer->Start();
}

void Logger::PostNow(String^ name, String^ body) {
    if (g_url == nullptr) return;
    std::wstring base(g_url->Data());
    size_t slash = base.find_last_of(L'/');
    std::wstring url = base.substr(0, slash + 1) + std::wstring(name->Data());
    Send(ref new String(url.c_str()), body, false);
}
