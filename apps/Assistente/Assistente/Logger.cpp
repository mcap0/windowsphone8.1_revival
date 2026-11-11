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
HANDLE g_file = INVALID_HANDLE_VALUE;  // LocalFolder\log.txt: survives a crash, sent at the next start

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
    if (g_file != INVALID_HANDLE_VALUE) {
        int n = WideCharToMultiByte(CP_UTF8, 0, l.c_str(), static_cast<int>(l.size()), nullptr, 0, nullptr, nullptr);
        std::string u(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, l.c_str(), static_cast<int>(l.size()), &u[0], n, nullptr, nullptr);
        DWORD written = 0;
        WriteFile(g_file, u.data(), static_cast<DWORD>(u.size()), &written, nullptr);
        FlushFileBuffers(g_file);
    }
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
    // the previous session's file: if the app died, its last lines are there
    std::wstring path = std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) + L"\\log.txt";
    std::string previous;
    HANDLE old = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, nullptr);
    if (old != INVALID_HANDLE_VALUE) {
        char buf[16384];
        DWORD got = 0;
        while (ReadFile(old, buf, sizeof(buf), &got, nullptr) && got > 0 && previous.size() < 256 * 1024) previous.append(buf, got);
        CloseHandle(old);
    }
    AcquireSRWLockExclusive(&g_lock);
    g_file = CreateFile2(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, CREATE_ALWAYS, nullptr);
    ReleaseSRWLockExclusive(&g_lock);
    if (!previous.empty()) {
        int n = MultiByteToWideChar(CP_UTF8, 0, previous.data(), static_cast<int>(previous.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, previous.data(), static_cast<int>(previous.size()), &w[0], n);
        Send(g_url, ref new String((L"--- sessione precedente ---\n" + w + L"--- fine sessione precedente ---\n").c_str()), false);
    }
    g_timer = ref new DispatcherTimer();
    TimeSpan every;
    every.Duration = 3LL * 10000000LL;
    g_timer->Interval = every;
    g_timer->Tick += ref new EventHandler<Object^>(&Flush);
    g_timer->Start();
}

void Logger::FlushNow() {
    Flush(nullptr, nullptr);
}

void Logger::PostNow(String^ name, String^ body) {
    if (g_url == nullptr) return;
    std::wstring base(g_url->Data());
    size_t slash = base.find_last_of(L'/');
    std::wstring url = base.substr(0, slash + 1) + std::wstring(name->Data());
    Send(ref new String(url.c_str()), body, false);
}
