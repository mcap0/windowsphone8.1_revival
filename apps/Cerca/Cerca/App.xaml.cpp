#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace Cerca;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Phone::UI::Input;
using namespace Windows::Storage;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

App::App()
{
    InitializeComponent();
    Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
    UnhandledException += ref new UnhandledExceptionEventHandler(this, &App::OnUnhandled);
    HardwareButtons::BackPressed += ref new EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);
    if (rootFrame == nullptr)
    {
        rootFrame = ref new Frame();
        rootFrame->CacheSize = 2;
        Window::Current->Content = rootFrame;
    }

    if (rootFrame->Content == nullptr)
    {
        if (!rootFrame->Navigate(MainPage::typeid, e->Arguments))
            throw ref new FailureException("Impossibile aprire Cerca");
    }

    Window::Current->Activate();
}

void App::OnSuspending(Object^, SuspendingEventArgs^)
{
}

void App::OnBackPressed(Object^, BackPressedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame != nullptr && frame->CanGoBack)
    {
        frame->GoBack();
        e->Handled = true;
    }
}

void App::OnUnhandled(Object^, UnhandledExceptionEventArgs^ e)
{
    e->Handled = true;
    String^ message = e->Message != nullptr ? e->Message : ref new String(L"errore sconosciuto");
    ApplicationData::Current->LocalSettings->Values->Insert("lastCrash", message);
}
