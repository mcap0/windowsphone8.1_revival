#include "pch.h"
#include "MainPage.xaml.h"

using namespace Avvisi;
using namespace concurrency;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Background;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Media::Playback;
using namespace Windows::Storage;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Navigation;

namespace
{
    long long NowUtc()
    {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        return (static_cast<long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }

    template <typename T> T Value(IPropertySet^ values, String^ key, T fallback)
    {
        try { return values->HasKey(key) ? safe_cast<T>(values->Lookup(key)) : fallback; }
        catch (...) { return fallback; }
    }

    // The last lines of LocalFolder\avvisi.log, newest first.
    std::wstring LogTail(size_t lines)
    {
        std::wstring path = std::wstring(ApplicationData::Current->LocalFolder->Path->Data()) + L"\\avvisi.log";
        HANDLE file = CreateFile2(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, OPEN_EXISTING, nullptr);
        if (file == INVALID_HANDLE_VALUE) return L"(registro vuoto)";
        FILE_STANDARD_INFO info = {};
        GetFileInformationByHandleEx(file, FileStandardInfo, &info, sizeof(info));
        LONGLONG size = info.EndOfFile.QuadPart;
        LONGLONG start = size > 8192 ? size - 8192 : 0;
        LARGE_INTEGER at;
        at.QuadPart = start;
        SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
        std::string bytes(static_cast<size_t>(size - start), '\0');
        DWORD read = 0;
        if (!bytes.empty()) ReadFile(file, &bytes[0], static_cast<DWORD>(bytes.size()), &read, nullptr);
        CloseHandle(file);
        bytes.resize(read);

        int wide = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        std::wstring text(wide, L'\0');
        if (wide > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &text[0], wide);

        std::vector<std::wstring> all;
        size_t from = 0;
        while (from < text.size())
        {
            size_t end = text.find(L'\n', from);
            if (end == std::wstring::npos) end = text.size();
            std::wstring line = text.substr(from, end - from);
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (!line.empty()) all.push_back(line);
            from = end + 1;
        }
        if (start > 0 && !all.empty()) all.erase(all.begin());   // first line may be cut
        std::wstring out;
        for (size_t i = 0; i < lines && i < all.size(); ++i) out += all[all.size() - 1 - i] + L"\n";
        return out;
    }
}

MainPage::MainPage()
{
    InitializeComponent();
    timer_ = ref new DispatcherTimer();
    TimeSpan every;
    every.Duration = 2 * 10000000LL;
    timer_->Interval = every;
    timer_->Tick += ref new EventHandler<Object^>([this](Object^, Object^) { Refresh(); });
    timer_->Start();
}

void MainPage::OnNavigatedTo(NavigationEventArgs^)
{
    Refresh();
    RegisterWatch();
}

void MainPage::Refresh()
{
    IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
    String^ state = Value<String^>(values, "state", "mai avviato");
    unsigned int ticks = Value<unsigned int>(values, "ticks", 0);
    long long started = Value<long long>(values, "started", 0);
    long long last = Value<long long>(values, "last", 0);
    int batteryStart = Value<int>(values, "batteryStart", -1);
    int battery = Value<int>(values, "battery", -1);
    long long now = NowUtc();

    std::wstring text = std::wstring(L"Stato: ") + state->Data();
    if (started != 0)
    {
        long long minutes = (now - started) / 600000000LL;
        long long since = last != 0 ? (now - last) / 600000000LL : -1;
        text += L"\nControlli: " + std::to_wstring(ticks) + (since >= 0 ? L", ultimo " + std::to_wstring(since) + L" min fa" : L"");
        text += L"\nDa " + std::to_wstring(minutes) + L" min, batteria " + std::to_wstring(batteryStart) + L"% \u2192 " +
            std::to_wstring(battery) + L"%";
        long long tickMinutes = (last - started) / 600000000LL;
        if (tickMinutes >= 30 && batteryStart >= battery)
        {
            double perHour = (batteryStart - battery) * 60.0 / static_cast<double>(tickMinutes);
            wchar_t rate[32];
            swprintf_s(rate, L" (%.1f%%/ora, telefono intero)", perHour);
            text += rate;
        }
    }
    StatusText->Text = ref new String(text.c_str());
    LogText->Text = ref new String(LogTail(30).c_str());
}

void MainPage::Send(String^ command)
{
    try
    {
        ValueSet^ message = ref new ValueSet();
        message->Insert("cmd", command);
        BackgroundMediaPlayer::SendMessageToBackground(message);
    }
    catch (Exception^ ex)
    {
        StatusText->Text = "Errore messaggio: " + ex->Message;
    }
}

void MainPage::OnStart(Object^, RoutedEventArgs^)
{
    IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
    String^ state = Value<String^>(values, "state", "");
    try
    {
        // a finished task leaves a dead player behind: shut it down so that Current starts a new KeepAlive
        if (state->Length() >= 5 && std::wstring(state->Data()).compare(0, 5, L"fermo") == 0)
            BackgroundMediaPlayer::Shutdown();
        MediaPlayer^ player = BackgroundMediaPlayer::Current;   // starts KeepAlive if it is not running
        (void)player;
        Send("start");
        RegisterWatch();
    }
    catch (Exception^ ex)
    {
        StatusText->Text = "Errore avvio: " + ex->Message;
    }
}

void MainPage::OnStop(Object^, RoutedEventArgs^)
{
    Send("stop");
}

// Watch every 30 min and when the network comes back. After an app update WP8.1 needs RemoveAccess + RequestAccess,
// otherwise the old registrations never fire.
void MainPage::RegisterWatch()
{
    IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
    PackageVersion v = Package::Current->Id->Version;
    String^ version = v.Major.ToString() + "." + v.Minor.ToString() + "." + v.Build.ToString() + "." + v.Revision.ToString();
    String^ registered = Value<String^>(values, "accessVersion", "");
    bool fresh = registered != version;
    if (fresh)
    {
        BackgroundExecutionManager::RemoveAccess();
        for (auto pair : BackgroundTaskRegistration::AllTasks) pair->Value->Unregister(true);
    }
    create_task(BackgroundExecutionManager::RequestAccessAsync()).then([values, version](BackgroundAccessStatus status)
    {
        if (status == BackgroundAccessStatus::Denied || status == BackgroundAccessStatus::Unspecified) return;
        bool timer = false, network = false;
        for (auto pair : BackgroundTaskRegistration::AllTasks)
        {
            if (pair->Value->Name == "avvisi-timer") timer = true;
            if (pair->Value->Name == "avvisi-rete") network = true;
        }
        if (!timer)
        {
            BackgroundTaskBuilder^ builder = ref new BackgroundTaskBuilder();
            builder->Name = "avvisi-timer";
            builder->TaskEntryPoint = "AvvisiTask.Watch";
            builder->SetTrigger(ref new TimeTrigger(30, false));
            builder->Register();
        }
        if (!network)
        {
            BackgroundTaskBuilder^ builder = ref new BackgroundTaskBuilder();
            builder->Name = "avvisi-rete";
            builder->TaskEntryPoint = "AvvisiTask.Watch";
            builder->SetTrigger(ref new SystemTrigger(SystemTriggerType::InternetAvailable, false));
            builder->Register();
        }
        values->Insert("accessVersion", version);
    }).then([this](task<void> t)
    {
        try { t.get(); }
        catch (Exception^ ex) { StatusText->Text = "Errore registrazione: " + ex->Message; }
    }, task_continuation_context::use_current());
}
