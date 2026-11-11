#include "pch.h"
#include "App.xaml.h"
#include "MainPage.xaml.h"

using namespace Remoto;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;

App::App()
{
    InitializeComponent();
    // The connection ends with the app in the background and starts again on return (MainPage).
    Suspending += ref new SuspendingEventHandler([](Platform::Object^, Windows::ApplicationModel::SuspendingEventArgs^)
    {
        if (auto frame = dynamic_cast<Frame^>(Window::Current->Content))
            if (auto page = dynamic_cast<MainPage^>(frame->Content))
                page->Suspend();
    });
    Resuming += ref new Windows::Foundation::EventHandler<Platform::Object^>([](Platform::Object^, Platform::Object^)
    {
        if (auto frame = dynamic_cast<Frame^>(Window::Current->Content))
            if (auto page = dynamic_cast<MainPage^>(frame->Content))
                page->Resume();
    });
}

void App::OnLaunched(Windows::ApplicationModel::Activation::LaunchActivatedEventArgs^)
{
    auto frame = dynamic_cast<Frame^>(Window::Current->Content);
    if (frame == nullptr)
    {
        frame = ref new Frame();
        Window::Current->Content = frame;
    }
    if (frame->Content == nullptr)
        frame->Navigate(MainPage::typeid);
    Window::Current->Activate();
}
