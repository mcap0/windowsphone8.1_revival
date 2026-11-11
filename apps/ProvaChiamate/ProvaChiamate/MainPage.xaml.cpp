#include "pch.h"
#include "MainPage.xaml.h"
#include "Probe.h"
#include "Voip.h"

using namespace ProvaChiamate;
using namespace Platform;
using namespace Windows::Foundation;
using namespace Windows::UI::Core;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::ApplicationModel::Background;
using namespace Windows::Networking::Sockets;
using namespace Windows::System::Threading;

// PLAN §69 step 0 on the Lumia: the old WP8 VoIP stack from a C++/CX Runtime app with ID_CAP_VOIP
// (WindowsPhoneReservedAppInfo.xml), call audio (echo, earpiece) and ControlChannelTrigger. Every line also goes to
// LocalFolder\run.log (tools/phonelog 97530709-48da-4c2f-8088-82c2b703bb67).

namespace
{
    std::wstring Hex(int hr)
    {
        wchar_t b[16];
        swprintf_s(b, L"0x%08X", static_cast<unsigned>(hr));
        return b;
    }
}

MainPage::MainPage()
{
    InitializeComponent();
    Add(L"squillo nativo ora", [this]() { Ring(); });
    Add(L"squillo nativo tra 15 s (blocca il telefono)", [this]()
    {
        Log(L"squillo tra 15 s");
        TimeSpan t;
        t.Duration = 15LL * 10000000;
        ThreadPoolTimer::CreateTimer(ref new TimerElapsedHandler([this](ThreadPoolTimer^)
        {
            Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this]() { Ring(); }));
        }), t);
    });
    Add(L"eco: Communications, altoparlante", [this]() { RunDuplex(3, 2, 6); });
    Add(L"eco: Other, altoparlante", [this]() { RunDuplex(0, 2, 6); });
    Add(L"auricolare: Communications (orecchio)", [this]() { RunDuplex(3, 1, 6); });
    Add(L"canale di controllo", [this]() { TryControlChannel(); });
    Log(L"Prova chiamate 1.0.0.1");
}

void MainPage::Add(String^ label, std::function<void()> action)
{
    auto b = ref new Button();
    b->Content = label;
    b->HorizontalAlignment = Windows::UI::Xaml::HorizontalAlignment::Stretch;
    b->Margin = Thickness(0, 0, 0, -8);
    b->Click += ref new RoutedEventHandler([action](Object^, RoutedEventArgs^) { action(); });
    Buttons->Children->Append(b);
}

