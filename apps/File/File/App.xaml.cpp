#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"
#include "EditorPage.xaml.h"

using namespace File;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::Storage;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Phone::UI::Input;

App::App()
{
    InitializeComponent();
    Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
    Resuming += ref new EventHandler<Object^>(this, &App::OnResuming);
    UnhandledException += ref new UnhandledExceptionEventHandler(this, &App::OnUnhandled);
    HardwareButtons::BackPressed += ref new EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
}

void App::OnLaunched(LaunchActivatedEventArgs^ e)
{
    auto rootFrame = dynamic_cast<Frame^>(Window::Current->Content);
    if (rootFrame == nullptr)
    {
        rootFrame = ref new Frame();
        rootFrame->CacheSize = 3;
        Window::Current->Content = rootFrame;
    }

    if (rootFrame->Content == nullptr)
    {
        if (!rootFrame->Navigate(MainPage::typeid, e->Arguments))
            throw ref new FailureException(L"Impossibile avviare File");
    }

    Window::Current->Activate();
}

void App::OnSuspending(Object^, SuspendingEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame != nullptr)
    {
        auto page = dynamic_cast<MainPage^>(frame->Content);
        if (page != nullptr)
            page->SaveView();
    }
}

void App::OnResuming(Object^, Object^)
{
}

void App::OnBackPressed(Object^, BackPressedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
        return;

    auto mainPage = dynamic_cast<MainPage^>(frame->Content);
    if (mainPage != nullptr && mainPage->HandleHardwareBack())
    {
        e->Handled = true;
        return;
    }

    auto editorPage = dynamic_cast<EditorPage^>(frame->Content);
    if (editorPage != nullptr && editorPage->HandleHardwareBack())
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
    try
    {
        String^ message = e->Message != nullptr ? e->Message : ref new String(L"errore sconosciuto");
        ApplicationData::Current->LocalSettings->Values->Insert("lastCrash", message);
    }
    catch (...)
    {
    }
}
