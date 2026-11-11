#define NOMINMAX
#include "Shared.h"

#include <windows.h>
#include <ppltasks.h>
#include <mutex>
#include <string>

using namespace Platform;
using namespace Windows::Data::Xml::Dom;
using namespace Windows::Foundation;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage;
using namespace Windows::System;
using namespace Windows::System::Threading;
using namespace Windows::UI::Notifications;
using namespace Windows::Web::Http;

namespace PostaTask
{
namespace
{
    std::mutex g_logLock;
    volatile long g_inboxPending = 0;       // log POSTs still on the way (WaitInbox)

    std::string Utf8(const std::wstring& text)
    {
        if (text.empty()) return std::string();
        int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        std::string out(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size, nullptr, nullptr);
        return out;
    }

    void SendToInbox(const std::wstring& line)
    {
        try
        {
            HttpClient^ client = ref new HttpClient();
            auto op = client->PostAsync(ref new Uri("http://192.168.1.148:8765/posta-bg"),
                ref new HttpStringContent(ref new String(line.c_str())));
            ThreadPoolTimer^ timeout = CancelAfter(op, 4);
            InterlockedIncrement(&g_inboxPending);
            concurrency::create_task(op).then([timeout, client](concurrency::task<HttpResponseMessage^> t)
            {
                timeout->Cancel();
                try { t.get(); } catch (...) { }
                InterlockedDecrement(&g_inboxPending);
            });
        }
        catch (...) { }
    }
}

void WaitInbox(int milliseconds)
{
    HANDLE never = CreateEventEx(nullptr, nullptr, 0, EVENT_ALL_ACCESS);
    ULONGLONG until = GetTickCount64() + milliseconds;
    while (g_inboxPending > 0 && GetTickCount64() < until) WaitForSingleObjectEx(never, 100, FALSE);
    if (never) CloseHandle(never);
}

ThreadPoolTimer^ CancelAfter(IAsyncInfo^ info, int seconds)
{
    TimeSpan delay;
    delay.Duration = seconds * 10000000LL;
    return ThreadPoolTimer::CreateTimer(ref new TimerElapsedHandler([info](ThreadPoolTimer^)
    {
        try { info->Cancel(); } catch (...) { }
    }), delay);
}

long long NowUtc()
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring LocalClock()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t text[16];
    swprintf_s(text, L"%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
    return text;
}

void Log(const std::wstring& text)
{
    std::wstring line = LocalClock() + L" " + text;
    {
        std::lock_guard<std::mutex> lock(g_logLock);
        try
        {
            std::wstring path = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\bg.log";
            HANDLE file = CreateFile2(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_ALWAYS, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                std::string bytes = Utf8(line + L"\r\n");
                DWORD written = 0;
                WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
                CloseHandle(file);
            }
        }
        catch (...) { }
    }
    SendToInbox(line);
}

namespace
{
    const wchar_t* const ToastModes[] = { L"normale", L"notifier App", L"notifier famiglia!App", L"programmato", L"programmato App" };
    const int ToastModeCount = 5;
    std::wstring g_toastKey = L"toastMode";

    ToastNotifier^ Notifier(int mode)
    {
        if (mode == 1 || mode == 4) return ToastNotificationManager::CreateToastNotifier("App");
        if (mode == 2) return ToastNotificationManager::CreateToastNotifier(Windows::ApplicationModel::Package::Current->Id->FamilyName + "!App");
        return ToastNotificationManager::CreateToastNotifier();
    }