void MainPage::Log(const std::wstring& s)
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t ts[32];
    swprintf_s(ts, L"%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::wstring line = ts + s;
    std::wstring path = std::wstring(Windows::Storage::ApplicationData::Current->LocalFolder->Path->Data()) + L"\\run.log";
    HANDLE f = CreateFile2(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, OPEN_ALWAYS, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        std::wstring l = line + L"\r\n";
        int n = WideCharToMultiByte(CP_UTF8, 0, l.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string u(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, l.c_str(), -1, &u[0], n, nullptr, nullptr);
        DWORD w = 0;
        WriteFile(f, u.data(), static_cast<DWORD>(n - 1), &w, nullptr);
        CloseHandle(f);
    }
    String^ text = ref new String(line.c_str());
    Dispatcher->RunAsync(CoreDispatcherPriority::Normal, ref new DispatchedHandler([this, text]()
    {
        LogText->Text = text + L"\n" + LogText->Text;
    }));
}

void MainPage::Ring()
{
    Voip::Events ev;
    std::shared_ptr<Voip::Call*> self = std::make_shared<Voip::Call*>(nullptr);
    ev.answered = [this, self]()
    {
        Log(L"AnswerRequested: rispondo, prova audio 6 s nella chiamata");
        Log(L"NotifyCallActive " + Hex(Voip::NotifyActive(*self)));
        ThreadPool::RunAsync(ref new WorkItemHandler([this, self](IAsyncAction^)
        {
            Probe::Duplex(3, -1, 6, [this](const std::wstring& s) { Log(s); });
            Log(L"NotifyCallEnded " + Hex(Voip::NotifyEnded(*self)));
        }));
    };
    ev.rejected = [this, self](int reason)
    {
        Log(L"RejectRequested, motivo " + std::to_wstring(reason));
        Log(L"NotifyCallEnded " + Hex(Voip::NotifyEnded(*self)));
    };
    ev.endRequested = [this, self]()
    {
        Log(L"EndRequested");
        Log(L"NotifyCallEnded " + Hex(Voip::NotifyEnded(*self)));
    };
    ev.holdRequested = [this]() { Log(L"HoldRequested"); };
    ev.resumeRequested = [this]() { Log(L"ResumeRequested"); };
    String^ dir = Windows::ApplicationModel::Package::Current->InstalledLocation->Path + L"\\";
    struct Variant { const wchar_t* label; Voip::Options o; };
    std::vector<Variant> vs(5);
    vs[0].label = L"A file path, ringtone wav";
    vs[0].o.image = dir + L"Assets\\StoreLogo.scale-240.png"; vs[0].o.ringtone = dir + L"Assets\\ring.wav"; vs[0].o.details = L"chiamata VoIP";
    vs[1].label = L"B ms-appx, ringtone wav";
    vs[1].o.image = L"ms-appx:///Assets/StoreLogo.png"; vs[1].o.ringtone = L"ms-appx:///Assets/ring.wav"; vs[1].o.details = L"chiamata VoIP";
    vs[2].label = L"C no images, ringtone file";
    vs[2].o.ringtone = dir + L"Assets\\ring.wav"; vs[2].o.details = L"x";
    vs[3].label = L"D context /MainPage.xaml";
    vs[3].o.context = L"/MainPage.xaml"; vs[3].o.image = dir + L"Assets\\StoreLogo.scale-240.png"; vs[3].o.ringtone = dir + L"Assets\\ring.wav"; vs[3].o.details = L"x";
    vs[4].label = L"E all null/empty";
    for (auto& v : vs)
    {
        std::wstring err;
        *self = Voip::RequestIncoming(L"Anna Rossi", L"+39 333 0000000", L"VoIP", v.o, ev, err);
        Log(std::wstring(v.label) + (*self ? L": ok, suona? " + err : L": " + err));
        if (*self) break;
    }
}

void MainPage::RunDuplex(int category, int endpoint, int seconds)
{
    Log(L"prova audio: categoria " + std::to_wstring(category) + L", uscita " + std::to_wstring(endpoint));
    ThreadPool::RunAsync(ref new WorkItemHandler([this, category, endpoint, seconds](IAsyncAction^)
    {
        Probe::Duplex(category, endpoint, seconds, [this](const std::wstring& s) { Log(s); });
    }));
}

void MainPage::TryControlChannel()
{
    concurrency::create_task(BackgroundExecutionManager::RequestAccessAsync()).then([this](concurrency::task<BackgroundAccessStatus> t)
    {
        try
        {
            Log(L"RequestAccessAsync: " + std::to_wstring(static_cast<int>(t.get())));
        }
        catch (Exception^ e)
        {
            Log(L"RequestAccessAsync: " + Hex(e->HResult));
        }
        try
        {
            auto cct = ref new ControlChannelTrigger(L"ponte", 15, ControlChannelTriggerResourceType::RequestSoftwareSlot);
            Log(L"ControlChannelTrigger creato, keep-alive " + std::to_wstring(cct->CurrentKeepAliveIntervalInMinutes) + L" min");
            auto push = cct->PushNotificationTrigger;
            Log(push != nullptr ? L"PushNotificationTrigger presente" : L"PushNotificationTrigger nullo");
            delete cct;
        }
        catch (Exception^ e)
        {
            Log(L"ControlChannelTrigger: " + Hex(e->HResult) + L" " + std::wstring(e->Message ? e->Message->Data() : L""));
        }
    }, concurrency::task_continuation_context::use_current());
}
