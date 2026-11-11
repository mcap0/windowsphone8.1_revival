#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace Registratore;
using namespace Platform;
using namespace Windows::ApplicationModel;
using namespace Windows::ApplicationModel::Activation;
using namespace Windows::Foundation;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::Phone::UI::Input;

App::App()
{
    InitializeComponent();
    Suspending += ref new SuspendingEventHandler(this, &App::OnSuspending);
    HardwareButtons::BackPressed += ref new EventHandler<BackPressedEventArgs^>(this, &App::OnBackPressed);
}

void App::OnLaunched(LaunchActivatedEventArgs^)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        Window::Current->Content = frame;
        frame->Navigate(MainPage::typeid);
    }
    Window::Current->Activate();
}

void App::OnSuspending(Object^, SuspendingEventArgs^ e)
{
    // a recording in progress is finished and kept when the app goes to the background
    auto deferral = e->SuspendingOperation->GetDeferral();
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    auto page = frame == nullptr ? nullptr : dynamic_cast<MainPage^>(frame->Content);
    if (page != nullptr) page->StopForSuspend();
    deferral->Complete();
}

void App::OnBackPressed(Object^, BackPressedEventArgs^ e)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    auto page = frame == nullptr ? nullptr : dynamic_cast<MainPage^>(frame->Content);
    if (page != nullptr && page->HandleBack()) e->Handled = true;
}