    bool TryToast(int mode, XmlDocument^ xml, bool quiet)
    {
        try
        {
            if (mode == 3 || mode == 4)
            {
                Windows::Foundation::DateTime when;
                when.UniversalTime = NowUtc() + 3 * 10000000LL;
                Notifier(mode)->AddToSchedule(ref new ScheduledToastNotification(xml, when));
            }
            else
            {
                Notifier(mode)->Show(ref new ToastNotification(xml));
            }
            return true;
        }
        catch (Exception^ ex)
        {
            if (!quiet) Log(std::wstring(L"toast: ") + ToastModes[mode] + L" errore " + Hex(ex->HResult));
            return false;
        }
    }
}

void SetToastContext(const wchar_t* name)
{
    g_toastKey = std::wstring(L"toastMode-") + name;
}

void Toast(const std::wstring& title, const std::wstring& text, bool silent, const std::wstring& launch)
{
    XmlDocument^ xml;
    try
    {
        xml = ToastNotificationManager::GetTemplateContent(ToastTemplateType::ToastText02);
        XmlNodeList^ texts = xml->GetElementsByTagName("text");
        texts->Item(0)->AppendChild(xml->CreateTextNode(ref new String(title.substr(0, 100).c_str())));
        texts->Item(1)->AppendChild(xml->CreateTextNode(ref new String(text.substr(0, 200).c_str())));
        XmlElement^ toast = safe_cast<XmlElement^>(xml->SelectSingleNode("/toast"));
        if (!launch.empty()) toast->SetAttribute("launch", ref new String(launch.c_str()));
        if (silent)
        {
            XmlElement^ audio = xml->CreateElement("audio");
            audio->SetAttribute("silent", "true");
            toast->AppendChild(audio);
        }
    }
    catch (Exception^ ex)
    {
        Log(L"toast: xml errore " + Hex(ex->HResult));
        return;
    }

    int known = -1;
    try
    {
        auto values = ApplicationData::Current->LocalSettings->Values;
        String^ key = ref new String(g_toastKey.c_str());
        if (values->HasKey(key)) known = safe_cast<int>(values->Lookup(key));
    }
    catch (...) { }
    if (known >= 0 && known < ToastModeCount && TryToast(known, xml, false)) return;
    for (int mode = 0; mode < ToastModeCount; ++mode)
    {
        if (mode == known) continue;
        if (!TryToast(mode, xml, false)) continue;
        try { ApplicationData::Current->LocalSettings->Values->Insert(ref new String(g_toastKey.c_str()), mode); } catch (...) { }
        Log(std::wstring(L"toast: funziona il modo ") + ToastModes[mode]);
        return;
    }
    Log(L"toast: nessun modo funziona");
}

int BatteryPercent()
{
    try { return Windows::Phone::Devices::Power::Battery::GetDefault()->RemainingChargePercent; }
    catch (...) { return -1; }
}

std::wstring NetworkText()
{
    try
    {
        ConnectionProfile^ profile = NetworkInformation::GetInternetConnectionProfile();
        if (profile == nullptr) return L"nessuna";
        std::wstring kind = profile->IsWlanConnectionProfile ? L"wifi" : (profile->IsWwanConnectionProfile ? L"cell" : L"altro");
        if (profile->GetNetworkConnectivityLevel() != NetworkConnectivityLevel::InternetAccess) kind += L" (limitata)";
        return kind;
    }
    catch (...) { return L"?"; }
}

std::wstring MemoryText()
{
    return std::to_wstring(MemoryManager::AppMemoryUsage / 1024) + L" KB / " +
        std::to_wstring(MemoryManager::AppMemoryUsageLimit / 1024) + L" KB";
}

std::wstring Hex(int value)
{
    wchar_t text[16];
    swprintf_s(text, L"0x%08X", static_cast<unsigned int>(value));
    return text;
}

void SetStatus(const std::wstring& state)
{
    try
    {
        auto values = ApplicationData::Current->LocalSettings->Values;
        values->Insert("bgState", ref new String(state.c_str()));
        values->Insert("bgStateAt", NowUtc());
    }
    catch (...) { }
}

void SetTick(unsigned int ticks, long long started, int batteryStart, int battery)
{
    try
    {
        auto values = ApplicationData::Current->LocalSettings->Values;
        values->Insert("bgTicks", ticks);
        values->Insert("bgStarted", started);
        values->Insert("bgLast", NowUtc());
        values->Insert("bgBatteryStart", batteryStart);
        values->Insert("bgBattery", battery);
    }
    catch (...) { }
}

void SetCheck(const std::wstring& result)
{
    try
    {
        auto values = ApplicationData::Current->LocalSettings->Values;
        values->Insert("bgCheck", ref new String(result.c_str()));
        values->Insert("bgCheckedAt", NowUtc());
    }
    catch (...) { }
}
}
