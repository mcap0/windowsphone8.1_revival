#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"
#include "LoginPage.xaml.h"
#include "ChatPage.xaml.h"
#include "ImagePage.xaml.h"
#include "FilePage.xaml.h"
#include "TgService.h"

using namespace Telegram;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Phone::UI::Input;

namespace
{
    // TelegramTask.Watch checks for new messages every 15 minutes (the WP8.1 minimum) and when the internet comes back,
    // also with the phone locked. After an app update WP8.1 needs RemoveAccess + RequestAccess, otherwise the old
    // registrations never fire again.
    void RegisterBackground()
    {
        using namespace Windows::ApplicationModel::Background;
        using namespace Windows::Foundation::Collections;
        using namespace Windows::Storage;
        IPropertySet^ values = ApplicationData::Current->LocalSettings->Values;
        PackageVersion v = Package::Current->Id->Version;
        String^ version = v.Major.ToString() + "." + v.Minor.ToString() + "." + v.Build.ToString() + "." + v.Revision.ToString();
        String^ registered = values->HasKey("bgVersion") ? dynamic_cast<String^>(values->Lookup("bgVersion")) : nullptr;
        if (registered != version)
        {
            BackgroundExecutionManager::RemoveAccess();
            for (auto pair : BackgroundTaskRegistration::AllTasks) pair->Value->Unregister(true);
        }
        concurrency::create_task(BackgroundExecutionManager::RequestAccessAsync()).then([values, version](BackgroundAccessStatus status)
        {
            TgService::Get().Log("background access " + std::to_string(static_cast<int>(status)));
            if (status == BackgroundAccessStatus::Denied || status == BackgroundAccessStatus::Unspecified) return;
            bool timer = false, network = false;
            for (auto pair : BackgroundTaskRegistration::AllTasks)
            {
                if (pair->Value->Name == "tg-timer") timer = true;
                if (pair->Value->Name == "tg-rete") network = true;
            }
            if (!timer)
            {
                BackgroundTaskBuilder^ builder = ref new BackgroundTaskBuilder();
                builder->Name = "tg-timer";
                builder->TaskEntryPoint = "TelegramTask.Watch";
                builder->SetTrigger(ref new TimeTrigger(15, false));
                builder->Register();
            }
            if (!network)
            {
                BackgroundTaskBuilder^ builder = ref new BackgroundTaskBuilder();
                builder->Name = "tg-rete";
                builder->TaskEntryPoint = "TelegramTask.Watch";
                builder->SetTrigger(ref new SystemTrigger(SystemTriggerType::InternetAvailable, false));
                builder->Register();
            }
            values->Insert("bgVersion", version);
            TgService::Get().Log(std::string("background tasks ") + (timer && network ? "already registered" : "registered"));
        }).then([](concurrency::task<void> t)
        {
            try { t.get(); }
            catch (Exception^ ex) { TgService::Get().Log("background registration error " + std::to_string(ex->HResult)); }
        });
    }
}

App::App()
{
    InitializeComponent();
    Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
    Resuming += ref new EventHandler<Object^>(this, &App::OnResuming);
    UnhandledException += ref new UnhandledExceptionEventHandler(this, &App::OnUnhandled);
    HardwareButtons::BackPressed += ref new EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
}

Frame^ App::EnsureFrame()
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        frame->CacheSize = 2;
        Window::Current->Content = frame;
        TgService::Get().Init(Window::Current->Dispatcher);
        if (TgService::Get().Engine().authorized())
            frame->Navigate(MainPage::typeid);
        else
            frame->Navigate(LoginPage::typeid);
    }
    return frame;
}

void App::OpenFromArguments(String^ args)
{
    // Toast launches may provide either the argument alone or the shell's full app://... URI.
    std::string a = FromP(args);
    std::string key;
    if (a.compare(0, 3, "tg:") == 0)
    {
        key = a.substr(3);
    }
    else
    {
        const size_t at = a.find("chat=");
        if (at == std::string::npos) return;
        const size_t start = at + 5;
        const size_t end = a.find_first_of("&#", start);
        key = a.substr(start, end == std::string::npos ? std::string::npos : end - start);
    }
    if (key.size() < 2 || !TgService::Get().Engine().authorized()) return;
    if ((key[0] != 'u' && key[0] != 'c' && key[0] != 'h') ||
        !std::all_of(key.begin() + 1, key.end(), [](char c) { return c >= '0' && c <= '9'; })) return;
    auto frame = EnsureFrame();
    auto open = dynamic_cast<ChatPage^>(frame->Content);
    if (open != nullptr && TgService::Get().openChat == key) return;
    frame->Navigate(ChatPage::typeid, PStr(key));
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    EnsureFrame();
    if (e->Arguments != nullptr && !e->Arguments->IsEmpty()) OpenFromArguments(e->Arguments);
    Window::Current->Activate();
    RegisterBackground();
}

void App::OnActivated(IActivatedEventArgs^ e)
{
    auto frame = EnsureFrame();
    auto picker = dynamic_cast<FileOpenPickerContinuationEventArgs^>(e);
    if (picker != nullptr)
    {
        auto page = dynamic_cast<ChatPage^>(frame->Content);
        if (page != nullptr) page->ContinueFileOpenPicker(picker);
    }
    Window::Current->Activate();
}

void App::OnSuspending(Object^, SuspendingEventArgs^ e)
{
    auto deferral = e->SuspendingOperation->GetDeferral();
    TgService::Get().Suspend();
    deferral->Complete();
}

void App::OnResuming(Object^, Object^)
{
    TgService::Get().Resume();
}

void App::OnBackPressed(Object^, BackPressedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr) return;
    auto chat = dynamic_cast<ChatPage^>(frame->Content);
    if (chat != nullptr && chat->HandleBack())
    {
        e->Handled = true;
        return;
    }
    auto files = dynamic_cast<FilePage^>(frame->Content);
    if (files != nullptr && files->HandleBack())
    {
        e->Handled = true;
        return;
    }
    if (frame->CanGoBack)
    {
        frame->GoBack();
        e->Handled = true;
    }
}

void App::OnUnhandled(Object^, UnhandledExceptionEventArgs^ e)
{
    e->Handled = true;
    TgService::Get().Log("UNHANDLED: " + FromP(e->Message));
}
